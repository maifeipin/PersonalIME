#pragma once
#include <windows.h>
#include <memory>
#include <atomic>
#include "../common/ipc_protocol.h"
#include "../core/types.h"

// Non-singleton, per-TIP instance IPC client
class IpcClient {
public:
    IpcClient();
    ~IpcClient();

    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;

    bool SendRequest(const IpcKeyRequest& req, IpcKeyResponse& resp, DWORD totalBudgetMs = 50);
    void ResetContext(uint64_t sessionId, uint64_t contextId);
    bool Rollback(uint64_t sessionId, uint64_t contextId, IpcKeyResponse* outResp = nullptr);
    bool NotifyCommitSuccess(uint64_t sessionId, uint64_t contextId, uint64_t eventId);
    bool NotifyCommitCancel(uint64_t sessionId, uint64_t contextId, uint64_t eventId, IpcKeyResponse* outResp = nullptr);
    bool SetMode(uint64_t sessionId, uint64_t contextId, uint32_t modeFlags, uint32_t protocolVersion = IPC_PROTOCOL_VERSION_STAGE3, uint32_t updateMask = 0);
    bool GetMode(uint64_t sessionId, uint64_t contextId, uint32_t& outModeFlags, ime::InputScheme& outScheme);
    bool SetScheme(uint64_t sessionId, uint64_t contextId, ime::InputScheme scheme);
    bool GetScheme(uint64_t sessionId, uint64_t contextId, ime::InputScheme& outScheme);
    void Disconnect();

    struct AsyncIoOperation {
        OVERLAPPED ov{};
        HANDLE hEvent = nullptr;
        IpcKeyRequest sendBuf{};
        IpcKeyResponse recvBuf{};
        std::atomic<bool> inProgress{false};

        // Embedded recovery resources - zero allocation at cancellation time
        HANDLE pipeToClose = INVALID_HANDLE_VALUE;
        HMODULE hModule = nullptr;
        std::atomic<AsyncIoOperation*> nextOrphan{nullptr};
        LONG refCount = 1;

        AsyncIoOperation();
        ~AsyncIoOperation();

        void AddRef() { InterlockedIncrement(&refCount); }
        void Release() {
            if (InterlockedDecrement(&refCount) == 0) {
                delete this;
            }
        }
    };

    // Public test hooks for fault injection verification
    static void DrainOrphanQueue();
    static size_t GetOrphanCount();
    static void SetFaultInjectCreateThread(bool inject) { s_faultInjectCreateThread = inject; }
    static void SetPipeNameForTest(const wchar_t* name) { s_testPipeName = name; }
    static void ResetBranchCounters();
    static void EnqueueOrphanForTest(AsyncIoOperation* op);
    static void SetForceCasRetryOnce(bool force) { s_forceCasRetryOnce = force; }

    // Branch hit counters for verification
    static inline std::atomic<int> s_hitFallbackEnqueue{0};
    static inline std::atomic<int> s_hitCasRetry{0};
    static inline std::atomic<int> s_hitDrainCompleted{0};
    static inline std::atomic<int> s_hitDrainPendingRemaining{0};

private:
    static inline bool s_faultInjectCreateThread = false;
    static inline std::atomic<bool> s_forceCasRetryOnce{false};
    static inline const wchar_t* s_testPipeName = nullptr;

    static DWORD WINAPI CleanupThreadProc(LPVOID lpParam);
    static void DispatchBackgroundCleanup(AsyncIoOperation* op, HANDLE pipeToClose);

    bool EnsureConnected(ULONGLONG deadline);

    HANDLE m_hPipe = INVALID_HANDLE_VALUE;
};