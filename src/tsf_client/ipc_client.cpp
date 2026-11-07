#include "ipc_client.h"
#include "globals.h"
#include "../common/ime_guids.h"
#include "../common/plugin_ipc_codec.h"
#include <algorithm>
#include <new>

// Global lock-free intrusive orphan queue for extreme resource-exhaustion fallback
static std::atomic<IpcClient::AsyncIoOperation*> s_orphanHead{nullptr};
static std::atomic<size_t> s_orphanCount{0};

IpcClient::AsyncIoOperation::AsyncIoOperation() {
    hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ov.hEvent = hEvent;
}

IpcClient::AsyncIoOperation::~AsyncIoOperation() {
    if (pipeToClose != INVALID_HANDLE_VALUE) {
        CloseHandle(pipeToClose);
        pipeToClose = INVALID_HANDLE_VALUE;
    }
    if (hEvent) {
        CloseHandle(hEvent);
        hEvent = nullptr;
    }
}

DWORD WINAPI IpcClient::CleanupThreadProc(LPVOID lpParam) {
    auto* op = static_cast<AsyncIoOperation*>(lpParam);
    if (!op) return 0;

    // 1. Wait indefinitely for driver cancellation to complete
    WaitForSingleObject(op->hEvent, INFINITE);
    DWORD transferred = 0;
    if (op->pipeToClose != INVALID_HANDLE_VALUE) {
        GetOverlappedResult(op->pipeToClose, &op->ov, &transferred, FALSE);
        CloseHandle(op->pipeToClose);
        op->pipeToClose = INVALID_HANDLE_VALUE;
    }
    op->inProgress.store(false);

    HMODULE hModule = op->hModule;

    // 2. Destruct all C++ objects while DLL is fully mapped
    op->Release();

    // 3. Decrement COM reference count
    DllRelease();

    // 4. Atomically free module loader reference and terminate thread.
    // This call is in kernel32.dll (never unloaded) and does not return to module code!
    if (hModule) {
        FreeLibraryAndExitThread(hModule, 0);
    }
    return 0;
}

void IpcClient::DispatchBackgroundCleanup(AsyncIoOperation* op, HANDLE pipeToClose) {
    if (!op) return;

    op->pipeToClose = pipeToClose;

    HMODULE hModule = nullptr;
    BOOL okMod = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&IpcClient::CleanupThreadProc),
        &hModule
    );
    op->hModule = okMod ? hModule : nullptr;

    op->AddRef();
    DllAddRef();

    HANDLE hThread = nullptr;
    if (okMod && !s_faultInjectCreateThread) {
        hThread = CreateThread(nullptr, 0, CleanupThreadProc, op, 0, nullptr);
    }

    if (hThread) {
        CloseHandle(hThread);
    } else {
        // Fallback: OS thread creation failed.
        // DO NOT call FreeLibrary or DllRelease: retain module and COM protections so the DLL
        // and code pages CANNOT be unmapped while in-flight kernel I/O is pending.
        // DO NOT block host thread with WaitForSingleObject(INFINITE).
        // Enqueue into lock-free intrusive queue with zero memory allocation:
        s_hitFallbackEnqueue.fetch_add(1, std::memory_order_relaxed);
        s_orphanCount.fetch_add(1, std::memory_order_relaxed);
        AsyncIoOperation* oldHead = s_orphanHead.load(std::memory_order_relaxed);
        do {
            op->nextOrphan.store(oldHead, std::memory_order_relaxed);
        } while (!s_orphanHead.compare_exchange_weak(oldHead, op, std::memory_order_release, std::memory_order_relaxed));
    }
}

