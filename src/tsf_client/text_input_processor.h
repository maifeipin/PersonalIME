#pragma once
#include <windows.h>
#include <msctf.h>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include "candidate_window.h"
#include "ipc_client.h"

enum class CompositionState {
    None,
    Composing,        // Active composition with in-progress input string (can be updated or canceled)
    CommitPendingEnd  // Text was written to document, but EndComposition failed
};

class CTextInputProcessor : 
    public ITfTextInputProcessor,
    public ITfKeyEventSink,
    public ITfCompositionSink {
public:
    CTextInputProcessor();
    ~CTextInputProcessor();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfTextInputProcessor
    STDMETHODIMP Activate(ITfThreadMgr* ptim, TfClientId tid) override;
    STDMETHODIMP Deactivate() override;

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL fForeground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* pic, REFGUID rguid, BOOL* pfEaten) override;

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) override;

    // Context-scoped composition helpers
    TfClientId GetClientId() const { return m_tfClientId; }
    ITfComposition* GetComposition(ITfContext* pContext) const {
        return (m_pCompositionContext == pContext) ? m_pComposition : nullptr;
    }
    CompositionState GetCompositionState(ITfContext* pContext) const {
        return (m_pCompositionContext == pContext) ? m_compState : CompositionState::None;
    }
    void SetCompositionState(CompositionState state) { m_compState = state; }
    void SetComposition(ITfComposition* pComp, ITfContext* pContext, CompositionState state = CompositionState::Composing);
    bool TerminateCompositionOnHost(ITfContext* pContextOverride = nullptr);

    enum class PendingTerminationStatus {
        PendingSync,
        QueuedAsync,
        FailedSync,
        FailedAsync,
        FailedRequest,
        Succeeded
    };

    struct PendingTermination {
        uint64_t contextId = 0;
        ITfContext* pContext = nullptr;      // Strong reference!
        ITfComposition* pComp = nullptr;     // Strong reference!
        uint64_t generation = 0;
        bool isCommitted = false;
        PendingTerminationStatus status = PendingTerminationStatus::PendingSync;
        HRESULT hrResult = S_OK;

        PendingTermination(uint64_t ctxId, ITfContext* ctx, ITfComposition* comp,
                           uint64_t gen, bool committed,
                           PendingTerminationStatus st, HRESULT hr)
            : contextId(ctxId), pContext(ctx), pComp(comp),
              generation(gen), isCommitted(committed), status(st), hrResult(hr) {
            if (pContext) pContext->AddRef();
            if (pComp) pComp->AddRef();
        }

        ~PendingTermination() {
            if (pComp) { pComp->Release(); pComp = nullptr; }
            if (pContext) { pContext->Release(); pContext = nullptr; }
        }

        PendingTermination(const PendingTermination& o)
            : contextId(o.contextId), pContext(o.pContext), pComp(o.pComp),
              generation(o.generation), isCommitted(o.isCommitted),
              status(o.status), hrResult(o.hrResult) {
            if (pContext) pContext->AddRef();
            if (pComp) pComp->AddRef();
        }

        PendingTermination& operator=(const PendingTermination& o) {
            if (this != &o) {
                if (pComp) pComp->Release();
                if (pContext) pContext->Release();
                contextId = o.contextId;
                pContext = o.pContext;
                pComp = o.pComp;
                generation = o.generation;
                isCommitted = o.isCommitted;
                status = o.status;
                hrResult = o.hrResult;
                if (pContext) pContext->AddRef();
                if (pComp) pComp->AddRef();
            }
            return *this;
        }
    };

    const std::vector<PendingTermination>& GetPendingTerminations() const { return m_pendingTerminations; }
    size_t GetPendingTerminationCount() const { return m_pendingTerminations.size(); }
    PendingTerminationStatus GetPendingStatus(uint64_t contextId) const;

    void OnTerminationSessionCompleted(uint64_t contextId, ITfComposition* pComp, uint64_t generation, HRESULT hr);
    void OnTerminationSessionRequestFailed(uint64_t contextId, ITfComposition* pComp, uint64_t generation, HRESULT hr);

    void SetClientIdForTest(TfClientId id) { m_tfClientId = id; }
    IpcClient& GetIpcClient() { return m_ipcClient; }

    // Verification branch hit counters
    static inline std::atomic<int> s_hitTermSyncSuccess{0};
    static inline std::atomic<int> s_hitTermSyncFail{0};
    static inline std::atomic<int> s_hitTermAsyncQueued{0};
    static inline std::atomic<int> s_hitTermAsyncReqFail{0};
    static inline std::atomic<int> s_hitTermAsyncExecFail{0};
    static inline std::atomic<int> s_hitTermAsyncExecSuccess{0};
    static inline std::atomic<int> s_hitLateNotificationIgnored{0};
    static inline std::atomic<int> s_hitDeactivatePreservedPending{0};
    static inline std::atomic<int> s_hitRecoveryBlocked{0};
    static inline std::atomic<int> s_hitPendingRetrySuccess{0};
    static inline std::atomic<int> s_hitPendingOnKeyDownRecovery{0};
    static inline std::atomic<int> s_hitDeactivateRetrySuccess{0};

    static void ResetBranchCounters();

    // Receipt and drain helpers
    void DrainUnconfirmedCommits(bool forceNow = false);

    // Retained unconfirmed commits for bounded recovery
    enum class UnconfirmedAction {
        Success,
        Cancel
    };

    struct UnconfirmedCommit {
        uint64_t context_id = 0;
        uint64_t event_id = 0;
        UnconfirmedAction action = UnconfirmedAction::Success;
        int retry_count = 0;
        std::chrono::steady_clock::time_point next_retry{};
        uint64_t session_id = 0;
        bool persisted = false;
    };
    std::vector<UnconfirmedCommit> m_unconfirmedCommits;
    bool EnqueueUnconfirmedCommit(uint64_t contextId, uint64_t eventId, UnconfirmedAction action);
    bool SpillUnconfirmedCommitsToFile(const std::vector<UnconfirmedCommit>& commits);
    void LoadSpilloverCommitsFromFile();
    bool RemoveConfirmedSpilloverRecord(uint64_t eventId);
    bool HasPendingSpilloverRecords();
    UINT_PTR m_drainTimerId = 0;
    void StartDrainTimer();
    void StopDrainTimer();

    void OpenManagerFromTray();

private:
    LONG m_cRef = 1;
    uint64_t m_sessionId = 0;
    uint64_t m_activeContextId = 0;
    uint64_t m_terminationGeneration = 0;
    ITfThreadMgr* m_pThreadMgr = nullptr;
    TfClientId m_tfClientId = TF_CLIENTID_NULL;

    // Context-isolated active composition tracking
    ITfComposition* m_pComposition = nullptr;
    ITfContext* m_pCompositionContext = nullptr;
    uint64_t m_compositionContextId = 0;
    CompositionState m_compState = CompositionState::None;

    // Retained pending terminations across contexts - preserves identity and prevents zombie overwrites
    std::vector<PendingTermination> m_pendingTerminations;

    // Per-instance Candidate Window
    CCandidateWindow m_candidateWindow;

    // Per-instance IPC Client (dedicated pipe connection per TIP thread)
    IpcClient m_ipcClient;

    // Context-isolated candidate snapshot versions
    std::map<uint64_t, uint32_t> m_contextCandidateVersions;

    // Taskbar Tray / Language Bar button
    class CLangBarItemButton* m_pLangBarButton = nullptr;

    bool InitKeyEventSink();
    void UninitKeyEventSink();
};