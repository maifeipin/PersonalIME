#pragma once
#include <windows.h>
#include <msctf.h>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <wrl/client.h>
#include "candidate_window.h"
#include "ipc_client.h"

enum class CompositionState {
    None,
    Composing,                // Active inline composition with in-progress input string
    CompositionlessComposing, // Valid context without host inline composition; UI-only preedit
    CommitPendingEnd,         // Text was written to document, but EndComposition failed
    Quarantined               // Unknown outcome or target invalidated with pending request
};

// Stage 7: Separate edit handling, commit outcome, and injection outcome contracts
enum class EditHandling {
    Applied,          // Inline composition updated or ended normally
    UiOnlyHandled,    // Compositionless mode: UI/candidates updated without modifying host document
    Unmodified,       // Host untouched and key not handled
    RecoveryBlocked,  // Previous commit pending end; blocks new edit without bypass
    Indeterminate     // Side-effect uncertain; context quarantined
};

enum class CommitOutcome {
    Success,           // Commit text confirmed written + tail cleanup succeeded
    PartiallyModified, // Commit text confirmed written, tail Selection/EndComposition failed
    Unmodified,        // Confirmed commit text was NOT written to host
    Unknown            // Outcome uncertain (timeout, disconnect, or crash)
};

enum class InjectionOutcome {
    NotAttempted,         // SendInput not called
    NotDispatched,        // SendInput returned 0
    DispatchedUnverified, // SendInput returned cInputs; delivery to host unverified
    Unknown               // Partial send (0 < ret < cInputs) or lost result
};

// Outcome of an edit session to govern key consumption and server synchronization
enum class EditSessionResult {
    Unmodified,        // Host text was not touched by this session (SetText failed or not reached). Safe to rollback server & pass key (*pfEaten = FALSE).
    PartiallyModified, // THIS session modified host text by SetText, but tail operation (SetSelection/EndComposition) failed. Must eat key (*pfEaten = TRUE), do NOT rollback server.
    RecoveryBlocked,   // Previous commit could not be ended, so THIS session could not execute. Current request text was NOT touched. Must NOT eat key (*pfEaten = FALSE), and MUST rollback server for current key!
    Success,           // All operations succeeded. (*pfEaten = TRUE).
    UiOnlyHandled,     // Stage 7: Compositionless UI updated cleanly without touching host document (*pfEaten = TRUE, no rollback).
    Indeterminate      // Stage 7: Side-effect uncertain; quarantine context and eat key (*pfEaten = TRUE).
};