void IpcClient::DrainOrphanQueue() {
    AsyncIoOperation* head = s_orphanHead.exchange(nullptr, std::memory_order_acquire);
    if (!head) return;

    AsyncIoOperation* curr = head;
    AsyncIoOperation* remainingHead = nullptr;
    AsyncIoOperation* remainingTail = nullptr;

    while (curr) {
        AsyncIoOperation* next = curr->nextOrphan.load(std::memory_order_relaxed);
        DWORD waitRes = WaitForSingleObject(curr->hEvent, 0);
        if (waitRes == WAIT_OBJECT_0) {
            s_hitDrainCompleted.fetch_add(1, std::memory_order_relaxed);
            DWORD transferred = 0;
            if (curr->pipeToClose != INVALID_HANDLE_VALUE) {
                GetOverlappedResult(curr->pipeToClose, &curr->ov, &transferred, FALSE);
                CloseHandle(curr->pipeToClose);
                curr->pipeToClose = INVALID_HANDLE_VALUE;
            }
            curr->inProgress.store(false);
            HMODULE hMod = curr->hModule;
            curr->Release();
            DllRelease();
            if (hMod) {
                FreeLibrary(hMod);
            }
            s_orphanCount.fetch_sub(1, std::memory_order_relaxed);
        } else {
            s_hitDrainPendingRemaining.fetch_add(1, std::memory_order_relaxed);
            // Still in progress, keep in remaining queue
            if (!remainingHead) {
                remainingHead = curr;
                remainingTail = curr;
                curr->nextOrphan.store(nullptr, std::memory_order_relaxed);
            } else {
                curr->nextOrphan.store(remainingHead, std::memory_order_relaxed);
                remainingHead = curr;
                // remainingTail remains fixed at the end of this private list
            }
        }
        curr = next;
    }

    if (remainingHead) {
        // remainingTail is fixed and private to this thread.
        // CAS retry updates only remainingTail->nextOrphan and NEVER traverses global/shared nodes.
        AsyncIoOperation* oldHead = s_orphanHead.load(std::memory_order_relaxed);
        if (s_forceCasRetryOnce.exchange(false, std::memory_order_relaxed)) {
            // The private list cannot already be the published head. Exercise
            // an actual failed CAS rather than incrementing a coverage counter.
            AsyncIoOperation* mismatchedExpected = remainingHead;
            if (!s_orphanHead.compare_exchange_strong(mismatchedExpected, remainingHead,
                                                      std::memory_order_release,
                                                      std::memory_order_relaxed)) {
                s_hitCasRetry.fetch_add(1, std::memory_order_relaxed);
                oldHead = mismatchedExpected;
            }
        }
        do {
            remainingTail->nextOrphan.store(oldHead, std::memory_order_relaxed);
            if (!s_orphanHead.compare_exchange_weak(oldHead, remainingHead,
                                                    std::memory_order_release,
                                                    std::memory_order_relaxed)) {
                s_hitCasRetry.fetch_add(1, std::memory_order_relaxed);
            } else {
                break;
            }
        } while (true);
    }
}

size_t IpcClient::GetOrphanCount() {
    return s_orphanCount.load(std::memory_order_relaxed);
}

void IpcClient::ResetBranchCounters() {
    s_hitFallbackEnqueue.store(0);
    s_hitCasRetry.store(0);
    s_hitDrainCompleted.store(0);
    s_hitDrainPendingRemaining.store(0);
}

void IpcClient::EnqueueOrphanForTest(AsyncIoOperation* op) {
    if (!op) return;
    op->AddRef();
    DllAddRef();
    s_hitFallbackEnqueue.fetch_add(1, std::memory_order_relaxed);
    s_orphanCount.fetch_add(1, std::memory_order_relaxed);
    AsyncIoOperation* oldHead = s_orphanHead.load(std::memory_order_relaxed);
    do {
        op->nextOrphan.store(oldHead, std::memory_order_relaxed);
    } while (!s_orphanHead.compare_exchange_weak(oldHead, op, std::memory_order_release, std::memory_order_relaxed));
}

IpcClient::IpcClient() {}

IpcClient::~IpcClient() {
    Disconnect();
}

