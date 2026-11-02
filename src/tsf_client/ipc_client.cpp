#include "ipc_client.h"
#include "globals.h"
#include "../common/ime_guids.h"
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
        IpcKeyResponse resp{};
        SendRequest(req, resp, 25);
    }
}

void IpcClient::Rollback(uint64_t sessionId, uint64_t contextId) {
    if (m_hPipe != INVALID_HANDLE_VALUE) {
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::Rollback);
        req.session_id = sessionId;
        req.context_id = contextId;
        IpcKeyResponse resp{};
        SendRequest(req, resp, 25);
    }
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

    // 3. Strict response identity & type matching
    if (op->recvBuf.msg_type != req.msg_type ||
        op->recvBuf.session_id != req.session_id ||
        op->recvBuf.context_id != req.context_id) {
        Disconnect();
        op->Release();
        return false;
    }

    // Defensive buffer sanitation
    op->recvBuf.composition[sizeof(op->recvBuf.composition)/sizeof(wchar_t) - 1] = L'\0';
    op->recvBuf.commit_text[sizeof(op->recvBuf.commit_text)/sizeof(wchar_t) - 1] = L'\0';
    if (op->recvBuf.candidate_count > 5) {
        op->recvBuf.candidate_count = 5;
    }
    for (uint32_t i = 0; i < op->recvBuf.candidate_count; ++i) {
        op->recvBuf.candidates[i][sizeof(op->recvBuf.candidates[i])/sizeof(wchar_t) - 1] = L'\0';
    }

    resp = op->recvBuf;
    op->inProgress.store(false);
    op->Release();
    return true;
}