class CTextInputProcessor : 
    public ITfTextInputProcessorEx,
    public ITfKeyEventSink,
    public ITfCompositionSink,
    public ITfDisplayAttributeProvider {
public:
    CTextInputProcessor();
    ~CTextInputProcessor();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfTextInputProcessor & ITfTextInputProcessorEx
    STDMETHODIMP Activate(ITfThreadMgr* ptim, TfClientId tid) override;
    STDMETHODIMP Deactivate() override;
    STDMETHODIMP ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) override;

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL fForeground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* pic, REFGUID rguid, BOOL* pfEaten) override;

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) override;

    // ITfDisplayAttributeProvider
    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) override;
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) override;
    TfGuidAtom GetDisplayAttributeAtom() const { return m_displayAttributeAtom; }
    void SetDisplayAttributeAtomForTest(TfGuidAtom atom) { m_displayAttributeAtom = atom; }

    // Context-scoped composition helpers
    TfClientId GetClientId() const { return m_tfClientId; }
    ITfComposition* GetComposition(ITfContext* pContext) const {
        return (m_pCompositionContext == pContext) ? m_pComposition : nullptr;
    }
    CompositionState GetCompositionState(ITfContext* pContext) const {
        if (m_pCompositionContext == pContext && m_compState != CompositionState::None) {
            return m_compState;
        }
        return GetContextState(reinterpret_cast<uint64_t>(pContext));
    }
    void SetCompositionState(CompositionState state) {
        m_compState = state;
        if (m_compositionContextId != 0) {
            SetContextState(m_compositionContextId, state);
        }
    }
    void SetComposition(ITfComposition* pComp, ITfContext* pContext, CompositionState state = CompositionState::Composing);
    bool TerminateCompositionOnHost(ITfContext* pContextOverride = nullptr);

    // Stage 7: Compositionless mode & lifecycle helpers
    uint64_t EnsureContextEpoch(uint64_t contextId);
    uint64_t GetContextEpoch(uint64_t contextId) const;
    uint64_t AdvanceContextEpoch(uint64_t contextId);
    uint64_t GetContextInputGeneration(uint64_t contextId) const;
    uint64_t AdvanceContextInputGeneration(uint64_t contextId);
    CompositionState GetContextState(uint64_t contextId) const;
    void SetContextState(uint64_t contextId, CompositionState state);
    CandidateWindowMode GetContextMode(uint64_t contextId) const;
    void SetContextMode(uint64_t contextId, CandidateWindowMode mode);
    void EnterCompositionlessMode(ITfContext* pContext);
    void ExitCompositionlessMode(uint64_t contextId);
    bool HasCompositionlessLease(uint64_t contextId, ITfContext* pContext) const;
    void ResetCompositionlessContext(uint64_t contextId, bool advanceEpoch, bool resetServer);
    void ResetAllCompositionlessContexts(bool resetServer);
    bool ResolveAnchorPointForContext(ITfContext* pContext, TfEditCookie ec, ITfRange* pRange, bool hasValidEditCookie, POINT* outPt);
    void CacheContextAnchorPoint(uint64_t contextId, uint64_t epoch, uint64_t viewId, POINT pt);

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

    enum class PendingEditStatus {
        PendingSync,
        QueuedAsync
    };

    struct PendingEditSession {
        uint64_t contextId = 0;
        ITfContext* pContext = nullptr; // Strong reference!
        uint64_t generation = 0;
        bool hasComposition = false;
        std::wstring composition;
        bool hasCommit = false;
        std::wstring commitText;
        uint64_t commitEventId = 0;
        CandidateDisplaySnapshot displaySnapshot;
        PendingEditStatus status = PendingEditStatus::PendingSync;
        bool uiSuppressed = false;
        bool terminateAfterEdit = false;
        bool completed = false;
        EditSessionResult finalResult = EditSessionResult::Unmodified;
        HRESULT finalHr = S_OK;

        PendingEditSession() = default;
        PendingEditSession(uint64_t ctxId, ITfContext* ctx, uint64_t gen,
                           bool hasComp, const std::wstring& comp,
                           bool hasCom, const std::wstring& comText, uint64_t comEvId,
                           const CandidateDisplaySnapshot& snap,
                           PendingEditStatus st)
            : contextId(ctxId), pContext(ctx), generation(gen),
              hasComposition(hasComp), composition(comp),
              hasCommit(hasCom), commitText(comText), commitEventId(comEvId),
              displaySnapshot(snap), status(st) {
            if (pContext) pContext->AddRef();
        }

        ~PendingEditSession() {
            if (pContext) { pContext->Release(); pContext = nullptr; }
        }

        PendingEditSession(const PendingEditSession& o)
            : contextId(o.contextId), pContext(o.pContext), generation(o.generation),
              hasComposition(o.hasComposition), composition(o.composition),
              hasCommit(o.hasCommit), commitText(o.commitText), commitEventId(o.commitEventId),
              displaySnapshot(o.displaySnapshot), status(o.status),
              uiSuppressed(o.uiSuppressed), terminateAfterEdit(o.terminateAfterEdit),
              completed(o.completed), finalResult(o.finalResult), finalHr(o.finalHr) {
            if (pContext) pContext->AddRef();
        }

        PendingEditSession& operator=(const PendingEditSession& o) {
            if (this != &o) {
                if (pContext) pContext->Release();
                contextId = o.contextId;
                pContext = o.pContext;
                generation = o.generation;
                hasComposition = o.hasComposition;
                composition = o.composition;
                hasCommit = o.hasCommit;
                commitText = o.commitText;
                commitEventId = o.commitEventId;
                displaySnapshot = o.displaySnapshot;
                status = o.status;
                uiSuppressed = o.uiSuppressed;
                terminateAfterEdit = o.terminateAfterEdit;
                completed = o.completed;
                finalResult = o.finalResult;
                finalHr = o.finalHr;
                if (pContext) pContext->AddRef();
            }
            return *this;
        }
    };

    const std::vector<PendingEditSession>& GetPendingEditSessions() const { return m_pendingEditSessions; }
    size_t GetPendingEditSessionCount() const;
    bool HasPendingEditSession(uint64_t contextId) const;
    bool HasQueuedEditSession(uint64_t contextId) const;
    bool IsEditSessionUiSuppressed(uint64_t contextId, uint64_t generation) const;
    bool ShouldTerminateAfterEdit(uint64_t contextId, uint64_t generation) const;
    bool IsCompositionlessCommitTargetValid(
        uint64_t contextId,
        ITfContext* pContext,
        uint64_t editGeneration,
        uint64_t expectedEpoch,
        uint64_t expectedInputGen,
        CandidateWindowMode expectedMode) const;
    bool CanShowCandidateWindow(uint64_t contextId, uint64_t generation) const;
    void HideCandidateWindowIfOwnedBy(uint64_t contextId);
    void SuppressQueuedEditSessionsForContext(uint64_t contextId, bool terminateAfter);
    void SuppressAllQueuedEditSessions(bool terminateAfter);
    void OnEditSessionCompleted(uint64_t contextId, uint64_t generation, EditSessionResult result, HRESULT hrFinal);
    bool IsDisplayedSnapshotValid(const CandidateDisplaySnapshot& snap, ITfContext* expectedContext = nullptr) const;
    bool SubmitCandidateSelectionFromSnapshot(const CandidateDisplaySnapshot& snap, int index);
    bool SubmitPageScrollFromSnapshot(const CandidateDisplaySnapshot& snap, bool nextPage);

    CCandidateWindow& GetCandidateWindowForTest() { return m_candidateWindow; }
    uint64_t GetSessionIdForTest() const { return m_sessionId; }
    bool NegotiateStage7ForTest() { return m_ipcClient.NegotiateStage7Capability(m_sessionId); }

    using PluginQueryOverrideFunc = std::function<bool(std::vector<CandidatePluginItem>& outPlugins, bool* outTruncated, uint32_t* outRevision)>;
    using PluginExecuteOverrideFunc = std::function<bool(uint32_t pluginId, uint32_t* outExecStatus)>;
    void SetPluginQueryOverrideForTest(PluginQueryOverrideFunc fn) { m_pluginQueryOverride = std::move(fn); }
    void SetPluginExecuteOverrideForTest(PluginExecuteOverrideFunc fn) { m_pluginExecuteOverride = std::move(fn); }
    bool TrySyncPluginsFromServer(uint64_t contextId, uint32_t advRevTag, bool force = false);
    uint32_t GetSyncedPluginsRevisionForTest() const { return m_syncedPluginsFullRevision; }
    bool WasLastPluginsQueryTruncatedForTest() const { return m_lastPluginsTruncated; }

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
    uint64_t m_editSessionGeneration = 0;
    bool m_hasFocus = true;
    bool m_isDeactivated = false;
    ITfThreadMgr* m_pThreadMgr = nullptr;
    TfClientId m_tfClientId = TF_CLIENTID_NULL;

    // Context-isolated active composition tracking
    ITfComposition* m_pComposition = nullptr;
    ITfContext* m_pCompositionContext = nullptr;
    uint64_t m_compositionContextId = 0;
    CompositionState m_compState = CompositionState::None;

    // Retained pending terminations across contexts - preserves identity and prevents zombie overwrites
    std::vector<PendingTermination> m_pendingTerminations;

    // Retained pending edit sessions across contexts (for TF_S_ASYNC deferred execution)
    std::vector<PendingEditSession> m_pendingEditSessions;

    // Per-instance Candidate Window
    CCandidateWindow m_candidateWindow;

    // Per-instance IPC Client (dedicated pipe connection per TIP thread)
    IpcClient m_ipcClient;
    // Separate IPC Client for non-critical plugin metadata queries/executions so keystroke pipe is never blocked or disconnected
    IpcClient m_pluginIpcClient;
    PluginQueryOverrideFunc m_pluginQueryOverride;
    PluginExecuteOverrideFunc m_pluginExecuteOverride;
    bool m_hasSyncedPlugins = false;
    uint32_t m_syncedPluginsRevTag = 0;
    uint32_t m_syncedPluginsFullRevision = 0;
    bool m_lastPluginsTruncated = false;

    // Context-isolated candidate snapshot versions
    std::map<uint64_t, uint32_t> m_contextCandidateVersions;
    std::map<uint64_t, uint32_t> m_latestServerCandidateVersions;
    std::map<uint64_t, uint32_t> m_contextPageIndices;
    std::map<uint64_t, uint32_t> m_contextPageCounts;
    uint32_t m_currentPageIndex = 0;
    uint32_t m_currentPageCount = 0;

    // Stage 7: Per-context ownership, lifecycle epochs, input generations & compositionless states
    uint64_t m_ownerInstanceId = 0;
    TfGuidAtom m_displayAttributeAtom = TF_INVALID_GUIDATOM;
    std::map<uint64_t, uint64_t> m_contextEpochs;
    std::map<uint64_t, uint64_t> m_contextInputGenerations;
    std::map<uint64_t, CompositionState> m_contextStates;
    std::map<uint64_t, CandidateWindowMode> m_contextModes;
    std::map<uint64_t, Microsoft::WRL::ComPtr<ITfContext>> m_compositionlessContextLeases;

    struct CachedAnchorPoint {
        uint64_t contextEpoch = 0;
        uint64_t viewId = 0;
        POINT pt{0, 0};
        bool valid = false;
    };
    std::map<uint64_t, CachedAnchorPoint> m_cachedAnchors;

    bool ProcessKeyInternal(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten,
                            bool hasExplicitVersion, uint32_t explicitCandidateVersion);

    // Taskbar Tray / Language Bar button
    class CLangBarItemButton* m_pLangBarButton = nullptr;

    bool InitKeyEventSink();
    void UninitKeyEventSink();
};