bool IpcClient::EnsureConnected(ULONGLONG deadline) {
    if (m_hPipe != INVALID_HANDLE_VALUE) {
        return true;
    }

    const wchar_t* pipeName = s_testPipeName ? s_testPipeName : IME_PIPE_NAME;

    while (true) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            return false;
        }

        HANDLE hPipe = CreateFileW(
            pipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr
        );

        if (hPipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr)) {
                CloseHandle(hPipe);
                return false;
            }
            m_hPipe = hPipe;
            return true;
        }

        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            now = GetTickCount64();
            if (now >= deadline) return false;
            DWORD remaining = static_cast<DWORD>(deadline - now);
            DWORD waitTime = (std::min)(remaining, static_cast<DWORD>(15));
            WaitNamedPipeW(pipeName, waitTime);
            continue;
        }

        return false;
    }
}

void IpcClient::Disconnect() {
    if (m_hPipe != INVALID_HANDLE_VALUE) {
        CancelIo(m_hPipe);
        CloseHandle(m_hPipe);
        m_hPipe = INVALID_HANDLE_VALUE;
    }
}

void IpcClient::ResetContext(uint64_t sessionId, uint64_t contextId) {
    if (m_hPipe != INVALID_HANDLE_VALUE) {
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::Reset);
        req.session_id = sessionId;
        req.context_id = contextId;
        req.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
        IpcKeyResponse resp{};
        SendRequest(req, resp, 25);
    }
}

bool IpcClient::Rollback(uint64_t sessionId, uint64_t contextId, IpcKeyResponse* outResp) {
    if (m_hPipe != INVALID_HANDLE_VALUE) {
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::Rollback);
        req.session_id = sessionId;
        req.context_id = contextId;
        req.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
        IpcKeyResponse resp{};
        bool ok = SendRequest(req, resp, 25);
        if (outResp) *outResp = resp;
        return ok && (resp.handled == 1);
    }
    return false;
}

bool IpcClient::NotifyCommitSuccess(uint64_t sessionId, uint64_t contextId, uint64_t eventId) {
    if (eventId == 0) return false;
    IpcKeyRequest req{};
    req.msg_type = static_cast<uint32_t>(IpcMsgType::CommitSuccess);
    req.session_id = sessionId;
    req.context_id = contextId;
    req.event_id = eventId;
    req.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
    IpcKeyResponse resp{};
    return SendRequest(req, resp, 50) && (resp.handled == 1);
}

bool IpcClient::NotifyCommitCancel(uint64_t sessionId, uint64_t contextId, uint64_t eventId, IpcKeyResponse* outResp) {
    if (eventId == 0) return false;
    IpcKeyRequest req{};
    req.msg_type = static_cast<uint32_t>(IpcMsgType::CommitCancel);
    req.session_id = sessionId;
    req.context_id = contextId;
    req.event_id = eventId;
    req.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
    IpcKeyResponse resp{};
    bool ok = SendRequest(req, resp, 50);
    if (outResp) *outResp = resp;
    return ok && (resp.handled == 1);
}

bool IpcClient::SetMode(uint64_t sessionId, uint64_t contextId, uint32_t modeFlags, uint32_t protocolVersion, uint32_t updateMask) {
    IpcKeyRequest req{};
    req.msg_type = static_cast<uint32_t>(IpcMsgType::SetMode);
    req.session_id = sessionId;
    req.context_id = contextId;
    req.mode_arg = modeFlags;
    req.protocol_version = protocolVersion;
    req.update_mask = updateMask;
    IpcKeyResponse resp{};
    return SendRequest(req, resp, 80) && (resp.handled == 1);
}

bool IpcClient::GetMode(uint64_t sessionId, uint64_t contextId, uint32_t& outModeFlags, ime::InputScheme& outScheme) {
    IpcKeyRequest req{};
    req.msg_type = static_cast<uint32_t>(IpcMsgType::GetMode);
    req.session_id = sessionId;
    req.context_id = contextId;
    req.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
    IpcKeyResponse resp{};
    if (SendRequest(req, resp, 80) && resp.handled == 1) {
        outModeFlags = resp.status_flags;
        uint32_t s = (resp.status_flags & IPC_SCHEME_MASK) >> IPC_SCHEME_SHIFT;
        IpcSchemeIdToInputScheme(s, outScheme);
        return true;
    }
    return false;
}

bool IpcClient::SetScheme(uint64_t sessionId, uint64_t contextId, ime::InputScheme scheme) {
    IpcKeyRequest req{};
    req.msg_type = static_cast<uint32_t>(IpcMsgType::SetScheme);
    req.session_id = sessionId;
    req.context_id = contextId;
    IpcSchemeId wireId = InputSchemeToIpcSchemeId(scheme);
    req.scheme_id = static_cast<uint32_t>(wireId);
    req.mode_arg = req.scheme_id;
    req.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
    IpcKeyResponse resp{};
    if (SendRequest(req, resp, 80)) {
        return (resp.handled == 1);
    }
    return false;
}

bool IpcClient::GetScheme(uint64_t sessionId, uint64_t contextId, ime::InputScheme& outScheme) {
    IpcKeyRequest req{};
    req.msg_type = static_cast<uint32_t>(IpcMsgType::GetScheme);
    req.session_id = sessionId;
    req.context_id = contextId;
    req.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
    IpcKeyResponse resp{};
    if (SendRequest(req, resp, 80) && resp.handled == 1) {
        uint32_t s = (resp.status_flags & IPC_SCHEME_MASK) >> IPC_SCHEME_SHIFT;
        return IpcSchemeIdToInputScheme(s, outScheme);
    }
    return false;
}

bool IpcClient::OpenManager(uint64_t sessionId, uint64_t contextId) {
    IpcKeyRequest req{};
    req.msg_type = static_cast<uint32_t>(IpcMsgType::OpenManager);
    req.session_id = sessionId;
    req.context_id = contextId;
    req.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
    IpcKeyResponse resp{};
    return SendRequest(req, resp, 1000) && (resp.handled == 1);
}

bool IpcClient::ExecutePlugin(uint64_t sessionId, uint64_t contextId, uint32_t pluginId, uint32_t* outExecStatus) {
    IpcKeyRequest req{};
    req.msg_type = static_cast<uint32_t>(IpcMsgType::ExecutePlugin);
    req.session_id = sessionId;
    req.context_id = contextId;
    req.vk_code = pluginId;
    req.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
    IpcKeyResponse resp{};
    if (!SendRequest(req, resp, 200)) {
        if (outExecStatus) *outExecStatus = static_cast<uint32_t>(ime::PluginExecStatus::LaunchFailed);
        return false;
    }
    if (outExecStatus) *outExecStatus = resp.page_index;
    return (resp.handled == 1);
}

bool IpcClient::GetPlugins(uint64_t sessionId, uint64_t contextId, std::vector<CandidatePluginItem>& outPlugins,
                           bool* outTruncated, uint32_t* outRevision) {
    IpcKeyRequest req{};
    req.msg_type = static_cast<uint32_t>(IpcMsgType::GetPlugins);
    req.session_id = sessionId;
    req.context_id = contextId;
    req.protocol_version = IPC_PROTOCOL_VERSION_STAGE4;
    IpcKeyResponse resp{};
    if (!SendRequest(req, resp, 30) || resp.handled != 1) {
        return false;
    }

    std::vector<ime::DecodedPluginItem> decoded;
    if (!ime::DecodePluginsFromIpcResponse(resp, decoded, outTruncated, outRevision)) {
        return false;
    }

    outPlugins.clear();
    outPlugins.reserve(decoded.size());
    for (auto& d : decoded) {
        CandidatePluginItem item{};
        item.id = d.id;
        item.name = std::move(d.name);
        item.icon = std::move(d.icon);
        item.exec_path = std::move(d.exec_path);
        item.arguments = std::move(d.arguments);
        outPlugins.push_back(std::move(item));
    }
    return true;
}

bool IpcClient::SendRequest(const IpcKeyRequest& req, IpcKeyResponse& resp, DWORD totalBudgetMs) {
    DrainOrphanQueue();

    const ULONGLONG startTime = GetTickCount64();
    const ULONGLONG deadline = startTime + totalBudgetMs;

    auto GetRemainingMs = [deadline]() -> DWORD {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) return 0;
        return static_cast<DWORD>(deadline - now);
    };

    if (!EnsureConnected(deadline)) {
        return false;
    }

    auto* op = new (std::nothrow) AsyncIoOperation();
    if (!op || !op->hEvent) {
        if (op) op->Release();
        Disconnect();
        return false;
    }
    op->sendBuf = req;
    if (op->sendBuf.protocol_version == 0) {
        op->sendBuf.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
    }
    op->inProgress.store(true);

    // 1. Write Phase
    DWORD writeRemaining = GetRemainingMs();
    if (writeRemaining == 0) {
        Disconnect();
        op->Release();
        return false;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(m_hPipe, &op->sendBuf, sizeof(op->sendBuf), &written, &op->ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            DWORD waitRes = WaitForSingleObject(op->hEvent, writeRemaining);
            if (waitRes != WAIT_OBJECT_0) {
                // Cancel pending I/O and hand over completion tracking to background task
                CancelIoEx(m_hPipe, &op->ov);
                HANDLE pipeToClose = m_hPipe;
                m_hPipe = INVALID_HANDLE_VALUE;
                DispatchBackgroundCleanup(op, pipeToClose);
                op->Release();
                return false;
            }
            if (!GetOverlappedResult(m_hPipe, &op->ov, &written, FALSE)) {
                Disconnect();
                op->Release();
                return false;
            }
        } else {
            Disconnect();
            op->Release();
            return false;
        }
    }

    if (written != sizeof(op->sendBuf)) {
        Disconnect();
        op->Release();
        return false;
    }

    // 2. Read Phase
    DWORD readRemaining = GetRemainingMs();
    if (readRemaining == 0) {
        Disconnect();
        op->Release();
        return false;
    }

    // Reset event for read operation
    ResetEvent(op->hEvent);
    op->ov = {};
    op->ov.hEvent = op->hEvent;

    DWORD bytesRead = 0;
    ok = ReadFile(m_hPipe, &op->recvBuf, sizeof(op->recvBuf), &bytesRead, &op->ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            DWORD waitRes = WaitForSingleObject(op->hEvent, readRemaining);
            if (waitRes != WAIT_OBJECT_0) {
                // Cancel pending read and hand over completion tracking to background task
                CancelIoEx(m_hPipe, &op->ov);
                HANDLE pipeToClose = m_hPipe;
                m_hPipe = INVALID_HANDLE_VALUE;
                DispatchBackgroundCleanup(op, pipeToClose);
                op->Release();
                return false;
            }
            if (!GetOverlappedResult(m_hPipe, &op->ov, &bytesRead, FALSE)) {
                Disconnect();
                op->Release();
                return false;
            }
        } else {
            Disconnect();
            op->Release();
            return false;
        }
    }

    if (bytesRead != sizeof(op->recvBuf)) {
        Disconnect();
        op->Release();
        return false;
    }

    // 3. Strict response identity, protocol version & type matching
    if (op->recvBuf.protocol_version != op->sendBuf.protocol_version ||
        op->recvBuf.msg_type != req.msg_type ||
        op->recvBuf.session_id != req.session_id ||
        op->recvBuf.context_id != req.context_id) {
        Disconnect();
        op->Release();
        return false;
    }

    // Defensive buffer sanitation
    op->recvBuf.composition[sizeof(op->recvBuf.composition)/sizeof(wchar_t) - 1] = L'\0';
    op->recvBuf.commit_text[sizeof(op->recvBuf.commit_text)/sizeof(wchar_t) - 1] = L'\0';
    if (op->recvBuf.candidate_count > 9) {
        op->recvBuf.candidate_count = 9;
    }
    for (uint32_t i = 0; i < op->recvBuf.candidate_count; ++i) {
        op->recvBuf.candidates[i][sizeof(op->recvBuf.candidates[i])/sizeof(wchar_t) - 1] = L'\0';
    }

    resp = op->recvBuf;
    op->inProgress.store(false);
    op->Release();
    return true;
}
