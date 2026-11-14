#include "text_input_processor.h"
#include "lang_bar_item.h"
#include "edit_session.h"
#include "display_attribute.h"
#include "globals.h"
#include "../common/ime_guids.h"
#include <wrl/client.h>
#include <io.h>
#include <sstream>
#include <shellapi.h>
#include <filesystem>
#include "receipt_journal.h"
#include "receipt_journal_v2.h"
#include "receipt_retry.h"

static LONG64 s_sessionCounter = 0;
static std::mutex s_liveOwnersMutex;
static std::set<uint64_t> s_liveOwnerInstances;

CTextInputProcessor::CTextInputProcessor() {
    m_cRef = 1;
    uint64_t pid = static_cast<uint64_t>(GetCurrentProcessId());
    uint64_t counter = static_cast<uint64_t>(InterlockedIncrement64(&s_sessionCounter));
    m_sessionId = (pid << 32) | (counter & 0xFFFFFFFF);
    m_ownerInstanceId = m_sessionId ^ (GetTickCount64() * 11400714819323198485ull);
    if (m_ownerInstanceId == 0 || m_ownerInstanceId == m_sessionId) {
        m_ownerInstanceId = m_sessionId ^ 0x9E3779B97F4A7C15ULL;
    }
    {
        std::lock_guard<std::mutex> lock(s_liveOwnersMutex);
        s_liveOwnerInstances.insert(m_ownerInstanceId);
    }
    DllAddRef();

    m_candidateWindow.SetOnOpenManager([this]() {
        // Cleanly terminate in-progress composition so host document has no stray underline
        TerminateCompositionOnHost();
        ResetAllCompositionlessContexts(true);
        OpenManagerFromTray();
    });

    m_candidateWindow.SetOnExecutePlugin([this](const CandidatePluginItem& item) {
        TerminateCompositionOnHost();
        ResetAllCompositionlessContexts(true);
        AllowSetForegroundWindow(ASFW_ANY);
        uint32_t execStatus = 0;
        bool ok = false;
        if (m_pluginExecuteOverride) {
            ok = m_pluginExecuteOverride(item.id, &execStatus);
        } else {
            ok = m_pluginIpcClient.ExecutePlugin(m_sessionId, 0, item.id, &execStatus);
        }
        // Do NOT fall back to local ShellExecuteW on cached item.exec_path!
        // If server rejected (e.g. plugin disabled or deleted), refresh plugin metadata from server.
        if (!ok) {
            TrySyncPluginsFromServer(0, 0, true);
        }
    });

    m_candidateWindow.SetOnSelectCandidateSnapshot([this](const CandidateDisplaySnapshot& snap, int index) {
        ResetShiftTapState();
        SubmitCandidateSelectionFromSnapshot(snap, index);
    });

    m_candidateWindow.SetOnPageScrollSnapshot([this](const CandidateDisplaySnapshot& snap, bool nextPage) {
        ResetShiftTapState();
        SubmitPageScrollFromSnapshot(snap, nextPage);
    });
}

bool CTextInputProcessor::TrySyncPluginsFromServer(uint64_t contextId, uint32_t advRevTag, bool force) {
    if (!force && m_hasSyncedPlugins && advRevTag == m_syncedPluginsRevTag) {
        return true;
    }
    std::vector<CandidatePluginItem> dbPlugins;
    bool truncated = false;
    uint32_t fullRev = 0;
    bool ok = false;
    if (m_pluginQueryOverride) {
        ok = m_pluginQueryOverride(dbPlugins, &truncated, &fullRev);
    } else {
        ok = m_pluginIpcClient.GetPlugins(m_sessionId, contextId, dbPlugins, &truncated, &fullRev);
    }
    if (!ok) {
        return false;
    }
    m_hasSyncedPlugins = true;
    m_syncedPluginsRevTag = (fullRev != 0) ? (fullRev & 0xFFu) : advRevTag;
    m_syncedPluginsFullRevision = fullRev;
    m_lastPluginsTruncated = truncated;
    m_candidateWindow.SetPlugins(dbPlugins);
    return true;
}

void CTextInputProcessor::SetUnicodeInjectionAllowedHosts(const std::vector<std::wstring>& hosts, bool filterActive) {
    m_unicodeHostFilterActive = filterActive;
    m_unicodeAllowedHosts.clear();
    m_unicodeAllowedHosts.reserve(hosts.size());
    for (const auto& h : hosts) {
        std::wstring norm = ime::NormalizeProcessNameLower(h);
        if (!norm.empty()) {
            m_unicodeAllowedHosts.push_back(std::move(norm));
        }
    }
}

void CTextInputProcessor::SetHostProcessNameOverrideForTest(const std::wstring& name) {
    m_hostProcessNameOverride = ime::NormalizeProcessNameLower(name);
}

bool CTextInputProcessor::IsUnicodeInjectionAllowedForHost() const {
    if (!m_unicodeInjectionEnabled) {
        return false;
    }
    if (!m_unicodeHostFilterActive) {
        return true;
    }
    if (!m_stage7SettingsSyncHealthy) {
        return false;
    }
    if ((GetTickCount64() - m_lastStage7SettingsSyncTickMs) >= m_stage7SettingsFreshnessMs) {
        return false;
    }
    if (m_unicodeAllowedHosts.empty()) {
        return false;
    }
    std::wstring currentHost = m_hostProcessNameOverride;
    if (currentHost.empty()) {
        wchar_t exePath[MAX_PATH]{};
        DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            return false;
        }
        std::filesystem::path p(exePath);
        currentHost = ime::NormalizeProcessNameLower(p.filename().wstring());
    }
    for (const auto& allowed : m_unicodeAllowedHosts) {
        if (currentHost == allowed) {
            return true;
        }
    }
    return false;
}

void CTextInputProcessor::ApplyStage7Settings(const ime::Stage7SettingsPayload& settings) {
    m_lastStage7SettingsSyncTickMs = GetTickCount64();

    if (settings.theme_mode == "light") {
        m_candidateWindow.SetThemeMode(ThemeMode::Light);
    } else if (settings.theme_mode == "dark") {
        m_candidateWindow.SetThemeMode(ThemeMode::Dark);
    } else {
        m_candidateWindow.SetThemeMode(ThemeMode::FollowSystem);
    }

    if (settings.preedit_display_mode == "compositionless_only") {
        m_candidateWindow.SetPreeditDisplayMode(PreeditDisplayMode::CompositionlessOnly);
    } else if (settings.preedit_display_mode == "disabled") {
        m_candidateWindow.SetPreeditDisplayMode(PreeditDisplayMode::Disabled);
    } else {
        m_candidateWindow.SetPreeditDisplayMode(PreeditDisplayMode::Always);
    }

    m_unicodeInjectionEnabled = settings.unicode_injection_enabled;
    SetUnicodeInjectionAllowedHosts(settings.unicode_allowed_hosts, true);
    m_hasSyncedStage7Settings = true;
    m_stage7SettingsSyncHealthy = true;
    m_syncedPolicyFullRevision = settings.revision;
    m_syncedPolicyRevTag = settings.revision & 0x7u;
}

bool CTextInputProcessor::TrySyncStage7SettingsFromServer(uint64_t contextId, uint32_t advRevTag, bool force) {
    if (!force && m_hasSyncedStage7Settings && m_stage7SettingsSyncHealthy && advRevTag == m_syncedPolicyRevTag) {
        return true;
    }
    ime::Stage7SettingsPayload settings;
    if (!m_ipcClient.GetStage7Settings(m_sessionId, contextId, settings)) {
        // Fail closed for Unicode injection until a complete, consistent policy is synced;
        // existing appearance settings on CCandidateWindow remain intact.
        m_stage7SettingsSyncHealthy = false;
        return false;
    }
    ApplyStage7Settings(settings);
    return true;
}

void CTextInputProcessor::OpenManagerFromTray() {
    // Grant permission for the manager to be brought to the foreground
    AllowSetForegroundWindow(ASFW_ANY);

    if (!m_ipcClient.OpenManager(m_sessionId, 0)) {
        // Direct launch fallback if background IPC service is unavailable
        wchar_t envBuf[MAX_PATH];
        DWORD envLen = GetEnvironmentVariableW(L"PERSONALIME_MANAGER_PATH", envBuf, MAX_PATH);
        if (envLen > 0 && envLen < MAX_PATH) {
            std::error_code ec;
            if (std::filesystem::exists(envBuf, ec)) {
                ShellExecuteW(nullptr, L"open", envBuf, nullptr, nullptr, SW_SHOWNORMAL);
                return;
            }
        }

        wchar_t dllPath[MAX_PATH];
        if (GetModuleFileNameW(g_hInst, dllPath, MAX_PATH) > 0) {
            std::filesystem::path p(dllPath);
            std::filesystem::path dir = p.parent_path();
            std::vector<std::filesystem::path> cands = {
                dir / L"manager" / L"PersonalIME.Manager.exe",
                dir / L"PersonalIME.Manager.exe",
                dir / L"../../src/manager/PersonalIME.Manager/bin/Release/net10.0-windows/PersonalIME.Manager.exe",
                dir / L"../../out/publish/manager/PersonalIME.Manager.exe",
                L"C:\\Program Files\\PersonalIME\\manager\\PersonalIME.Manager.exe"
            };
            for (const auto& c : cands) {
                std::error_code ec;
                if (std::filesystem::exists(c, ec)) {
                    ShellExecuteW(nullptr, L"open", c.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                }
            }
        }
    }
}

CTextInputProcessor::~CTextInputProcessor() {
    {
        std::lock_guard<std::mutex> lock(s_liveOwnersMutex);
        s_liveOwnerInstances.erase(m_ownerInstanceId);
    }
    m_candidateWindow.SetOnOpenManager(nullptr);
    m_candidateWindow.SetOnExecutePlugin(nullptr);
    m_candidateWindow.SetOnSelectCandidate(nullptr);
    m_candidateWindow.SetOnSelectCandidateSnapshot(nullptr);
    m_candidateWindow.SetOnPageScroll(nullptr);
    m_candidateWindow.SetOnPageScrollSnapshot(nullptr);

    // Any queued async edit session that never executed before TIP destruction
    // has NOT modified the host document; cancel any pending commit event it owned.
    for (auto& pending : m_pendingEditSessions) {
        if (!pending.completed) {
            pending.completed = true;
            if (pending.hasCommit && pending.commitEventId != 0) {
                EnqueueUnconfirmedCommit(pending.contextId, pending.commitEventId, UnconfirmedAction::Cancel);
            }
        }
    }
    m_pendingEditSessions.clear();

    // Final Release must not create sessions that AddRef a TIP already being destroyed.
    DrainUnconfirmedCommits(false);
    for (const auto& c : m_unconfirmedCommits) {
        if (!c.persisted) receipt_retry::Retain({c.session_id, c.context_id, c.event_id, static_cast<int>(c.action), c.retry_count});
    }
    m_unconfirmedCommits.clear(); // Durable journal or independent retry owner retains every result.
    StopDrainTimer();
    UninitKeyEventSink();
    SetComposition(nullptr, nullptr, CompositionState::None);
    ResetAllCompositionlessContexts(false);
    m_pendingTerminations.clear();

    if (m_pLangBarButton) {
        m_pLangBarButton->Release();
        m_pLangBarButton = nullptr;
    }
    if (m_pThreadMgr) {
        m_pThreadMgr->Release();
        m_pThreadMgr = nullptr;
    }
    m_candidateWindow.Destroy();
    DllRelease();
}

void CTextInputProcessor::SetComposition(ITfComposition* pComp, ITfContext* pContext, CompositionState state) {
    if (m_pComposition == pComp && m_pCompositionContext == pContext) {
        m_compState = (pComp != nullptr) ? state : CompositionState::None;
        if (m_compositionContextId != 0) {
            SetContextState(m_compositionContextId, m_compState);
        }
        return;
    }
    uint64_t oldCtxId = m_compositionContextId;
    if (pComp) pComp->AddRef();
    if (pContext) pContext->AddRef();

    if (m_pComposition) {
        m_pComposition->Release();
    }
    if (m_pCompositionContext) {
        m_pCompositionContext->Release();
    }

    m_pComposition = pComp;
    m_pCompositionContext = pContext;
    m_compositionContextId = reinterpret_cast<uint64_t>(pContext);
    m_compState = (pComp != nullptr) ? state : CompositionState::None;
    if (oldCtxId != 0 && oldCtxId != m_compositionContextId &&
        GetContextState(oldCtxId) != CompositionState::CompositionlessComposing &&
        GetContextState(oldCtxId) != CompositionState::Quarantined) {
        SetContextState(oldCtxId, CompositionState::None);
    }
    if (m_compositionContextId != 0) {
        ExitCompositionlessMode(m_compositionContextId);
        SetContextState(m_compositionContextId, m_compState);
        SetContextMode(m_compositionContextId, CandidateWindowMode::StandardInline);
    }
}

bool CTextInputProcessor::TerminateCompositionOnHost(ITfContext* pContextOverride) {
    if (m_tfClientId == TF_CLIENTID_NULL) {
        return true;
    }

    ITfContext* pTargetContext = pContextOverride ? pContextOverride : m_pCompositionContext;
    if (!pTargetContext) {
        return true;
    }
    // A synchronous completion may erase the pending record before RequestEditSession
    // returns. Keep its context alive across the entire call and callback.
    Microsoft::WRL::ComPtr<ITfContext> contextLease(pTargetContext);
    uint64_t targetContextId = reinterpret_cast<uint64_t>(pTargetContext);

    // 1. Retry/recovery path for an existing pending termination on this context
    for (auto it = m_pendingTerminations.begin(); it != m_pendingTerminations.end(); ++it) {
        if (it->contextId == targetContextId && it->status != PendingTerminationStatus::Succeeded) {
            if (it->status == PendingTerminationStatus::QueuedAsync ||
                it->status == PendingTerminationStatus::PendingSync) {
                return false; // Already owned by the host; do not duplicate or replace it.
            }
            uint64_t gen = ++m_terminationGeneration;
            it->generation = gen;
            it->status = PendingTerminationStatus::PendingSync;
            ITfComposition* pComp = it->pComp;
            Microsoft::WRL::ComPtr<ITfComposition> compositionLease(pComp);
            ITfContext* pCtx = it->pContext;
            bool isCommitted = it->isCommitted;

            CTerminateCompositionSession* pSession = new CTerminateCompositionSession(
                this, pComp, targetContextId, gen, isCommitted);

            HRESULT hrSession = S_OK;
            HRESULT hr = pCtx->RequestEditSession(
                m_tfClientId, pSession, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);

            bool isSyncBlocked = (hr == TF_E_SYNCHRONOUS || hrSession == TF_E_SYNCHRONOUS);
            if (isSyncBlocked) {
                it->status = PendingTerminationStatus::QueuedAsync;
                s_hitTermAsyncQueued.fetch_add(1, std::memory_order_relaxed);
                hr = pCtx->RequestEditSession(
                    m_tfClientId, pSession, TF_ES_ASYNC | TF_ES_READWRITE, &hrSession);
                if (FAILED(hr) || FAILED(hrSession)) {
                    s_hitTermAsyncReqFail.fetch_add(1, std::memory_order_relaxed);
                    OnTerminationSessionRequestFailed(targetContextId, pComp, gen, FAILED(hr) ? hr : hrSession);
                    pSession->Release();
                    return false;
                }
                pSession->Release();
                return true;
            }

            pSession->Release();
            if (FAILED(hr) && GetPendingStatus(targetContextId) == PendingTerminationStatus::PendingSync) {
                OnTerminationSessionCompleted(targetContextId, pComp, gen, hr);
            }
            if (GetPendingStatus(targetContextId) == PendingTerminationStatus::Succeeded) {
                s_hitPendingRetrySuccess.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            return false;
        }
    }

    // 2. Initial detach path: active composition belongs to this context
    if (m_pComposition && m_pCompositionContext && (pTargetContext == m_pCompositionContext)) {
        ITfComposition* pCompToTerm = m_pComposition;
        Microsoft::WRL::ComPtr<ITfComposition> compositionLease(pCompToTerm);
        ITfContext* pContextToTerm = m_pCompositionContext;
        bool isCommitted = (m_compState == CompositionState::CommitPendingEnd);
        uint64_t gen = ++m_terminationGeneration;

        // Detach active slot
        m_pComposition = nullptr;
        m_pCompositionContext = nullptr;
        m_compState = CompositionState::None;

        // Enqueue into pending list with strong references
        m_pendingTerminations.emplace_back(
            targetContextId, pContextToTerm, pCompToTerm, gen, isCommitted,
            PendingTerminationStatus::PendingSync, S_OK
        );

        // Unconditionally release the references previously held by m_pComposition and m_pCompositionContext
        pCompToTerm->Release();
        pContextToTerm->Release();

        CTerminateCompositionSession* pSession = new CTerminateCompositionSession(
            this, pCompToTerm, targetContextId, gen, isCommitted);

        HRESULT hrSession = S_OK;
        HRESULT hr = pContextToTerm->RequestEditSession(
            m_tfClientId, pSession, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);

        bool isSyncBlocked = (hr == TF_E_SYNCHRONOUS || hrSession == TF_E_SYNCHRONOUS);
        if (isSyncBlocked) {
            for (auto& pending : m_pendingTerminations) {
                if (pending.contextId == targetContextId && pending.generation == gen) {
                    pending.status = PendingTerminationStatus::QueuedAsync;
                    break;
                }
            }
            s_hitTermAsyncQueued.fetch_add(1, std::memory_order_relaxed);
            hr = pContextToTerm->RequestEditSession(
                m_tfClientId, pSession, TF_ES_ASYNC | TF_ES_READWRITE, &hrSession);
            if (FAILED(hr) || FAILED(hrSession)) {
                s_hitTermAsyncReqFail.fetch_add(1, std::memory_order_relaxed);
                OnTerminationSessionRequestFailed(targetContextId, pCompToTerm, gen, FAILED(hr) ? hr : hrSession);
                pSession->Release();
                return false;
            }
            pSession->Release();
            return true;
        }

        pSession->Release();
        if (FAILED(hr) && GetPendingStatus(targetContextId) == PendingTerminationStatus::PendingSync) {
            OnTerminationSessionCompleted(targetContextId, pCompToTerm, gen, hr);
        }
        if (GetPendingStatus(targetContextId) == PendingTerminationStatus::Succeeded) {
            return true;
        }
        return false;
    }

    return true;
}

void CTextInputProcessor::OnTerminationSessionCompleted(
    uint64_t contextId,
    ITfComposition* pComp,
    uint64_t generation,
    HRESULT hr)
{
    for (auto it = m_pendingTerminations.begin(); it != m_pendingTerminations.end(); ++it) {
        if (it->contextId == contextId && it->generation == generation && it->pComp == pComp) {
            it->hrResult = hr;
            if (SUCCEEDED(hr)) {
                if (it->status == PendingTerminationStatus::QueuedAsync) {
                    s_hitTermAsyncExecSuccess.fetch_add(1, std::memory_order_relaxed);
                } else {
                    s_hitTermSyncSuccess.fetch_add(1, std::memory_order_relaxed);
                }
                it->status = PendingTerminationStatus::Succeeded;
                m_ipcClient.ResetContext(m_sessionId, contextId);
                m_pendingTerminations.erase(it);
                return;
            } else {
                if (it->status == PendingTerminationStatus::QueuedAsync) {
                    s_hitTermAsyncExecFail.fetch_add(1, std::memory_order_relaxed);
                    it->status = PendingTerminationStatus::FailedAsync;
                } else {
                    s_hitTermSyncFail.fetch_add(1, std::memory_order_relaxed);
                    it->status = PendingTerminationStatus::FailedSync;
                }
                return;
            }
        }
    }
}

void CTextInputProcessor::OnTerminationSessionRequestFailed(
    uint64_t contextId,
    ITfComposition* pComp,
    uint64_t generation,
    HRESULT hr)
{
    for (auto& pending : m_pendingTerminations) {
        if (pending.contextId == contextId && pending.generation == generation && pending.pComp == pComp) {
            pending.status = PendingTerminationStatus::FailedRequest;
            pending.hrResult = hr;
            return;
        }
    }
}

CTextInputProcessor::PendingTerminationStatus CTextInputProcessor::GetPendingStatus(uint64_t contextId) const {
    for (const auto& pending : m_pendingTerminations) {
        if (pending.contextId == contextId) {
            return pending.status;
        }
    }
    return PendingTerminationStatus::Succeeded;
}

size_t CTextInputProcessor::GetPendingEditSessionCount() const {
    size_t count = 0;
    for (const auto& p : m_pendingEditSessions) {
        if (!p.completed) ++count;
    }
    return count;
}

bool CTextInputProcessor::HasPendingEditSession(uint64_t contextId) const {
    for (const auto& p : m_pendingEditSessions) {
        if (p.contextId == contextId && !p.completed) {
            return true;
        }
    }
    return false;
}

bool CTextInputProcessor::HasQueuedEditSession(uint64_t contextId) const {
    for (const auto& p : m_pendingEditSessions) {
        if (p.contextId == contextId && !p.completed && p.status == PendingEditStatus::QueuedAsync) {
            return true;
        }
    }
    return false;
}

bool CTextInputProcessor::IsEditSessionUiSuppressed(uint64_t contextId, uint64_t generation) const {
    if (m_isDeactivated || !m_hasFocus || m_activeContextId != contextId) {
        return true;
    }
    for (const auto& p : m_pendingEditSessions) {
        if (p.contextId == contextId && p.generation == generation) {
            return p.uiSuppressed;
        }
    }
    return false;
}

bool CTextInputProcessor::ShouldTerminateAfterEdit(uint64_t contextId, uint64_t generation) const {
    for (const auto& p : m_pendingEditSessions) {
        if (p.contextId == contextId && p.generation == generation) {
            return p.terminateAfterEdit;
        }
    }
    return false;
}

bool CTextInputProcessor::IsCompositionlessCommitTargetValid(
    uint64_t contextId,
    ITfContext* pContext,
    uint64_t editGeneration,
    uint64_t expectedEpoch,
    uint64_t expectedInputGen,
    CandidateWindowMode expectedMode) const {
    if (contextId == 0 || !pContext || m_isDeactivated || !m_hasFocus) {
        return false;
    }
    if (editGeneration != 0) {
        if (m_activeContextId != contextId) {
            return false;
        }
        if (ShouldTerminateAfterEdit(contextId, editGeneration)) {
            return false;
        }
        for (const auto& p : m_pendingEditSessions) {
            if (p.contextId == contextId && p.generation == editGeneration &&
                p.status == PendingEditStatus::QueuedAsync) {
                if (!HasTextEditSinkAdvised(contextId)) {
                    return false;
                }
                break;
            }
        }
    } else if (m_activeContextId != 0 && m_activeContextId != contextId) {
        return false;
    }
    if (expectedEpoch != 0 && GetContextEpoch(contextId) != expectedEpoch) {
        return false;
    }
    if (expectedInputGen != 0 && GetContextInputGeneration(contextId) != expectedInputGen) {
        return false;
    }
    if (expectedMode == CandidateWindowMode::Compositionless) {
        if (GetContextState(contextId) != CompositionState::CompositionlessComposing) {
            return false;
        }
        if (!HasCompositionlessLease(contextId, pContext)) {
            return false;
        }
    }
    return true;
}

bool CTextInputProcessor::CanShowCandidateWindow(uint64_t contextId, uint64_t generation) const {
    if (m_isDeactivated || !m_hasFocus || m_activeContextId != contextId) {
        return false;
    }
    for (const auto& p : m_pendingEditSessions) {
        if (p.contextId == contextId && p.generation == generation) {
            return !p.uiSuppressed && !p.terminateAfterEdit;
        }
    }
    return true;
}

void CTextInputProcessor::HideCandidateWindowIfOwnedBy(uint64_t contextId) {
    uint64_t shownCtx = m_candidateWindow.GetSnapshot().context_id;
    if (shownCtx == 0 || shownCtx == contextId) {
        m_candidateWindow.Hide();
    }
}

void CTextInputProcessor::SuppressQueuedEditSessionsForContext(uint64_t contextId, bool terminateAfter) {
    for (auto& p : m_pendingEditSessions) {
        if (p.contextId == contextId && !p.completed) {
            p.uiSuppressed = true;
            if (terminateAfter) {
                p.terminateAfterEdit = true;
            }
            if (p.controlBlock) {
                p.controlBlock->revoked = true;
            }
        }
    }
}

void CTextInputProcessor::SuppressAllQueuedEditSessions(bool terminateAfter) {
    for (auto& p : m_pendingEditSessions) {
        if (!p.completed) {
            p.uiSuppressed = true;
            if (terminateAfter) {
                p.terminateAfterEdit = true;
            }
            if (p.controlBlock) {
                p.controlBlock->revoked = true;
            }
        }
    }
}

void CTextInputProcessor::OnEditSessionCompleted(
    uint64_t contextId,
    uint64_t generation,
    EditSessionResult result,
    HRESULT hrFinal)
{
    PendingEditSession pending{};
    bool found = false;
    for (auto it = m_pendingEditSessions.begin(); it != m_pendingEditSessions.end(); ++it) {
        if (it->contextId == contextId && it->generation == generation) {
            if (it->completed) {
                return; // Strict single terminal state per edit session
            }
            it->completed = true;
            it->finalResult = result;
            it->finalHr = hrFinal;
            pending = *it;
            m_pendingEditSessions.erase(it);
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }

    struct SinkCleanupGuard {
        CTextInputProcessor* tip;
        uint64_t ctxId;
        ~SinkCleanupGuard() {
            if (!tip->HasQueuedEditSession(ctxId) &&
                tip->GetContextState(ctxId) != CompositionState::CompositionlessComposing) {
                tip->UnadviseTextEditSink(ctxId);
            }
        }
    } sinkCleanupGuard{this, contextId};

    auto applySnapshotState = [&](uint32_t ver, uint32_t pageIdx, uint32_t pageCnt) {
        m_contextCandidateVersions[contextId] = ver;
        m_latestServerCandidateVersions[contextId] = ver;
        m_contextPageIndices[contextId] = pageIdx;
        m_contextPageCounts[contextId] = pageCnt;
        if (contextId == m_activeContextId) {
            m_currentPageIndex = pageIdx;
            m_currentPageCount = pageCnt;
        }
    };

    auto finalizeStage7Commit = [&](IpcCommitDisposition disp, IpcKeyResponse* outResp) -> bool {
        return FinalizeStage7CommitForBlock(pending.controlBlock, disp, hrFinal, 0, outResp);
    };

    bool deferredUnicodeInjection = false;
    if (result == EditSessionResult::Success) {
        if (pending.terminateAfterEdit && !pending.hasCommit) {
            HideCandidateWindowIfOwnedBy(contextId);
            ExitCompositionlessMode(contextId);
            applySnapshotState(0, 0, 0);
            m_ipcClient.ResetContext(m_sessionId, contextId);
            RetireContextIfNoPotentialWriter(contextId);
            return;
        }

        if (!pending.displaySnapshot.candidates.empty() ||
            (pending.hasComposition && !pending.composition.empty())) {
            applySnapshotState(
                pending.displaySnapshot.candidate_version,
                pending.displaySnapshot.page_index,
                pending.displaySnapshot.page_count);
        } else if ((pending.hasComposition && pending.composition.empty()) || pending.hasCommit) {
            applySnapshotState(0, 0, 0);
            ExitCompositionlessMode(contextId);
        }

        if (pending.hasCommit && pending.commitEventId != 0) {
            if (m_ipcClient.IsStage7Capable() && pending.controlBlock && pending.controlBlock->wal_prepared) {
                finalizeStage7Commit(IpcCommitDisposition::ConfirmedWritten, nullptr);
            } else {
                EnqueueUnconfirmedCommit(contextId, pending.commitEventId, UnconfirmedAction::Success);
                DrainUnconfirmedCommits(true);
                if (pending.controlBlock && pending.controlBlock->toggle_chinese_after_commit) {
                    m_ipcClient.SetMode(pending.controlBlock->session_id, pending.controlBlock->context_id,
                                        0, IPC_PROTOCOL_VERSION_STAGE4, IPC_UPDATE_CHINESE);
                    if (m_pLangBarButton) {
                        m_pLangBarButton->UpdateStatus();
                    }
                }
            }
        }

        if (pending.terminateAfterEdit || pending.uiSuppressed) {
            HideCandidateWindowIfOwnedBy(contextId);
        }
    } else if (result == EditSessionResult::UiOnlyHandled) {
        if (pending.terminateAfterEdit && !pending.hasCommit) {
            ResetCompositionlessContext(contextId, false, true);
            RetireContextIfNoPotentialWriter(contextId);
            return;
        }

        if (!pending.displaySnapshot.candidates.empty() ||
            (pending.hasComposition && !pending.composition.empty())) {
            applySnapshotState(
                pending.displaySnapshot.candidate_version,
                pending.displaySnapshot.page_index,
                pending.displaySnapshot.page_count);
        } else if ((pending.hasComposition && pending.composition.empty()) || pending.hasCommit) {
            applySnapshotState(0, 0, 0);
            ExitCompositionlessMode(contextId);
        }

        if (pending.terminateAfterEdit || pending.uiSuppressed) {
            HideCandidateWindowIfOwnedBy(contextId);
        }
    } else if (result == EditSessionResult::PartiallyModified) {
        HideCandidateWindowIfOwnedBy(contextId);
        if ((pending.hasComposition && pending.composition.empty()) || pending.hasCommit) {
            applySnapshotState(0, 0, 0);
            if (GetContextState(contextId) == CompositionState::CompositionlessComposing) {
                ExitCompositionlessMode(contextId);
            }
        } else if (!pending.displaySnapshot.candidates.empty() ||
                   (pending.hasComposition && !pending.composition.empty())) {
            applySnapshotState(
                pending.displaySnapshot.candidate_version,
                pending.displaySnapshot.page_index,
                pending.displaySnapshot.page_count);
        }

        if (pending.hasCommit && pending.commitEventId != 0) {
            if (m_ipcClient.IsStage7Capable() && pending.controlBlock && pending.controlBlock->wal_prepared) {
                finalizeStage7Commit(IpcCommitDisposition::ConfirmedWritten, nullptr);
            } else {
                EnqueueUnconfirmedCommit(contextId, pending.commitEventId, UnconfirmedAction::Success);
                DrainUnconfirmedCommits(true);
                if (pending.controlBlock && pending.controlBlock->toggle_chinese_after_commit) {
                    m_ipcClient.SetMode(pending.controlBlock->session_id, pending.controlBlock->context_id,
                                        0, IPC_PROTOCOL_VERSION_STAGE4, IPC_UPDATE_CHINESE);
                    if (m_pLangBarButton) {
                        m_pLangBarButton->UpdateStatus();
                    }
                }
            }
        }
    } else if (result == EditSessionResult::Indeterminate) {
        HideCandidateWindowIfOwnedBy(contextId);
        ExitCompositionlessMode(contextId);
        if (pending.controlBlock) {
            MarkContextUnknownPending(pending.controlBlock->edit_request_id);
        } else {
            SetContextState(contextId, CompositionState::Quarantined);
        }
        applySnapshotState(0, 0, 0);
    } else {
        // Unmodified or RecoveryBlocked: current key text was NOT applied to host!
        if (result == EditSessionResult::RecoveryBlocked) {
            s_hitRecoveryBlocked.fetch_add(1, std::memory_order_relaxed);
        }
        HideCandidateWindowIfOwnedBy(contextId);

        IpcKeyResponse cancelOrRollbackResp{};
        if (pending.hasCommit && pending.commitEventId != 0) {
            if (m_ipcClient.IsStage7Capable() && pending.controlBlock && pending.controlBlock->wal_prepared) {
                if (result == EditSessionResult::Unmodified &&
                    IsUnicodeInjectionEligiblePrecheck(pending, pending.controlBlock, pending.pContext)) {
                    PendingUnicodeInjection injItem{};
                    injItem.edit_request_id = pending.controlBlock->edit_request_id;
                    injItem.controlBlock = pending.controlBlock;
                    injItem.pContext = pending.pContext;
                    injItem.uiSuppressed = pending.uiSuppressed;
                    injItem.terminateAfterEdit = pending.terminateAfterEdit;
                    m_pendingUnicodeInjections.push_back(std::move(injItem));
                    deferredUnicodeInjection = true;
                    if (m_syncEditSessionDepth == 0) {
                        StartDrainTimer();
                    }
                } else {
                    IpcCommitDisposition disp =
                        (pending.uiSuppressed || pending.terminateAfterEdit ||
                         pending.controlBlock->revoked || IsContextQuarantined(contextId))
                        ? IpcCommitDisposition::CancelDiscard
                        : IpcCommitDisposition::CancelRestore;
                    bool ok = finalizeStage7Commit(disp, &cancelOrRollbackResp);
                    if (!ok || disp == IpcCommitDisposition::CancelDiscard) {
                        applySnapshotState(0, 0, 0);
                        ExitCompositionlessMode(contextId);
                    } else if (cancelOrRollbackResp.candidate_count > 0 ||
                               (cancelOrRollbackResp.has_composition && cancelOrRollbackResp.composition[0] != L'\0')) {
                        applySnapshotState(
                            cancelOrRollbackResp.candidate_version,
                            cancelOrRollbackResp.page_index,
                            cancelOrRollbackResp.page_count);
                    } else {
                        applySnapshotState(0, 0, 0);
                        ExitCompositionlessMode(contextId);
                    }
                }
            } else {
                bool durableClaim = EnqueueUnconfirmedCommit(contextId, pending.commitEventId, UnconfirmedAction::Cancel);
                bool cancelOk = false;
                if (durableClaim) {
                    cancelOk = m_ipcClient.NotifyCommitCancel(m_sessionId, contextId, pending.commitEventId, &cancelOrRollbackResp);
                    if (cancelOk) {
                        if (RemoveConfirmedSpilloverRecord(pending.commitEventId)) {
                            for (auto itU = m_unconfirmedCommits.begin(); itU != m_unconfirmedCommits.end(); ++itU) {
                                if (itU->event_id == pending.commitEventId) {
                                    m_unconfirmedCommits.erase(itU);
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!durableClaim || !cancelOk) {
                    applySnapshotState(0, 0, 0);
                    ExitCompositionlessMode(contextId);
                } else {
                    if (cancelOrRollbackResp.candidate_count > 0 ||
                        (cancelOrRollbackResp.has_composition && cancelOrRollbackResp.composition[0] != L'\0')) {
                        applySnapshotState(
                            cancelOrRollbackResp.candidate_version,
                            cancelOrRollbackResp.page_index,
                            cancelOrRollbackResp.page_count);
                    } else {
                        applySnapshotState(0, 0, 0);
                        ExitCompositionlessMode(contextId);
                    }
                }
            }
        } else {
            if (m_ipcClient.Rollback(m_sessionId, contextId, &cancelOrRollbackResp)) {
                if (cancelOrRollbackResp.candidate_count > 0 ||
                    (cancelOrRollbackResp.has_composition && cancelOrRollbackResp.composition[0] != L'\0')) {
                    applySnapshotState(
                        cancelOrRollbackResp.candidate_version,
                        cancelOrRollbackResp.page_index,
                        cancelOrRollbackResp.page_count);
                } else {
                    applySnapshotState(0, 0, 0);
                    ExitCompositionlessMode(contextId);
                }
            } else {
                applySnapshotState(0, 0, 0);
                ExitCompositionlessMode(contextId);
            }
        }

        if (pending.terminateAfterEdit && pending.pContext) {
            bool termOk = TerminateCompositionOnHost(pending.pContext);
            if (termOk && GetComposition(pending.pContext) == nullptr &&
                GetPendingStatus(contextId) == PendingTerminationStatus::Succeeded) {
                applySnapshotState(0, 0, 0);
                ExitCompositionlessMode(contextId);
                m_ipcClient.ResetContext(m_sessionId, contextId);
            }
        }
    }
    if (pending.controlBlock) {
        pending.controlBlock->target_context.Reset();
        if (!deferredUnicodeInjection &&
            !pending.controlBlock->unknown_pending &&
            (!pending.controlBlock->has_commit || !pending.controlBlock->wal_prepared || pending.controlBlock->wal_resolved)) {
            m_activeRequestBlocks.erase(pending.controlBlock->edit_request_id);
        }
    }
    RetireContextIfNoPotentialWriter(contextId);
}

std::vector<INPUT> CTextInputProcessor::BuildUnicodeInputs(const std::wstring& text) {
    std::vector<INPUT> inputs;
    inputs.reserve(text.size() * 2);
    for (wchar_t ch : text) {
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wVk = 0;
        down.ki.wScan = static_cast<WORD>(ch);
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        INPUT up = down;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(down);
        inputs.push_back(up);
    }
    return inputs;
}

static bool IsContextReadOnly(ITfContext* pic);

bool CTextInputProcessor::FinalizeStage7CommitForBlock(
    const std::shared_ptr<EditRequestControlBlock>& cb,
    IpcCommitDisposition disp,
    HRESULT hrFinal,
    uint32_t channel,
    IpcKeyResponse* outResp)
{
    if (!cb) return false;
    uint64_t contextId = cb->context_id;
    receipt_journal_v2::Entry entry{};
    entry.owner_id = cb->owner_id;
    entry.session_id = cb->session_id;
    entry.context_id = cb->context_id;
    entry.context_epoch = cb->context_epoch;
    entry.input_generation = cb->server_commit_generation;
    entry.edit_request_id = cb->edit_request_id;
    entry.commit_event_id = cb->commit_event_id;
    entry.channel = channel;
    entry.text_len = static_cast<uint32_t>(cb->commit_text.size());
    entry.text_digest = receipt_journal_v2::TextDigest(cb->commit_text);
    entry.disposition = static_cast<uint32_t>(disp);
    entry.hr = static_cast<int32_t>(hrFinal);

    auto buildPendingReceipt = [&](bool resolvedInWal, bool ackedFromServer, IpcCommitDisposition ackedDisp) {
        PendingV2Receipt r{};
        r.owner_id = cb->owner_id;
        r.session_id = cb->session_id;
        r.context_id = cb->context_id;
        r.context_epoch = cb->context_epoch;
        r.input_generation = cb->server_commit_generation;
        r.edit_request_id = cb->edit_request_id;
        r.commit_event_id = cb->commit_event_id;
        r.channel = channel;
        r.text_len = entry.text_len;
        r.text_digest = entry.text_digest;
        r.requested_disp = disp;
        r.acked_disp = ackedDisp;
        r.hr = entry.hr;
        r.has_resolved_in_wal = resolvedInWal;
        r.has_acked_from_server = ackedFromServer;
        return r;
    };

    if (!receipt_journal_v2::AppendResolved(entry)) {
        RecordWalFault(cb->commit_event_id, WalFaultStage::Resolved);
        SetContextState(contextId, CompositionState::Quarantined);
        EnqueuePendingV2Receipt(buildPendingReceipt(false, false, IpcCommitDisposition::None));
        return false;
    }
    cb->wal_resolved = true;
    ClearWalFault(cb->commit_event_id, WalFaultStage::Resolved);

    IpcCommitDisposition finalDisp = IpcCommitDisposition::None;
    bool conflict = false;
    bool ok = m_ipcClient.NotifyCommitTerminal(
        cb->session_id,
        cb->context_id,
        cb->commit_event_id,
        disp,
        cb->context_epoch,
        cb->server_commit_generation,
        cb->owner_id,
        finalDisp,
        conflict,
        outResp);
    if (conflict) {
        SetContextState(contextId, CompositionState::Quarantined);
        return false;
    }
    if (ok) {
        entry.disposition = static_cast<uint32_t>(finalDisp);
        if (receipt_journal_v2::AppendServiceAck(entry)) {
            cb->wal_acked = true;
            ClearAllWalFaultsForEvent(cb->commit_event_id);
        } else {
            RecordWalFault(cb->commit_event_id, WalFaultStage::ServiceAck);
        }
    }
    if (!cb->wal_acked) {
        EnqueuePendingV2Receipt(buildPendingReceipt(true, ok, ok ? finalDisp : IpcCommitDisposition::None));
    }
    if (ok && cb->toggle_chinese_after_commit &&
        (finalDisp == IpcCommitDisposition::ConfirmedWritten ||
         finalDisp == IpcCommitDisposition::DispatchedUnverified)) {
        m_ipcClient.SetMode(cb->session_id, cb->context_id, 0, IPC_PROTOCOL_VERSION_STAGE4, IPC_UPDATE_CHINESE);
        if (m_pLangBarButton) {
            m_pLangBarButton->UpdateStatus();
        }
    }
    return ok;
}

bool CTextInputProcessor::IsUnicodeInjectionEligiblePrecheck(
    const PendingEditSession& pending,
    const std::shared_ptr<EditRequestControlBlock>& cb,
    ITfContext* pContext) const
{
    if (!IsUnicodeInjectionAllowedForHost() || !m_ipcClient.IsStage7Capable()) {
        return false;
    }
    if (!cb || !cb->was_compositionless || cb->commit_text.empty() || cb->commit_event_id == 0) {
        return false;
    }
    if (m_pComposition != nullptr ||
        GetContextState(cb->context_id) == CompositionState::CommitPendingEnd ||
        GetContextState(cb->context_id) == CompositionState::Quarantined) {
        return false;
    }
    if (cb->revoked || cb->crossed_side_effect || cb->injection_attempted || IsWalBarrierFaulted()) {
        return false;
    }
    if (pending.uiSuppressed || pending.terminateAfterEdit) {
        return false;
    }
    if (HasQueuedEditSession(cb->context_id) || HasPotentialWriterForContext(cb->context_id)) {
        return false;
    }
    if (m_isDeactivated || !m_hasFocus || m_activeContextId != cb->context_id ||
        !pContext || IsContextReadOnly(pContext)) {
        return false;
    }
    if (GetContextEpoch(cb->context_id) != cb->context_epoch ||
        GetContextInputGeneration(cb->context_id) != cb->input_generation) {
        return false;
    }
    return true;
}

InjectionOutcome CTextInputProcessor::TryUnicodeInjectionFallback(
    const PendingUnicodeInjection& item,
    bool* outTargetInvalidated)
{
    if (outTargetInvalidated) *outTargetInvalidated = false;
    auto markInvalidated = [&]() {
        if (outTargetInvalidated) *outTargetInvalidated = true;
        return InjectionOutcome::NotAttempted;
    };

    const auto& cb = item.controlBlock;
    ITfContext* pContext = item.pContext.Get();
    // 0. Must NEVER execute SendInput while inside DoEditSession.
    if (IsInEditSession()) {
        return InjectionOutcome::NotAttempted;
    }
    // 1. Default OFF; requires explicit opt-in (and host allowlist match when policy-configured) and Stage 7 capable server.
    if (!IsUnicodeInjectionAllowedForHost() || !m_ipcClient.IsStage7Capable()) {
        return InjectionOutcome::NotAttempted;
    }
    // 2. Only eligible for unmodified, non-RecoveryBlocked, pure compositionless rounds.
    if (!cb || !cb->was_compositionless || cb->commit_text.empty() || cb->commit_event_id == 0) {
        return InjectionOutcome::NotAttempted;
    }
    if (m_pComposition != nullptr ||
        GetContextState(cb->context_id) == CompositionState::CommitPendingEnd ||
        GetContextState(cb->context_id) == CompositionState::Quarantined) {
        return InjectionOutcome::NotAttempted;
    }
    // 3. No prior side effects, no revocation, no prior injection attempt, and WAL barrier healthy.
    if (cb->revoked) {
        return markInvalidated();
    }
    if (cb->crossed_side_effect || cb->injection_attempted || IsWalBarrierFaulted()) {
        return InjectionOutcome::NotAttempted;
    }
    if (item.uiSuppressed || item.terminateAfterEdit) {
        return markInvalidated();
    }
    // 4. All TSF write requests for this context must have returned or been revoked before write.
    if (HasQueuedEditSession(cb->context_id) || HasPotentialWriterForContext(cb->context_id)) {
        return InjectionOutcome::NotAttempted;
    }
    // 5. Focus, context, epoch, generation, read-only, and modifier state checks.
    if (m_isDeactivated || !m_hasFocus || m_activeContextId != cb->context_id ||
        !pContext || IsContextReadOnly(pContext)) {
        return markInvalidated();
    }
    if (GetContextEpoch(cb->context_id) != cb->context_epoch ||
        GetContextInputGeneration(cb->context_id) != cb->input_generation) {
        return markInvalidated();
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
        (GetKeyState(VK_MENU) & 0x8000) != 0 ||
        ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0) {
        return InjectionOutcome::NotAttempted;
    }

    // 6. Window, foreground root, GUI focus control, and TSF focus document/context ownership verification.
    struct VerifiedTarget {
        HWND viewHwnd = nullptr;
        HWND rootHwnd = nullptr;
        HWND guiFocusHwnd = nullptr;
        Microsoft::WRL::ComPtr<ITfDocumentMgr> focusDoc;
    };
    auto verifyRealTargetOwnership = [&](const VerifiedTarget* expected, VerifiedTarget* out) -> bool {
        if (m_bypassWindowCheckForTest) {
            return true;
        }
        ITfContextView* pView = nullptr;
        HWND hwnd = nullptr;
        if (SUCCEEDED(pContext->GetActiveView(&pView)) && pView) {
            pView->GetWnd(&hwnd);
            pView->Release();
        }
        if (!hwnd || !IsWindow(hwnd)) {
            return false;
        }
        HWND rootWnd = GetAncestor(hwnd, GA_ROOT);
        HWND fgWnd = GetForegroundWindow();
        if (!rootWnd || rootWnd != fgWnd) {
            return false;
        }
        GUITHREADINFO gti{};
        gti.cbSize = sizeof(gti);
        DWORD fgTid = GetWindowThreadProcessId(fgWnd, nullptr);
        if (!fgTid || !GetGUIThreadInfo(fgTid, &gti) || !gti.hwndFocus) {
            return false;
        }
        // If context has a child control HWND, focus must stay on that HWND (or its descendant),
        // never on a sibling control under the same top-level window.
        if (hwnd != rootWnd) {
            if (gti.hwndFocus != hwnd && !IsChild(hwnd, gti.hwndFocus)) {
                return false;
            }
        } else if (gti.hwndFocus != hwnd && !IsChild(rootWnd, gti.hwndFocus)) {
            return false;
        }
        // Must prove TSF focus document and top context still belong to this exact pContext.
        if (!m_pThreadMgr) {
            return false;
        }
        Microsoft::WRL::ComPtr<ITfDocumentMgr> focusDoc;
        if (FAILED(m_pThreadMgr->GetFocus(&focusDoc)) || !focusDoc) {
            return false;
        }
        Microsoft::WRL::ComPtr<ITfContext> topCtx;
        if (FAILED(focusDoc->GetTop(&topCtx)) || topCtx.Get() != pContext) {
            return false;
        }
        Microsoft::WRL::ComPtr<ITfDocumentMgr> targetDoc;
        if (SUCCEEDED(pContext->GetDocumentMgr(&targetDoc)) && targetDoc && targetDoc.Get() != focusDoc.Get()) {
            return false;
        }
        if (expected) {
            if (hwnd != expected->viewHwnd ||
                rootWnd != expected->rootHwnd ||
                gti.hwndFocus != expected->guiFocusHwnd ||
                focusDoc.Get() != expected->focusDoc.Get()) {
                return false;
            }
        }
        if (out) {
            out->viewHwnd = hwnd;
            out->rootHwnd = rootWnd;
            out->guiFocusHwnd = gti.hwndFocus;
            out->focusDoc = focusDoc;
        }
        return true;
    };

    VerifiedTarget origTarget{};
    if (!verifyRealTargetOwnership(nullptr, &origTarget)) {
        return markInvalidated();
    }
    // 7. Ensure no prior InjectionAttempt or terminal frame exists in v2 WAL for this commit_event_id.
    {
        receipt_journal_v2::Snapshot snap;
        if (!receipt_journal_v2::LoadSnapshot(snap, false)) {
            return InjectionOutcome::NotAttempted;
        }
        auto evIt = snap.events.find(cb->commit_event_id);
        if (evIt != snap.events.end() &&
            (evIt->second.has_injection_attempt || evIt->second.has_resolved || evIt->second.has_service_ack)) {
            return InjectionOutcome::NotAttempted;
        }
    }

    std::vector<INPUT> inputs = BuildUnicodeInputs(cb->commit_text);
    if (inputs.empty()) {
        return InjectionOutcome::NotAttempted;
    }

    // 8. Persist InjectionAttempt BEFORE calling SendInput so crash recovery never re-injects.
    receipt_journal_v2::Entry injEntry{};
    injEntry.owner_id = cb->owner_id;
    injEntry.session_id = cb->session_id;
    injEntry.context_id = cb->context_id;
    injEntry.context_epoch = cb->context_epoch;
    injEntry.input_generation = cb->server_commit_generation;
    injEntry.edit_request_id = cb->edit_request_id;
    injEntry.commit_event_id = cb->commit_event_id;
    injEntry.channel = 1; // 1 = SendInput
    injEntry.text_len = static_cast<uint32_t>(cb->commit_text.size());
    injEntry.text_digest = receipt_journal_v2::TextDigest(cb->commit_text);
    injEntry.disposition = static_cast<uint32_t>(IpcCommitDisposition::None);
    if (!receipt_journal_v2::AppendInjectionAttempt(injEntry)) {
        return InjectionOutcome::NotAttempted;
    }
    cb->injection_attempted = true;

    // 9. Re-check cached fence AND real foreground target immediately before calling SendInput.
    if (IsInEditSession() ||
        m_isDeactivated || !m_hasFocus || m_activeContextId != cb->context_id ||
        cb->revoked ||
        GetContextEpoch(cb->context_id) != cb->context_epoch ||
        GetContextInputGeneration(cb->context_id) != cb->input_generation) {
        return markInvalidated();
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
        (GetKeyState(VK_MENU) & 0x8000) != 0 ||
        ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0) {
        return InjectionOutcome::NotAttempted;
    }
    if (!verifyRealTargetOwnership(&origTarget, nullptr)) {
        return markInvalidated();
    }
    if (m_targetReverifyHookForTest && !m_targetReverifyHookForTest()) {
        return markInvalidated();
    }

    UINT cInputs = static_cast<UINT>(inputs.size());
    UINT sent = m_sendInputOverride
        ? m_sendInputOverride(cInputs, inputs.data(), sizeof(INPUT))
        : SendInput(cInputs, inputs.data(), sizeof(INPUT));

    if (sent == 0) {
        return InjectionOutcome::NotDispatched;
    }
    if (sent == cInputs) {
        return InjectionOutcome::DispatchedUnverified;
    }
    return InjectionOutcome::Unknown;
}

InjectionOutcome CTextInputProcessor::ExecutePendingUnicodeInjection(const PendingUnicodeInjection& item) {
    auto cb = item.controlBlock;
    if (!cb) return InjectionOutcome::NotAttempted;
    uint64_t contextId = cb->context_id;

    auto clearSnapshotState = [&]() {
        m_contextCandidateVersions[contextId] = 0;
        m_latestServerCandidateVersions[contextId] = 0;
        m_contextPageIndices[contextId] = 0;
        m_contextPageCounts[contextId] = 0;
        if (contextId == m_activeContextId) {
            m_currentPageIndex = 0;
            m_currentPageCount = 0;
        }
    };

    bool targetInvalidated = false;
    InjectionOutcome injOutcome = TryUnicodeInjectionFallback(item, &targetInvalidated);
    if (injOutcome == InjectionOutcome::DispatchedUnverified) {
        FinalizeStage7CommitForBlock(cb, IpcCommitDisposition::DispatchedUnverified, S_OK, 1, nullptr);
        clearSnapshotState();
        ExitCompositionlessMode(contextId);
    } else if (injOutcome == InjectionOutcome::Unknown) {
        MarkContextUnknownPending(cb->edit_request_id, 1);
        clearSnapshotState();
        ExitCompositionlessMode(contextId);
        // Since all TSF write requests have already returned, converge immediately to ClosedUnknown.
        if (!HasPotentialWriterForContext(contextId)) {
            FinalizeStage7CommitForBlock(cb, IpcCommitDisposition::ClosedUnknown, cb->hr, 1, nullptr);
            if (cb->wal_resolved) {
                RetireContextIfNoPotentialWriter(contextId);
            }
        }
    } else {
        IpcCommitDisposition disp =
            (targetInvalidated || item.uiSuppressed || item.terminateAfterEdit ||
             cb->revoked || IsContextQuarantined(contextId))
            ? IpcCommitDisposition::CancelDiscard
            : IpcCommitDisposition::CancelRestore;
        IpcKeyResponse cancelOrRollbackResp{};
        bool ok = FinalizeStage7CommitForBlock(cb, disp, cb->hr, 0, &cancelOrRollbackResp);
        if (!ok || disp == IpcCommitDisposition::CancelDiscard) {
            clearSnapshotState();
            ExitCompositionlessMode(contextId);
        } else if (cancelOrRollbackResp.candidate_count > 0 ||
                   (cancelOrRollbackResp.has_composition && cancelOrRollbackResp.composition[0] != L'\0')) {
            m_contextCandidateVersions[contextId] = cancelOrRollbackResp.candidate_version;
            m_latestServerCandidateVersions[contextId] = cancelOrRollbackResp.candidate_version;
            m_contextPageIndices[contextId] = cancelOrRollbackResp.page_index;
            m_contextPageCounts[contextId] = cancelOrRollbackResp.page_count;
            if (contextId == m_activeContextId) {
                m_currentPageIndex = cancelOrRollbackResp.page_index;
                m_currentPageCount = cancelOrRollbackResp.page_count;
            }
        } else {
            clearSnapshotState();
            ExitCompositionlessMode(contextId);
        }
    }

    if (!cb->unknown_pending || cb->wal_resolved) {
        if (!cb->has_commit || !cb->wal_prepared || cb->wal_resolved) {
            m_activeRequestBlocks.erase(cb->edit_request_id);
        }
    }
    RetireContextIfNoPotentialWriter(contextId);
    return injOutcome;
}

bool CTextInputProcessor::ProcessPendingUnicodeInjectionForRequest(
    uint64_t editRequestId,
    InjectionOutcome* outOutcome)
{
    for (auto it = m_pendingUnicodeInjections.begin(); it != m_pendingUnicodeInjections.end(); ++it) {
        if (it->edit_request_id == editRequestId) {
            PendingUnicodeInjection item = std::move(*it);
            m_pendingUnicodeInjections.erase(it);
            InjectionOutcome outcome = ExecutePendingUnicodeInjection(item);
            if (outOutcome) *outOutcome = outcome;
            return true;
        }
    }
    return false;
}

void CTextInputProcessor::DrainPendingUnicodeInjections() {
    if (IsInEditSession() || m_pendingUnicodeInjections.empty()) {
        return;
    }
    auto pendingList = std::move(m_pendingUnicodeInjections);
    m_pendingUnicodeInjections.clear();
    for (const auto& item : pendingList) {
        ExecutePendingUnicodeInjection(item);
    }
}

bool CTextInputProcessor::IsContextQuarantined(uint64_t contextId) const {
    return GetContextState(contextId) == CompositionState::Quarantined;
}

bool CTextInputProcessor::HasPotentialWriterForContext(uint64_t contextId) const {
    for (const auto& p : m_pendingEditSessions) {
        if (p.contextId == contextId && !p.completed) {
            if (!p.controlBlock ||
                p.controlBlock->state == RequestLifecycleState::Queued ||
                p.controlBlock->state == RequestLifecycleState::Executing) {
                return true;
            }
        }
    }
    for (const auto& kv : m_activeRequestBlocks) {
        const auto& cb = kv.second;
        if (cb && cb->context_id == contextId &&
            (cb->state == RequestLifecycleState::Queued ||
             cb->state == RequestLifecycleState::Executing)) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<EditRequestControlBlock> CTextInputProcessor::GetRequestControlBlockForTest(uint64_t editRequestId) const {
    auto it = m_activeRequestBlocks.find(editRequestId);
    return (it != m_activeRequestBlocks.end()) ? it->second : nullptr;
}

bool CTextInputProcessor::MarkContextUnknownPending(uint64_t editRequestId, uint32_t channel) {
    auto it = m_activeRequestBlocks.find(editRequestId);
    if (it == m_activeRequestBlocks.end() || !it->second) {
        return false;
    }
    auto cb = it->second;
    cb->unknown_pending = true;

    if (cb->has_commit && cb->commit_event_id != 0) {
        receipt_journal_v2::Entry entry{};
        entry.owner_id = cb->owner_id;
        entry.session_id = cb->session_id;
        entry.context_id = cb->context_id;
        entry.context_epoch = cb->context_epoch;
        entry.input_generation = cb->server_commit_generation;
        entry.edit_request_id = cb->edit_request_id;
        entry.commit_event_id = cb->commit_event_id;
        entry.channel = channel;
        entry.text_len = static_cast<uint32_t>(cb->commit_text.size());
        entry.text_digest = receipt_journal_v2::TextDigest(cb->commit_text);
        entry.disposition = static_cast<uint32_t>(IpcCommitDisposition::None);
        entry.hr = static_cast<int32_t>(cb->hr);
        if (!receipt_journal_v2::AppendUnknownPending(entry)) {
            RecordWalFault(cb->commit_event_id, WalFaultStage::UnknownPending);
        }
    }

    SetContextState(cb->context_id, CompositionState::Quarantined);
    HideCandidateWindowIfOwnedBy(cb->context_id);
    return true;
}

bool CTextInputProcessor::MarkObservationTimeout(uint64_t editRequestId) {
    auto it = m_activeRequestBlocks.find(editRequestId);
    if (it == m_activeRequestBlocks.end() || !it->second) {
        return false;
    }
    it->second->observation_timed_out = true;
    // Revoke any queued request that has not crossed a host side effect so it cannot write
    // after subsequent keys have been passed through during Quarantine.
    if (it->second->state == RequestLifecycleState::Queued && !it->second->crossed_side_effect) {
        it->second->revoked = true;
    }
    // Timeout alone NEVER forces ClosedUnknown; mark UnknownPending and quarantine only this context.
    return MarkContextUnknownPending(editRequestId);
}

void CTextInputProcessor::CheckObservationTimeouts(std::chrono::milliseconds timeout) {
    auto now = std::chrono::steady_clock::now();
    std::vector<uint64_t> timedOutIds;
    for (const auto& kv : m_activeRequestBlocks) {
        const auto& cb = kv.second;
        if (cb && !cb->observation_timed_out &&
            (cb->state == RequestLifecycleState::Queued || cb->state == RequestLifecycleState::Executing) &&
            (now - cb->created_at >= timeout)) {
            timedOutIds.push_back(kv.first);
        }
    }
    for (uint64_t id : timedOutIds) {
        MarkObservationTimeout(id);
    }
}

bool CTextInputProcessor::IsOwnerInstanceAlive(uint64_t ownerId, uint64_t sessionId) {
    if (ownerId != 0) {
        std::lock_guard<std::mutex> lock(s_liveOwnersMutex);
        if (s_liveOwnerInstances.find(ownerId) != s_liveOwnerInstances.end()) {
            return true;
        }
    }
    DWORD ownerPid = static_cast<DWORD>(sessionId >> 32);
    if (ownerPid == 0) {
        return true; // Unknown PID: be conservative and do not declare dead.
    }
    if (ownerPid == GetCurrentProcessId()) {
        return false; // Same process, but ownerId is no longer in s_liveOwnerInstances.
    }
    HANDLE hProc = OpenProcess(SYNCHRONIZE, FALSE, ownerPid);
    if (hProc != nullptr) {
        DWORD w = WaitForSingleObject(hProc, 0);
        CloseHandle(hProc);
        return (w == WAIT_TIMEOUT);
    }
    return (GetLastError() == ERROR_ACCESS_DENIED);
}

void CTextInputProcessor::RetireContextIfNoPotentialWriter(uint64_t contextId) {
    if (GetContextState(contextId) != CompositionState::Quarantined) {
        return;
    }
    if (HasPotentialWriterForContext(contextId)) {
        return;
    }
    for (const auto& kv : m_activeRequestBlocks) {
        const auto& cb = kv.second;
        if (cb && cb->context_id == contextId && cb->has_commit && cb->wal_prepared && !cb->wal_resolved) {
            return;
        }
    }
    if (m_pComposition && m_compositionContextId == contextId) {
        SetContextState(contextId, CompositionState::CommitPendingEnd);
    } else {
        SetContextState(contextId, CompositionState::None);
    }
    AdvanceContextInputGeneration(contextId);
}

void CTextInputProcessor::RecoverReceiptsFromWal() {
    receipt_journal_v2::Snapshot snap;
    if (!receipt_journal_v2::LoadSnapshot(snap, true)) {
        return;
    }

    // Legacy v1 R1/REC records (P1 intent-only records are already excluded by receipt_journal::Read).
    for (const auto& rec : snap.legacy_records) {
        auto v2It = snap.events.find(rec.eid);
        if (v2It != snap.events.end() && v2It->second.has_service_ack) {
            continue;
        }
        bool ok = false;
        if (rec.action == 0) {
            ok = m_ipcClient.NotifyCommitSuccess(rec.sid, rec.cid, rec.eid);
        } else {
            ok = m_ipcClient.NotifyCommitCancel(rec.sid, rec.cid, rec.eid, nullptr);
        }
        if (ok) {
            receipt_journal::Ack(rec.eid);
            receipt_journal_v2::Entry ack{};
            ack.owner_id = m_ownerInstanceId;
            ack.session_id = rec.sid;
            ack.context_id = rec.cid;
            ack.commit_event_id = rec.eid;
            ack.disposition = static_cast<uint32_t>(
                (rec.action == 0)
                ? IpcCommitDisposition::ConfirmedWritten
                : IpcCommitDisposition::CancelDiscard);
            receipt_journal_v2::AppendServiceAck(ack);
        }
    }

    for (const auto& kv : snap.events) {
        const auto& ev = kv.second;
        if (ev.conflicted) {
            continue;
        }

        receipt_journal_v2::Entry base{};
        base.owner_id = ev.owner_id;
        base.session_id = ev.session_id;
        base.context_id = ev.context_id;
        base.context_epoch = ev.context_epoch;
        base.input_generation = ev.input_generation;
        base.edit_request_id = ev.edit_request_id;
        base.commit_event_id = ev.commit_event_id;
        base.channel = ev.channel;
        base.text_len = ev.text_len;
        base.text_digest = ev.text_digest;

        if (ev.has_resolved && !ev.has_service_ack) {
            IpcCommitDisposition finalDisp = IpcCommitDisposition::None;
            bool conflict = false;
            bool ok = m_ipcClient.NotifyCommitTerminal(
                ev.session_id,
                ev.context_id,
                ev.commit_event_id,
                static_cast<IpcCommitDisposition>(ev.resolved_disposition),
                static_cast<uint32_t>(ev.context_epoch),
                static_cast<uint32_t>(ev.input_generation),
                ev.owner_id,
                finalDisp,
                conflict,
                nullptr);
            if (ok && !conflict) {
                base.disposition = static_cast<uint32_t>(finalDisp);
                receipt_journal_v2::AppendServiceAck(base);
            }
        } else if ((ev.has_prepared || ev.has_injection_attempt || ev.has_unknown_pending) && !ev.has_resolved) {
            IpcCommitDisposition queriedDisp = IpcCommitDisposition::None;
            if (!m_ipcClient.QueryCommitStatus(
                    ev.session_id,
                    ev.context_id,
                    ev.commit_event_id,
                    static_cast<uint32_t>(ev.context_epoch),
                    ev.owner_id,
                    queriedDisp,
                    static_cast<uint32_t>(ev.input_generation))) {
                continue;
            }
            if (queriedDisp != IpcCommitDisposition::None) {
                base.disposition = static_cast<uint32_t>(queriedDisp);
                if (receipt_journal_v2::AppendResolved(base)) {
                    receipt_journal_v2::AppendServiceAck(base);
                }
                continue;
            }

            // Server still has pending commit. Only settle ClosedUnknown if the original owner/writer is verified dead.
            bool ownerDead = false;
            if (ev.owner_id == m_ownerInstanceId) {
                ownerDead = !HasPotentialWriterForContext(ev.context_id);
            } else {
                ownerDead = !IsOwnerInstanceAlive(ev.owner_id, ev.session_id);
            }
            if (ownerDead) {
                base.disposition = static_cast<uint32_t>(IpcCommitDisposition::ClosedUnknown);
                if (!receipt_journal_v2::AppendResolved(base)) {
                    continue;
                }
                IpcCommitDisposition finalDisp = IpcCommitDisposition::None;
                bool conflict = false;
                bool ok = m_ipcClient.NotifyCommitTerminal(
                    ev.session_id,
                    ev.context_id,
                    ev.commit_event_id,
                    IpcCommitDisposition::ClosedUnknown,
                    static_cast<uint32_t>(ev.context_epoch),
                    static_cast<uint32_t>(ev.input_generation),
                    ev.owner_id,
                    finalDisp,
                    conflict,
                    nullptr);
                if (ok && !conflict) {
                    base.disposition = static_cast<uint32_t>(finalDisp);
                    receipt_journal_v2::AppendServiceAck(base);
                }
            }
        }
    }
}

bool CTextInputProcessor::IsDisplayedSnapshotValid(
    const CandidateDisplaySnapshot& snap,
    ITfContext* expectedContext) const
{
    // 1. Global & session state
    if (m_isDeactivated || !m_hasFocus) return false;
    if (snap.session_id == 0 || snap.session_id != m_sessionId) return false;
    if (snap.context_id == 0 || snap.context_id != m_activeContextId || snap.pContext == nullptr) return false;
    if (reinterpret_cast<uint64_t>(snap.pContext) != snap.context_id) return false;
    if (expectedContext != nullptr &&
        (snap.pContext != expectedContext || snap.context_id != reinterpret_cast<uint64_t>(expectedContext))) {
        return false;
    }

    // 2. Context not blocked or quarantined
    if (HasPendingEditSession(snap.context_id)) return false;
    if (GetPendingStatus(snap.context_id) != PendingTerminationStatus::Succeeded) return false;
    CompositionState ctxState = GetCompositionState(snap.pContext);
    if (ctxState == CompositionState::CommitPendingEnd || ctxState == CompositionState::Quarantined) {
        return false;
    }

    // 3. Exact version, epoch, generation & page alignment
    if (snap.candidate_version == 0 || snap.context_epoch == 0 || snap.input_generation == 0) return false;
    if (m_candidateWindow.GetCandidateCount() <= 0) return false;

    const auto& winSnap = m_candidateWindow.GetSnapshot();
    if (winSnap.session_id != snap.session_id ||
        winSnap.context_id != snap.context_id ||
        winSnap.pContext != snap.pContext ||
        winSnap.context_epoch != snap.context_epoch ||
        winSnap.input_generation != snap.input_generation ||
        winSnap.candidate_version != snap.candidate_version ||
        winSnap.page_index != snap.page_index ||
        winSnap.mode != snap.mode) {
        return false;
    }

    auto itEpoch = m_contextEpochs.find(snap.context_id);
    if (itEpoch == m_contextEpochs.end() || itEpoch->second != snap.context_epoch) {
        return false;
    }

    auto itGen = m_contextInputGenerations.find(snap.context_id);
    if (itGen == m_contextInputGenerations.end() || itGen->second != snap.input_generation) {
        return false;
    }

    auto itVer = m_contextCandidateVersions.find(snap.context_id);
    if (itVer == m_contextCandidateVersions.end() || itVer->second != snap.candidate_version) {
        return false;
    }

    auto itSrvVer = m_latestServerCandidateVersions.find(snap.context_id);
    if (itSrvVer == m_latestServerCandidateVersions.end() || itSrvVer->second != snap.candidate_version) {
        return false;
    }

    auto itPage = m_contextPageIndices.find(snap.context_id);
    if (itPage == m_contextPageIndices.end() || itPage->second != snap.page_index) {
        return false;
    }

    // 4 & 5. Mode-specific state & COM strong reference lease
    if (snap.mode == CandidateWindowMode::StandardInline) {
        if (m_pCompositionContext != snap.pContext || m_pComposition == nullptr ||
            m_compState != CompositionState::Composing) {
            return false;
        }
    } else if (snap.mode == CandidateWindowMode::Compositionless) {
        auto itState = m_contextStates.find(snap.context_id);
        if (itState == m_contextStates.end() || itState->second != CompositionState::CompositionlessComposing) {
            return false;
        }
        if ((m_pCompositionContext == snap.pContext && m_pComposition != nullptr) ||
            GetPendingStatus(snap.context_id) != PendingTerminationStatus::Succeeded) {
            return false;
        }
        if (!HasCompositionlessLease(snap.context_id, snap.pContext)) {
            return false;
        }
    } else {
        return false;
    }

    return true;
}

bool CTextInputProcessor::SubmitCandidateSelectionFromSnapshot(const CandidateDisplaySnapshot& snap, int index) {
    ResetShiftTapState();
    if (index < 0 || index >= 9 || index >= static_cast<int>(snap.candidates.size())) {
        return false;
    }
    if (!IsDisplayedSnapshotValid(snap)) {
        return false;
    }
    Microsoft::WRL::ComPtr<ITfContext> ctxLease(snap.pContext);
    BOOL eaten = FALSE;
    return ProcessKeyInternal(
        snap.pContext,
        static_cast<WPARAM>('1' + index),
        0,
        &eaten,
        true,
        snap.candidate_version);
}

bool CTextInputProcessor::SubmitPageScrollFromSnapshot(const CandidateDisplaySnapshot& snap, bool nextPage) {
    ResetShiftTapState();
    if (!IsDisplayedSnapshotValid(snap)) {
        return false;
    }
    Microsoft::WRL::ComPtr<ITfContext> ctxLease(snap.pContext);
    BOOL eaten = FALSE;
    return ProcessKeyInternal(
        snap.pContext,
        nextPage ? VK_NEXT : VK_PRIOR,
        0,
        &eaten,
        true,
        snap.candidate_version);
}

void CTextInputProcessor::ResetBranchCounters() {
    s_hitTermSyncSuccess.store(0);
    s_hitTermSyncFail.store(0);
    s_hitTermAsyncQueued.store(0);
    s_hitTermAsyncReqFail.store(0);
    s_hitTermAsyncExecFail.store(0);
    s_hitTermAsyncExecSuccess.store(0);
    s_hitLateNotificationIgnored.store(0);
    s_hitDeactivatePreservedPending.store(0);
    s_hitRecoveryBlocked.store(0);
    s_hitPendingRetrySuccess.store(0);
    s_hitPendingOnKeyDownRecovery.store(0);
    s_hitDeactivateRetrySuccess.store(0);
}

STDMETHODIMP CTextInputProcessor::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfTextInputProcessor) ||
        IsEqualIID(riid, IID_ITfTextInputProcessorEx)) {
        *ppvObj = static_cast<ITfTextInputProcessorEx*>(this);
    } else if (IsEqualIID(riid, IID_ITfKeyEventSink)) {
        *ppvObj = static_cast<ITfKeyEventSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfCompositionSink)) {
        *ppvObj = static_cast<ITfCompositionSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider)) {
        *ppvObj = static_cast<ITfDisplayAttributeProvider*>(this);
    } else if (IsEqualIID(riid, IID_ITfTextEditSink)) {
        *ppvObj = static_cast<ITfTextEditSink*>(this);
    }

    if (*ppvObj) {
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

STDMETHODIMP CTextInputProcessor::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new CEnumDisplayAttributeInfo();
    return S_OK;
}

STDMETHODIMP CTextInputProcessor::GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) {
    if (!ppInfo) return E_POINTER;
    *ppInfo = nullptr;
    if (IsEqualGUID(guid, GUID_PersonalIME_DisplayAttributeInput)) {
        *ppInfo = new CDisplayAttributeInfo();
        return S_OK;
    }
    return E_INVALIDARG;
}

STDMETHODIMP_(ULONG) CTextInputProcessor::AddRef() {
    return InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(ULONG) CTextInputProcessor::Release() {
    LONG cr = InterlockedDecrement(&m_cRef);
    if (cr == 0) delete this;
    return cr;
}

STDMETHODIMP CTextInputProcessor::Activate(ITfThreadMgr* ptim, TfClientId tid) {
    if (!ptim) return E_INVALIDARG;

    m_isDeactivated = false;
    m_hasFocus = true;
    m_pThreadMgr = ptim;
    m_pThreadMgr->AddRef();
    m_tfClientId = tid;

    if (!InitKeyEventSink()) {
        Deactivate();
        return E_FAIL;
    }

    // Register Display Attribute GUID to TfGuidAtom for inline composition underline rendering
    ITfCategoryMgr* pCatMgr = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ITfCategoryMgr, (void**)&pCatMgr)) && pCatMgr) {
        pCatMgr->RegisterGUID(GUID_PersonalIME_DisplayAttributeInput, &m_displayAttributeAtom);
        pCatMgr->Release();
    }

    LoadSpilloverCommitsFromFile();
    if (!m_unconfirmedCommits.empty()) {
        DrainUnconfirmedCommits(false);
        StartDrainTimer();
    }

    // Dock Language Bar Item onto taskbar notification tray
    ITfLangBarItemMgr* pLangBarItemMgr = nullptr;
    if (m_pThreadMgr && SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfLangBarItemMgr, (void**)&pLangBarItemMgr))) {
        m_pLangBarButton = new CLangBarItemButton(this);
        pLangBarItemMgr->AddItem(m_pLangBarButton);
        pLangBarItemMgr->Release();
    }

    m_ipcClient.NegotiateStage7Capability(m_sessionId);
    if (m_ipcClient.IsStage7Capable()) {
        TrySyncStage7SettingsFromServer(0, 0, true);
    }
    RecoverReceiptsFromWal();

    return S_OK;
}

STDMETHODIMP CTextInputProcessor::ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) {
    return Activate(ptim, tid);
}

STDMETHODIMP CTextInputProcessor::Deactivate() {
    m_isDeactivated = true;
    ResetShiftTapState();
    SuppressAllQueuedEditSessions(true);
    UninitKeyEventSink();

    if (m_pLangBarButton) {
        ITfLangBarItemMgr* pLangBarItemMgr = nullptr;
        if (m_pThreadMgr && SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfLangBarItemMgr, (void**)&pLangBarItemMgr))) {
            pLangBarItemMgr->RemoveItem(m_pLangBarButton);
            pLangBarItemMgr->Release();
        }
        m_pLangBarButton->Release();
        m_pLangBarButton = nullptr;
    }

    m_candidateWindow.Hide();
    m_candidateWindow.Destroy();

    ResetAllCompositionlessContexts(true);
    UnadviseAllTextEditSinks();

    if (m_pCompositionContext) {
        uint64_t oldContextId = m_compositionContextId;
        if (!HasQueuedEditSession(oldContextId)) {
            bool termOk = TerminateCompositionOnHost();
            if (termOk && m_pComposition == nullptr && GetPendingStatus(oldContextId) == PendingTerminationStatus::Succeeded) {
                m_ipcClient.ResetContext(m_sessionId, oldContextId);
            }
        }
    }

    // Attempt a final synchronous retry for any failed pending terminations
    for (size_t i = 0; i < m_pendingTerminations.size(); ) {
        auto& pending = m_pendingTerminations[i];
        if (pending.status == PendingTerminationStatus::FailedSync ||
            pending.status == PendingTerminationStatus::FailedAsync ||
            pending.status == PendingTerminationStatus::FailedRequest) {
            uint64_t contextId = pending.contextId;
            bool ok = TerminateCompositionOnHost(pending.pContext);
            if (ok && GetPendingStatus(contextId) == PendingTerminationStatus::Succeeded) {
                s_hitDeactivateRetrySuccess.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
        }
        ++i;
    }

    // A failed attempt is not a terminated host composition. Keep its owner
    // available for reactivation or a later host termination notification.
    auto it = m_pendingTerminations.begin();
    while (it != m_pendingTerminations.end()) {
        if (it->status == PendingTerminationStatus::QueuedAsync) {
            s_hitDeactivatePreservedPending.fetch_add(1, std::memory_order_relaxed);
            ++it;
        } else if (it->status == PendingTerminationStatus::Succeeded) {
            m_ipcClient.ResetContext(m_sessionId, it->contextId);
            it = m_pendingTerminations.erase(it);
        } else {
            ++it;
        }
    }

    if (m_pThreadMgr) {
        m_pThreadMgr->Release();
        m_pThreadMgr = nullptr;
    }

    m_tfClientId = TF_CLIENTID_NULL;
    m_displayAttributeAtom = TF_INVALID_GUIDATOM;
    DrainUnconfirmedCommits(false);
    if (!m_unconfirmedCommits.empty()) {
        if (SpillUnconfirmedCommitsToFile(m_unconfirmedCommits)) {
            m_unconfirmedCommits.clear();
        }
    }
    StopDrainTimer();
    m_ipcClient.Disconnect();
    return S_OK;
}

bool CTextInputProcessor::InitKeyEventSink() {
    if (!m_pThreadMgr) return false;

    ITfKeystrokeMgr* pKeystrokeMgr = nullptr;
    if (FAILED(m_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKeystrokeMgr))) {
        return false;
    }

    HRESULT hr = pKeystrokeMgr->AdviseKeyEventSink(m_tfClientId, this, TRUE);
    pKeystrokeMgr->Release();
    return SUCCEEDED(hr);
}

void CTextInputProcessor::UninitKeyEventSink() {
    if (!m_pThreadMgr || m_tfClientId == TF_CLIENTID_NULL) return;

    ITfKeystrokeMgr* pKeystrokeMgr = nullptr;
    if (SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKeystrokeMgr))) {
        pKeystrokeMgr->UnadviseKeyEventSink(m_tfClientId);
        pKeystrokeMgr->Release();
    }
}

static bool ShouldFilterKeystroke(WPARAM wParam) {
    bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool isAlt  = (GetKeyState(VK_MENU) & 0x8000) != 0;
    bool isWin  = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;

    // Never intercept Alt or Win shortcuts (e.g. Alt+Tab, Win+R)
    if (isAlt || isWin) return true;

    // If Ctrl is held, only allow specific IME hotkeys to pass into IME engine
    if (isCtrl) {
        if (wParam == VK_SHIFT || wParam == VK_SPACE || wParam == VK_OEM_PERIOD) {
            return false; // Allowed for IME (Ctrl+Shift, Ctrl+Space, Ctrl+.)
        }
        return true; // Pass all other Ctrl+... (Ctrl+C, Ctrl+V, etc.) to host
    }

    return false;
}

static bool IsContextReadOnly(ITfContext* pic) {
    if (!pic) return true;
    TF_STATUS status{};
    if (SUCCEEDED(pic->GetStatus(&status))) {
        if (status.dwDynamicFlags & TS_SD_READONLY) {
            return true;
        }
    }
    return false;
}

void CTextInputProcessor::ResetShiftTapState() {
    m_shiftTapPending = false;
    m_leftShiftDown = false;
    m_rightShiftDown = false;
    m_genericShiftDown = false;
}

bool CTextInputProcessor::IsShiftKeyCode(WPARAM wParam) {
    return wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT;
}

void CTextInputProcessor::UpdateShiftKeyState(WPARAM wParam, LPARAM lParam, bool isDown) {
    UINT vkSide = static_cast<UINT>(wParam);
    if (vkSide == VK_SHIFT) {
        UINT sc = static_cast<UINT>((lParam >> 16) & 0xFF);
        if (sc != 0) {
            UINT mapped = MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX);
            if (mapped == VK_LSHIFT || mapped == VK_RSHIFT) {
                vkSide = mapped;
            }
        }
    }

    if (isDown) {
        bool isRepeat = (lParam & 0x40000000) != 0;
        bool otherMods = ((GetKeyState(VK_CONTROL) | GetKeyState(VK_MENU) |
                           GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
        bool alreadyAnyShiftDown = m_leftShiftDown || m_rightShiftDown || m_genericShiftDown;

        if (vkSide == VK_LSHIFT) {
            if (m_rightShiftDown || m_genericShiftDown || isRepeat || otherMods) {
                m_shiftTapPending = false;
            } else if (!alreadyAnyShiftDown) {
                m_shiftTapPending = true;
            }
            m_leftShiftDown = true;
        } else if (vkSide == VK_RSHIFT) {
            if (m_leftShiftDown || m_genericShiftDown || isRepeat || otherMods) {
                m_shiftTapPending = false;
            } else if (!alreadyAnyShiftDown) {
                m_shiftTapPending = true;
            }
            m_rightShiftDown = true;
        } else {
            if (alreadyAnyShiftDown || isRepeat || otherMods) {
                m_shiftTapPending = false;
            } else {
                m_shiftTapPending = true;
            }
            m_genericShiftDown = true;
        }
    } else {
        if (vkSide == VK_LSHIFT) {
            m_leftShiftDown = false;
        } else if (vkSide == VK_RSHIFT) {
            m_rightShiftDown = false;
        } else {
            m_genericShiftDown = false;
        }
    }
}

STDMETHODIMP CTextInputProcessor::OnSetFocus(BOOL fForeground) {
    m_hasFocus = (fForeground != FALSE);
    if (!fForeground) {
        ResetShiftTapState();
        if (m_activeContextId != 0) {
            AdvanceContextEpoch(m_activeContextId);
        }
        m_candidateWindow.Hide();
        SuppressAllQueuedEditSessions(true);
        ResetAllCompositionlessContexts(true);
        if (m_pCompositionContext) {
            uint64_t oldContextId = m_compositionContextId;
            if (!HasQueuedEditSession(oldContextId)) {
                bool termOk = TerminateCompositionOnHost();
                if (termOk && m_pComposition == nullptr && GetPendingStatus(oldContextId) == PendingTerminationStatus::Succeeded) {
                    m_ipcClient.ResetContext(m_sessionId, oldContextId);
                }
            }
        } else if (m_activeContextId != 0 &&
                   !HasQueuedEditSession(m_activeContextId) &&
                   GetPendingStatus(m_activeContextId) == PendingTerminationStatus::Succeeded) {
            m_ipcClient.ResetContext(m_sessionId, m_activeContextId);
        }
    }
    return S_OK;
}

STDMETHODIMP CTextInputProcessor::OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (!pfEaten) return E_POINTER;
    *pfEaten = FALSE;

    if (ShouldFilterKeystroke(wParam) || IsContextReadOnly(pic)) {
        ResetShiftTapState();
        return S_OK;
    }

    if (IsShiftKeyCode(wParam)) {
        UpdateShiftKeyState(wParam, lParam, true);
        return S_OK;
    }
    m_shiftTapPending = false;

    uint64_t contextId = reinterpret_cast<uint64_t>(pic);
    CheckObservationTimeouts();
    if (HasQueuedEditSession(contextId) || IsContextQuarantined(contextId)) {
        return S_OK;
    }

    if ((wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN) &&
        m_candidateWindow.GetCandidateCount() > 0 &&
        IsDisplayedSnapshotValid(m_candidateWindow.GetSnapshot(), pic)) {
        *pfEaten = TRUE;
        return S_OK;
    }

    IpcKeyRequest req{};
    req.msg_type = static_cast<uint32_t>(IpcMsgType::TestKeyDown);
    req.session_id = m_sessionId;
    req.context_id = reinterpret_cast<uint64_t>(pic);
    req.context_epoch = static_cast<uint32_t>(EnsureContextEpoch(contextId));
    SetIpcRequestOwnerInstanceId(req, m_ownerInstanceId);
    req.vk_code = static_cast<uint32_t>(wParam);
    req.scan_code = static_cast<uint32_t>(lParam & 0xFF);
    req.modifiers = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) req.modifiers |= 1;
    if (GetKeyState(VK_CONTROL) & 0x8000) req.modifiers |= 2;
    if (GetKeyState(VK_MENU) & 0x8000) req.modifiers |= 4;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) req.modifiers |= 8;
    if (GetKeyState(VK_CAPITAL) & 0x0001) req.modifiers |= IPC_MOD_CAPSLOCK;
    req.protocol_version = m_ipcClient.GetPreferredProtocolVersion(m_sessionId);

    IpcKeyResponse resp{};
    bool sentOk = m_ipcClient.SendRequest(req, resp, 50);
    if (sentOk && !resp.handled &&
        req.protocol_version == IPC_PROTOCOL_VERSION_STAGE7 &&
        (resp.status_flags & IPC_STATUS_STAGE7_CAPABLE) == 0) {
        m_ipcClient.MarkStage7Capable(false);
        req.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
        resp = {};
        sentOk = m_ipcClient.SendRequest(req, resp, 50);
    }
    if (sentOk) {
        *pfEaten = (resp.handled != 0) ? TRUE : FALSE;
    }

    return S_OK;
}

bool CTextInputProcessor::SpillUnconfirmedCommitsToFile(const std::vector<UnconfirmedCommit>& commits) {
    std::vector<receipt_journal::Record> records;
    for (const auto& c : commits) if (!c.persisted)
        records.push_back({c.session_id ? c.session_id : m_sessionId, c.context_id, c.event_id, static_cast<int>(c.action), (std::min)(c.retry_count, 32)});
    return records.empty() || receipt_journal::Append(records);
}

void CTextInputProcessor::LoadSpilloverCommitsFromFile() {
    std::vector<receipt_journal::Record> records;
    if (!receipt_journal::Read(records)) return; // Failure is not an empty snapshot.
    for (const auto& r : records) {
        if (m_unconfirmedCommits.size() >= 512) break;
        bool exists = false;
        for (const auto& c : m_unconfirmedCommits) if (c.event_id == r.eid) { exists = true; break; }
        if (!exists) m_unconfirmedCommits.push_back({r.cid, r.eid, static_cast<UnconfirmedAction>(r.action), r.retry,
            std::chrono::steady_clock::now(), r.sid, true});
    }
}

bool CTextInputProcessor::RemoveConfirmedSpilloverRecord(uint64_t eventId) {
    return receipt_journal::Ack(eventId);
}

bool CTextInputProcessor::HasPendingSpilloverRecords() {
    std::vector<receipt_journal::Record> records;
    return receipt_retry::Pending() || !receipt_journal::Read(records) || !records.empty();
}
static thread_local std::map<UINT_PTR, CTextInputProcessor*> s_timerMap;

static VOID CALLBACK DrainTimerCallback(HWND, UINT, UINT_PTR idEvent, DWORD) {
    auto it = s_timerMap.find(idEvent);
    if (it != s_timerMap.end() && it->second) {
        if (it->second->m_unconfirmedCommits.size() < 512 && it->second->HasPendingSpilloverRecords()) {
            it->second->LoadSpilloverCommitsFromFile();
        }
        it->second->CheckObservationTimeouts();
        it->second->DrainUnconfirmedCommits(false);
    }
}

void CTextInputProcessor::StartDrainTimer() {
    if (m_drainTimerId == 0) {
        m_drainTimerId = SetTimer(nullptr, 0, 300, DrainTimerCallback);
        if (m_drainTimerId != 0) {
            s_timerMap[m_drainTimerId] = this;
        }
    }
}

void CTextInputProcessor::StopDrainTimer() {
    if (m_drainTimerId != 0) {
        s_timerMap.erase(m_drainTimerId);
        KillTimer(nullptr, m_drainTimerId);
        m_drainTimerId = 0;
    }
}

bool CTextInputProcessor::EnqueueUnconfirmedCommit(uint64_t contextId, uint64_t eventId, UnconfirmedAction action) {
    UnconfirmedCommit c{contextId, eventId, action, 0, std::chrono::steady_clock::now(), m_sessionId, false};
    c.persisted = SpillUnconfirmedCommitsToFile({c});
    if (!c.persisted) {
        receipt_retry::Retain({c.session_id,c.context_id,c.event_id,static_cast<int>(c.action),0});
        StartDrainTimer();
        return false; // An independent owner, NOT a durable claim.
    }
    if (m_unconfirmedCommits.size() < 512) m_unconfirmedCommits.push_back(c);
    StartDrainTimer();
    return true;
}
void CTextInputProcessor::RecordWalFault(uint64_t eventId, WalFaultStage stage) {
    if (eventId == 0) return;
    m_walFaultedEvents[eventId] |= static_cast<uint32_t>(stage);
}

void CTextInputProcessor::ClearWalFault(uint64_t eventId, WalFaultStage stage) {
    auto it = m_walFaultedEvents.find(eventId);
    if (it == m_walFaultedEvents.end()) return;
    it->second &= ~static_cast<uint32_t>(stage);
    if (it->second == 0) {
        m_walFaultedEvents.erase(it);
    }
}

void CTextInputProcessor::ClearAllWalFaultsForEvent(uint64_t eventId) {
    m_walFaultedEvents.erase(eventId);
}

bool CTextInputProcessor::HasPendingObservationRequests() const {
    for (const auto& kv : m_activeRequestBlocks) {
        const auto& cb = kv.second;
        if (cb && !cb->observation_timed_out &&
            (cb->state == RequestLifecycleState::Queued || cb->state == RequestLifecycleState::Executing)) {
            return true;
        }
    }
    return false;
}

void CTextInputProcessor::EnqueuePendingV2Receipt(const PendingV2Receipt& r) {
    for (const auto& existing : m_pendingV2Receipts) {
        if (existing.commit_event_id == r.commit_event_id) {
            return;
        }
    }
    if (m_pendingV2Receipts.size() < 512) {
        PendingV2Receipt copy = r;
        copy.next_retry = std::chrono::steady_clock::now();
        m_pendingV2Receipts.push_back(copy);
    }
    StartDrainTimer();
}

void CTextInputProcessor::DrainPendingV2Receipts(bool forceNow) {
    if (m_pendingV2Receipts.empty()) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    size_t processed = 0;
    constexpr size_t MAX_DRAIN_PER_INVOCATION = 8;

    for (auto it = m_pendingV2Receipts.begin(); it != m_pendingV2Receipts.end() && processed < MAX_DRAIN_PER_INVOCATION; ) {
        if (!forceNow && now < it->next_retry) {
            ++it;
            continue;
        }
        processed++;

        if (!it->has_resolved_in_wal) {
            receipt_journal_v2::Entry resEntry{};
            resEntry.owner_id = it->owner_id;
            resEntry.session_id = it->session_id;
            resEntry.context_id = it->context_id;
            resEntry.context_epoch = it->context_epoch;
            resEntry.input_generation = it->input_generation;
            resEntry.edit_request_id = it->edit_request_id;
            resEntry.commit_event_id = it->commit_event_id;
            resEntry.channel = it->channel;
            resEntry.text_len = it->text_len;
            resEntry.text_digest = it->text_digest;
            resEntry.disposition = static_cast<uint32_t>(it->requested_disp);
            resEntry.hr = it->hr;
            if (!receipt_journal_v2::AppendResolved(resEntry)) {
                it->retry_count = (std::min)(it->retry_count + 1, 32);
                int shift = (std::min)(it->retry_count, 5);
                it->next_retry = now + std::chrono::milliseconds(50 * (1 << shift));
                ++it;
                continue;
            }
            it->has_resolved_in_wal = true;
            ClearWalFault(it->commit_event_id, WalFaultStage::Resolved);
            auto cbIt = m_activeRequestBlocks.find(it->edit_request_id);
            if (cbIt != m_activeRequestBlocks.end() && cbIt->second) {
                cbIt->second->wal_resolved = true;
            }
            RetireContextIfNoPotentialWriter(it->context_id);
        }

        if (!it->has_acked_from_server) {
            IpcCommitDisposition finalDisp = IpcCommitDisposition::None;
            bool conflict = false;
            IpcKeyResponse termResp{};
            bool ok = m_ipcClient.NotifyCommitTerminal(
                it->session_id,
                it->context_id,
                it->commit_event_id,
                it->requested_disp,
                it->context_epoch,
                it->input_generation,
                it->owner_id,
                finalDisp,
                conflict,
                &termResp);
            if (conflict) {
                SetContextState(it->context_id, CompositionState::Quarantined);
                it = m_pendingV2Receipts.erase(it);
                continue;
            }
            if (ok) {
                it->has_acked_from_server = true;
                it->acked_disp = finalDisp;
                if (it->requested_disp == IpcCommitDisposition::CancelRestore &&
                    finalDisp == IpcCommitDisposition::CancelRestore &&
                    (termResp.candidate_count > 0 ||
                     (termResp.has_composition && termResp.composition[0] != L'\0'))) {
                    m_contextCandidateVersions[it->context_id] = termResp.candidate_version;
                    m_latestServerCandidateVersions[it->context_id] = termResp.candidate_version;
                    m_contextPageIndices[it->context_id] = termResp.page_index;
                    m_contextPageCounts[it->context_id] = termResp.page_count;
                }
            }
        }

        if (it->has_acked_from_server) {
            receipt_journal_v2::Entry ackEntry{};
            ackEntry.owner_id = it->owner_id;
            ackEntry.session_id = it->session_id;
            ackEntry.context_id = it->context_id;
            ackEntry.context_epoch = it->context_epoch;
            ackEntry.input_generation = it->input_generation;
            ackEntry.edit_request_id = it->edit_request_id;
            ackEntry.commit_event_id = it->commit_event_id;
            ackEntry.channel = it->channel;
            ackEntry.text_len = it->text_len;
            ackEntry.text_digest = it->text_digest;
            ackEntry.disposition = static_cast<uint32_t>(it->acked_disp);
            ackEntry.hr = it->hr;
            if (receipt_journal_v2::AppendServiceAck(ackEntry)) {
                ClearAllWalFaultsForEvent(it->commit_event_id);
                uint64_t ctxId = it->context_id;
                auto cbIt = m_activeRequestBlocks.find(it->edit_request_id);
                if (cbIt != m_activeRequestBlocks.end() && cbIt->second) {
                    cbIt->second->wal_acked = true;
                    if (!cbIt->second->unknown_pending) {
                        m_activeRequestBlocks.erase(cbIt);
                    }
                }
                it = m_pendingV2Receipts.erase(it);
                RetireContextIfNoPotentialWriter(ctxId);
                continue;
            }
        }

        it->retry_count = (std::min)(it->retry_count + 1, 32);
        int shift = (std::min)(it->retry_count, 5);
        it->next_retry = now + std::chrono::milliseconds(50 * (1 << shift));
        ++it;
    }
}

void CTextInputProcessor::DrainUnconfirmedCommits(bool forceNow) {
    DrainPendingUnicodeInjections();
    DrainPendingV2Receipts(forceNow);
    constexpr size_t MAX_QUEUE_SIZE = 512;
    if (m_unconfirmedCommits.empty()) {
        if (!HasPendingSpilloverRecords() && m_pendingV2Receipts.empty() && !HasPendingObservationRequests()) {
            StopDrainTimer();
        } else if (!m_pendingV2Receipts.empty() || HasPendingObservationRequests()) {
            StartDrainTimer();
        }
        return;
    }

    auto now = std::chrono::steady_clock::now();
    size_t processed = 0;
    constexpr size_t MAX_DRAIN_PER_INVOCATION = 8;

    for (auto it = m_unconfirmedCommits.begin(); it != m_unconfirmedCommits.end() && processed < MAX_DRAIN_PER_INVOCATION; ) {
        if (!forceNow && now < it->next_retry) {
            ++it;
            continue;
        }
        processed++;
        bool ok = false;
        IpcKeyResponse drainResp{};
        uint64_t effectiveSession = (it->session_id != 0) ? it->session_id : m_sessionId;
        if (it->action == UnconfirmedAction::Success) {
            ok = m_ipcClient.NotifyCommitSuccess(effectiveSession, it->context_id, it->event_id);
        } else {
            ok = m_ipcClient.NotifyCommitCancel(effectiveSession, it->context_id, it->event_id, &drainResp);
            if (ok) {
                if (drainResp.candidate_count > 0 || (drainResp.has_composition && drainResp.composition[0] != L'\0')) {
                    m_contextCandidateVersions[it->context_id] = drainResp.candidate_version;
                    m_latestServerCandidateVersions[it->context_id] = drainResp.candidate_version;
                    m_contextPageIndices[it->context_id] = drainResp.page_index;
                    m_contextPageCounts[it->context_id] = drainResp.page_count;
                }
            }
        }

        if (ok) {
            uint64_t confirmedEid = it->event_id;
            if (RemoveConfirmedSpilloverRecord(confirmedEid)) it = m_unconfirmedCommits.erase(it); else ++it;
        } else {
            it->retry_count = (std::min)(it->retry_count + 1, 32);
            int shift = (std::min)(it->retry_count, 5);
            it->next_retry = now + std::chrono::milliseconds(50 * (1 << shift));
            ++it;
        }
    }

    if (m_unconfirmedCommits.size() > MAX_QUEUE_SIZE) {
        size_t excess = m_unconfirmedCommits.size() - MAX_QUEUE_SIZE;
        std::vector<UnconfirmedCommit> toSpill(m_unconfirmedCommits.begin(), m_unconfirmedCommits.begin() + excess);
        if (SpillUnconfirmedCommitsToFile(toSpill)) {
            m_unconfirmedCommits.erase(m_unconfirmedCommits.begin(), m_unconfirmedCommits.begin() + excess);
        }
    }

    if (m_unconfirmedCommits.empty() && !HasPendingSpilloverRecords() &&
        m_pendingV2Receipts.empty() && !HasPendingObservationRequests()) {
        StopDrainTimer();
    } else {
        StartDrainTimer();
    }
}

STDMETHODIMP CTextInputProcessor::OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (!pfEaten) return E_POINTER;
    *pfEaten = FALSE;
    if (!IsShiftKeyCode(wParam)) {
        m_shiftTapPending = false;
    }
    ProcessKeyInternal(pic, wParam, lParam, pfEaten, false, 0);
    return S_OK;
}

bool CTextInputProcessor::ProcessKeyInternal(
    ITfContext* pic,
    WPARAM wParam,
    LPARAM lParam,
    BOOL* pfEaten,
    bool hasExplicitVersion,
    uint32_t explicitCandidateVersion,
    bool isShiftTap)
{
    if (!pfEaten) return false;
    *pfEaten = FALSE;

    DrainUnconfirmedCommits();
    if (receipt_retry::Pending()) return false;

    if (ShouldFilterKeystroke(wParam) || IsContextReadOnly(pic)) {
        return false;
    }

    uint64_t contextId = reinterpret_cast<uint64_t>(pic);
    CheckObservationTimeouts();

    // Do not overlap new keystrokes while an asynchronous edit session is queued on this context
    if (HasQueuedEditSession(contextId)) {
        *pfEaten = FALSE;
        return false;
    }

    // Do not overlap new keystrokes if context is Quarantined
    if (GetCompositionState(pic) == CompositionState::Quarantined) {
        *pfEaten = FALSE;
        HideCandidateWindowIfOwnedBy(contextId);
        return false;
    }

    // If returning to a context with a pending/failed termination, retry finishing it before handling new input
    if (GetPendingStatus(contextId) != PendingTerminationStatus::Succeeded) {
        bool retryOk = TerminateCompositionOnHost(pic);
        if (retryOk && GetPendingStatus(contextId) == PendingTerminationStatus::Succeeded) {
            s_hitPendingOnKeyDownRecovery.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Cannot cleanly end previous composition on this context: block new composition to prevent overlapping!
            *pfEaten = FALSE;
            HideCandidateWindowIfOwnedBy(contextId);
            return false;
        }
    }

    if (m_activeContextId != contextId) {
        if (m_activeContextId != 0) {
            SuppressQueuedEditSessionsForContext(m_activeContextId, true);
            HideCandidateWindowIfOwnedBy(m_activeContextId);
            if (GetContextState(m_activeContextId) == CompositionState::CompositionlessComposing) {
                ResetCompositionlessContext(m_activeContextId, true, true);
            } else {
                AdvanceContextEpoch(m_activeContextId);
            }
        }
        if (m_pCompositionContext && m_pCompositionContext != pic) {
            uint64_t oldContextId = m_compositionContextId;
            if (!HasQueuedEditSession(oldContextId)) {
                bool termOk = TerminateCompositionOnHost();
                if (termOk && m_pComposition == nullptr && GetPendingStatus(oldContextId) == PendingTerminationStatus::Succeeded) {
                    m_ipcClient.ResetContext(m_sessionId, oldContextId);
                }
            }
        }
        m_activeContextId = contextId;
    }
    EnsureContextEpoch(contextId);
    m_hasFocus = true;
    m_isDeactivated = false;

    bool selectLastOnPageUp = false;
    uint32_t navSnapshotVersion = 0;
    bool useNavSnapshotVersion = false;
    bool isExplicitCandidateSelect = hasExplicitVersion && (wParam >= '1' && wParam <= '9');

    if (!hasExplicitVersion &&
        m_candidateWindow.GetCandidateCount() > 0 &&
        IsDisplayedSnapshotValid(m_candidateWindow.GetSnapshot(), pic)) {
        const auto& snap = m_candidateWindow.GetSnapshot();
        int count = m_candidateWindow.GetCandidateCount();
        int sel = m_candidateWindow.GetSelectedIndex();
        if (wParam == VK_RIGHT || wParam == VK_DOWN) {
            if (sel + 1 < count) {
                m_candidateWindow.SetSelectedIndex(sel + 1);
                *pfEaten = TRUE;
                return true;
            } else if (snap.page_index + 1 < snap.page_count) {
                wParam = VK_NEXT;
                navSnapshotVersion = snap.candidate_version;
                useNavSnapshotVersion = true;
            } else {
                m_candidateWindow.SetSelectedIndex(0);
                *pfEaten = TRUE;
                return true;
            }
        } else if (wParam == VK_LEFT || wParam == VK_UP) {
            if (sel > 0) {
                m_candidateWindow.SetSelectedIndex(sel - 1);
                *pfEaten = TRUE;
                return true;
            } else if (snap.page_index > 0) {
                wParam = VK_PRIOR;
                selectLastOnPageUp = true;
                navSnapshotVersion = snap.candidate_version;
                useNavSnapshotVersion = true;
            } else {
                m_candidateWindow.SetSelectedIndex(count - 1);
                *pfEaten = TRUE;
                return true;
            }
        } else if ((wParam == VK_SPACE || wParam == VK_RETURN) && (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
            if (sel >= 0 && sel < count && sel < 9) {
                if (wParam == VK_RETURN || sel > 0) {
                    wParam = static_cast<WPARAM>('1' + sel);
                    isExplicitCandidateSelect = true;
                }
                navSnapshotVersion = snap.candidate_version;
                useNavSnapshotVersion = true;
            }
        }
    }

    IpcKeyRequest req{};
    req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
    req.session_id = m_sessionId;
    req.context_id = contextId;
    req.context_epoch = static_cast<uint32_t>(EnsureContextEpoch(contextId));
    SetIpcRequestOwnerInstanceId(req, m_ownerInstanceId);
    req.vk_code = static_cast<uint32_t>(wParam);
    req.scan_code = static_cast<uint32_t>(lParam & 0xFF);
    req.modifiers = 0;
    if (!isShiftTap && (GetKeyState(VK_SHIFT) & 0x8000)) req.modifiers |= 1;
    if (GetKeyState(VK_CONTROL) & 0x8000) req.modifiers |= 2;
    if (GetKeyState(VK_MENU) & 0x8000) req.modifiers |= 4;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) req.modifiers |= 8;
    if (GetKeyState(VK_CAPITAL) & 0x0001) req.modifiers |= IPC_MOD_CAPSLOCK;
    req.protocol_version = m_ipcClient.GetPreferredProtocolVersion(m_sessionId);
    if (isExplicitCandidateSelect) {
        req.mode_arg |= IPC_KEYDOWN_EXPLICIT_SELECT;
    }
    if (isShiftTap) {
        req.mode_arg |= IPC_KEYDOWN_SHIFT_TAP;
    }

    uint32_t currentCandVer = 0;
    if (hasExplicitVersion) {
        currentCandVer = explicitCandidateVersion;
    } else if (useNavSnapshotVersion) {
        currentCandVer = navSnapshotVersion;
    } else {
        auto itVer = m_contextCandidateVersions.find(contextId);
        if (itVer != m_contextCandidateVersions.end()) {
            currentCandVer = itVer->second;
        }
    }
    req.candidate_version = currentCandVer;

    IpcKeyResponse resp{};
    bool sentOk = m_ipcClient.SendRequest(req, resp, 80);
    if (sentOk && !resp.handled &&
        req.protocol_version == IPC_PROTOCOL_VERSION_STAGE7 &&
        (resp.status_flags & IPC_STATUS_STAGE7_CAPABLE) == 0) {
        m_ipcClient.MarkStage7Capable(false);
        req.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
        resp = {};
        sentOk = m_ipcClient.SendRequest(req, resp, 80);
    }
    if (sentOk) {
        if (!resp.handled) {
            return false;
        }
        if (isShiftTap && !resp.has_commit && m_pLangBarButton) {
            m_pLangBarButton->UpdateStatus();
        }
        if ((resp.status_flags & IPC_STATUS_HAS_POLICY_REV) != 0) {
            uint32_t advPolicyRevTag = (resp.status_flags & IPC_POLICY_REV_MASK) >> IPC_POLICY_REV_SHIFT;
            ULONGLONG nowTick = GetTickCount64();
            bool tagMismatch = !m_hasSyncedStage7Settings || advPolicyRevTag != m_syncedPolicyRevTag;
            bool freshnessExpired = (nowTick - m_lastStage7SettingsSyncTickMs) >= m_stage7SettingsFreshnessMs;
            if (tagMismatch || freshnessExpired) {
                TrySyncStage7SettingsFromServer(contextId, advPolicyRevTag, freshnessExpired);
            }
        }
        uint64_t currentInputGen = AdvanceContextInputGeneration(contextId);
        uint64_t currentEpoch = EnsureContextEpoch(contextId);

        if (resp.candidate_count > 0 || (resp.has_composition && resp.composition[0] != L'\0')) {
            m_latestServerCandidateVersions[contextId] = resp.candidate_version;
        } else if ((resp.has_composition && resp.composition[0] == L'\0') || resp.has_commit) {
            m_latestServerCandidateVersions[contextId] = 0;
        }

        std::vector<std::wstring> cands;
        for (uint32_t i = 0; i < resp.candidate_count && i < 9; ++i) {
            cands.push_back(resp.candidates[i]);
        }

        auto syncPluginsPostEditIfNeeded = [&]() {
            if (!cands.empty() && (resp.status_flags & IPC_STATUS_HAS_PLUGINS_REV) != 0) {
                uint32_t advRevTag = (resp.status_flags & IPC_PLUGINS_REV_MASK) >> IPC_PLUGINS_REV_SHIFT;
                if (!m_hasSyncedPlugins || advRevTag != m_syncedPluginsRevTag) {
                    TrySyncPluginsFromServer(contextId, advRevTag, false);
                }
            }
        };

        if (resp.has_composition || resp.has_commit) {
            std::shared_ptr<EditRequestControlBlock> controlBlock = std::make_shared<EditRequestControlBlock>();
            controlBlock->edit_request_id = ++m_nextEditRequestId;
            controlBlock->owner_id = m_ownerInstanceId;
            controlBlock->session_id = m_sessionId;
            controlBlock->context_id = contextId;
            controlBlock->context_epoch = static_cast<uint32_t>(currentEpoch);
            controlBlock->input_generation = static_cast<uint32_t>(currentInputGen);
            controlBlock->server_commit_generation =
                (resp.has_commit && resp.candidate_version != 0)
                ? resp.candidate_version
                : static_cast<uint32_t>(currentInputGen);
            controlBlock->commit_event_id = resp.commit_event_id;
            controlBlock->commit_text = resp.commit_text;
            controlBlock->target_context = pic;
            controlBlock->has_commit = (resp.has_commit != 0);
            controlBlock->toggle_chinese_after_commit = (isShiftTap && resp.has_commit != 0);
            controlBlock->was_compositionless =
                (GetContextState(contextId) == CompositionState::CompositionlessComposing) &&
                (GetComposition(pic) == nullptr);

            if (resp.has_commit && resp.commit_event_id != 0) {
                if (m_ipcClient.IsStage7Capable()) {
                    bool prepOk = false;
                    if (!IsWalBarrierFaulted()) {
                        receipt_journal_v2::Entry prepEntry{};
                        prepEntry.owner_id = controlBlock->owner_id;
                        prepEntry.session_id = controlBlock->session_id;
                        prepEntry.context_id = controlBlock->context_id;
                        prepEntry.context_epoch = controlBlock->context_epoch;
                        prepEntry.input_generation = controlBlock->server_commit_generation;
                        prepEntry.edit_request_id = controlBlock->edit_request_id;
                        prepEntry.commit_event_id = controlBlock->commit_event_id;
                        prepEntry.channel = 0; // 0 = TSF EditSession
                        prepEntry.text_len = static_cast<uint32_t>(controlBlock->commit_text.size());
                        prepEntry.text_digest = receipt_journal_v2::TextDigest(controlBlock->commit_text);
                        prepEntry.disposition = static_cast<uint32_t>(IpcCommitDisposition::None);
                        prepOk = receipt_journal_v2::AppendPrepared(prepEntry);
                    }
                    if (!prepOk) {
                        IpcKeyResponse restored{};
                        IpcCommitDisposition finalDisp = IpcCommitDisposition::None;
                        bool conflict = false;
                        if (m_ipcClient.NotifyCommitTerminal(
                                m_sessionId,
                                contextId,
                                resp.commit_event_id,
                                IpcCommitDisposition::CancelRestore,
                                controlBlock->context_epoch,
                                controlBlock->server_commit_generation,
                                m_ownerInstanceId,
                                finalDisp,
                                conflict,
                                &restored)) {
                            m_contextCandidateVersions[contextId] = restored.candidate_version;
                            m_latestServerCandidateVersions[contextId] = restored.candidate_version;
                            m_contextPageIndices[contextId] = restored.page_index;
                            m_contextPageCounts[contextId] = restored.page_count;
                        }
                        return false; // Pause host commit writes when WAL barrier fails.
                    }
                    controlBlock->wal_prepared = true;
                } else if (!receipt_journal::Prepare({m_sessionId, contextId, resp.commit_event_id, 0, 0})) {
                    IpcKeyResponse restored{};
                    if (m_ipcClient.NotifyCommitCancel(m_sessionId, contextId, resp.commit_event_id, &restored)) {
                        m_contextCandidateVersions[contextId] = restored.candidate_version;
                        m_latestServerCandidateVersions[contextId] = restored.candidate_version;
                        m_contextPageIndices[contextId] = restored.page_index;
                        m_contextPageCounts[contextId] = restored.page_count;
                    }
                    return false; // No host edit when the preflight barrier cannot persist.
                }
            }

            CandidateDisplaySnapshot snap{};
            snap.session_id = m_sessionId;
            snap.context_id = contextId;
            snap.pContext = pic;
            snap.input_generation = currentInputGen;
            snap.context_epoch = currentEpoch;
            snap.candidate_version = resp.candidate_version;
            snap.page_index = resp.page_index;
            snap.page_count = resp.page_count;
            snap.candidates = cands;
            snap.initial_selected_index =
                (selectLastOnPageUp && !cands.empty()) ? (static_cast<int>(cands.size()) - 1) : 0;
            snap.mode = (GetContextState(contextId) == CompositionState::CompositionlessComposing)
                        ? CandidateWindowMode::Compositionless
                        : CandidateWindowMode::StandardInline;
            snap.raw_input = resp.has_composition ? resp.composition : L"";
            if (!resp.has_commit && (resp.status_flags & IPC_STATUS_HAS_QUERY_CODE) != 0 && resp.commit_text[0] != L'\0') {
                snap.query_code = resp.commit_text;
            }
            if (!cands.empty() && snap.initial_selected_index >= 0 &&
                snap.initial_selected_index < static_cast<int>(cands.size())) {
                snap.preview_text = CCandidateWindow::ExtractCandidateCoreText(cands[snap.initial_selected_index]);
            }
            if ((resp.status_flags & IPC_STATUS_HAS_CAND_CORES) != 0) {
                constexpr size_t kCandCap = sizeof(resp.candidates[0]) / sizeof(wchar_t);
                snap.candidate_core_texts.reserve(cands.size());
                for (uint32_t i = 0; i < resp.candidate_count && i < 9; ++i) {
                    size_t labelLen = wcsnlen(resp.candidates[i], kCandCap);
                    size_t prefixLen = 0;
                    size_t dotPos = cands[i].find(L". ");
                    if (dotPos != std::wstring::npos && dotPos > 0 && dotPos <= 3) {
                        bool allDigits = true;
                        for (size_t d = 0; d < dotPos; ++d) {
                            if (cands[i][d] < L'0' || cands[i][d] > L'9') {
                                allDigits = false;
                                break;
                            }
                        }
                        if (allDigits) {
                            prefixLen = dotPos + 2;
                        }
                    }

                    std::wstring core;
                    if (labelLen + 2 < kCandCap && resp.candidates[i][labelLen + 1] >= 1) {
                        size_t coreLen = static_cast<size_t>(resp.candidates[i][labelLen + 1] - 1);
                        if (prefixLen + coreLen <= cands[i].length()) {
                            core = cands[i].substr(prefixLen, coreLen);
                        }
                    }
                    if (core.empty() && prefixLen < cands[i].length()) {
                        // Boundary metadata omitted due to item length: strip only index prefix, do NOT guess-strip [...]
                        core = cands[i].substr(prefixLen);
                    }
                    snap.candidate_core_texts.push_back(std::move(core));
                }
                if (!snap.candidate_core_texts.empty() &&
                    snap.initial_selected_index >= 0 &&
                    static_cast<size_t>(snap.initial_selected_index) < snap.candidate_core_texts.size() &&
                    !snap.candidate_core_texts[snap.initial_selected_index].empty()) {
                    snap.preview_text = snap.candidate_core_texts[snap.initial_selected_index];
                }
            }

            // Stage 7.1b: If context is already in CompositionlessComposing and this is not a commit,
            // update UI directly (UiOnlyHandled) without requesting per-key TF_ES_READWRITE locks!
            if (GetContextState(contextId) == CompositionState::CompositionlessComposing && !resp.has_commit) {
                if ((resp.has_composition && resp.composition[0] != L'\0') || !cands.empty()) {
                    EnterCompositionlessMode(pic);
                    snap.mode = CandidateWindowMode::Compositionless;
                    m_contextCandidateVersions[contextId] = resp.candidate_version;
                    m_latestServerCandidateVersions[contextId] = resp.candidate_version;
                    m_contextPageIndices[contextId] = resp.page_index;
                    m_contextPageCounts[contextId] = resp.page_count;
                    if (contextId == m_activeContextId) {
                        m_currentPageIndex = resp.page_index;
                        m_currentPageCount = resp.page_count;
                    }
                    POINT pt{};
                    if (!m_isDeactivated && m_hasFocus && m_activeContextId == contextId &&
                        m_candidateWindow.ShouldShowWindow(snap) &&
                        ResolveAnchorPointForContext(pic, TF_INVALID_EDIT_COOKIE, nullptr, false, &pt)) {
                        m_candidateWindow.Show(snap, pt);
                    } else {
                        HideCandidateWindowIfOwnedBy(contextId);
                    }
                    *pfEaten = TRUE;
                    syncPluginsPostEditIfNeeded();
                    return true;
                } else {
                    // Cleared compositionless preedit (Backspace to empty or Esc): return to Idle (UiOnlyHandled)
                    HideCandidateWindowIfOwnedBy(contextId);
                    ExitCompositionlessMode(contextId);
                    m_contextCandidateVersions[contextId] = 0;
                    m_latestServerCandidateVersions[contextId] = 0;
                    m_contextPageIndices[contextId] = 0;
                    m_contextPageCounts[contextId] = 0;
                    if (contextId == m_activeContextId) {
                        m_currentPageIndex = 0;
                        m_currentPageCount = 0;
                    }
                    m_ipcClient.ResetContext(m_sessionId, contextId);
                    *pfEaten = TRUE;
                    return true;
                }
            }

            uint64_t editGen = ++m_editSessionGeneration;
            m_activeRequestBlocks[controlBlock->edit_request_id] = controlBlock;
            m_pendingEditSessions.emplace_back(
                contextId,
                pic,
                editGen,
                resp.has_composition != 0,
                resp.composition,
                resp.has_commit != 0,
                resp.commit_text,
                resp.commit_event_id,
                snap,
                PendingEditStatus::PendingSync,
                controlBlock);

            CEditSession* pSession = new CEditSession(
                this, pic, &m_candidateWindow,
                resp.has_composition != 0, resp.composition,
                resp.has_commit != 0, resp.commit_text,
                cands,
                editGen,
                &snap,
                controlBlock
            );

            HRESULT hrSession = S_OK;
            HRESULT hr = E_FAIL;
            {
                struct SyncEditGuard {
                    int& depth;
                    explicit SyncEditGuard(int& d) : depth(d) { ++depth; }
                    ~SyncEditGuard() { if (depth > 0) --depth; }
                } syncGuard{m_syncEditSessionDepth};
                hr = pic->RequestEditSession(m_tfClientId, pSession, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
            }
            bool isSyncBlocked = (hr == TF_E_SYNCHRONOUS || hrSession == TF_E_SYNCHRONOUS);
            if (isSyncBlocked) {
                bool sinkAdvised = EnsureTextEditSinkAdvised(pic);
                if (!sinkAdvised && resp.has_commit && GetComposition(pic) == nullptr) {
                    // Cannot monitor same-context selection changes for an unguarded compositionless async commit
                    OnEditSessionCompleted(contextId, editGen, EditSessionResult::Unmodified, E_ACCESSDENIED);
                    if (controlBlock) {
                        ProcessPendingUnicodeInjectionForRequest(controlBlock->edit_request_id);
                    }
                    pSession->Release();
                    *pfEaten = FALSE;
                    return false;
                }
                for (auto& pe : m_pendingEditSessions) {
                    if (pe.contextId == contextId && pe.generation == editGen) {
                        pe.status = PendingEditStatus::QueuedAsync;
                        break;
                    }
                }
                hrSession = S_OK;
                hr = pic->RequestEditSession(m_tfClientId, pSession, TF_ES_ASYNC | TF_ES_READWRITE, &hrSession);
                if (FAILED(hr) || FAILED(hrSession)) {
                    if (!pSession->HasExecuted()) {
                        OnEditSessionCompleted(contextId, editGen, EditSessionResult::Unmodified, FAILED(hr) ? hr : hrSession);
                    }
                    if (controlBlock) {
                        InjectionOutcome injOutcome = InjectionOutcome::NotAttempted;
                        if (ProcessPendingUnicodeInjectionForRequest(controlBlock->edit_request_id, &injOutcome) &&
                            (injOutcome == InjectionOutcome::DispatchedUnverified || injOutcome == InjectionOutcome::Unknown)) {
                            pSession->Release();
                            *pfEaten = TRUE;
                            return true;
                        }
                    }
                    pSession->Release();
                    *pfEaten = FALSE;
                    return false;
                }
                if (!pSession->HasExecuted()) {
                    // Request is queued in host (TF_S_ASYNC); start drain/observation timer!
                    StartDrainTimer();
                    pSession->Release();
                    *pfEaten = TRUE;
                    return true;
                }
                EditSessionResult asyncResult = pSession->GetResult();
                pSession->Release();
                if (asyncResult == EditSessionResult::Success ||
                    asyncResult == EditSessionResult::UiOnlyHandled ||
                    asyncResult == EditSessionResult::PartiallyModified ||
                    asyncResult == EditSessionResult::Indeterminate) {
                    *pfEaten = TRUE;
                    if (asyncResult != EditSessionResult::Indeterminate) {
                        syncPluginsPostEditIfNeeded();
                    }
                    return true;
                }
                if (controlBlock) {
                    InjectionOutcome injOutcome = InjectionOutcome::NotAttempted;
                    if (ProcessPendingUnicodeInjectionForRequest(controlBlock->edit_request_id, &injOutcome) &&
                        (injOutcome == InjectionOutcome::DispatchedUnverified || injOutcome == InjectionOutcome::Unknown)) {
                        *pfEaten = TRUE;
                        return true;
                    }
                }
                *pfEaten = FALSE;
                return false;
            }

            if (!pSession->HasExecuted()) {
                OnEditSessionCompleted(contextId, editGen, EditSessionResult::Unmodified, FAILED(hr) ? hr : hrSession);
                if (controlBlock) {
                    InjectionOutcome injOutcome = InjectionOutcome::NotAttempted;
                    if (ProcessPendingUnicodeInjectionForRequest(controlBlock->edit_request_id, &injOutcome) &&
                        (injOutcome == InjectionOutcome::DispatchedUnverified || injOutcome == InjectionOutcome::Unknown)) {
                        pSession->Release();
                        *pfEaten = TRUE;
                        return true;
                    }
                }
                pSession->Release();
                *pfEaten = FALSE;
                return false;
            }

            EditSessionResult result = pSession->GetResult();
            pSession->Release();

            if (SUCCEEDED(hr) && SUCCEEDED(hrSession) &&
                (result == EditSessionResult::Success || result == EditSessionResult::UiOnlyHandled)) {
                *pfEaten = TRUE;
                syncPluginsPostEditIfNeeded();
                return true;
            } else if (result == EditSessionResult::PartiallyModified) {
                *pfEaten = TRUE;
                syncPluginsPostEditIfNeeded();
                return true;
            } else if (result == EditSessionResult::Indeterminate) {
                *pfEaten = TRUE;
                return true;
            } else {
                if (controlBlock) {
                    InjectionOutcome injOutcome = InjectionOutcome::NotAttempted;
                    if (ProcessPendingUnicodeInjectionForRequest(controlBlock->edit_request_id, &injOutcome) &&
                        (injOutcome == InjectionOutcome::DispatchedUnverified || injOutcome == InjectionOutcome::Unknown)) {
                        *pfEaten = TRUE;
                        return true;
                    }
                }
                *pfEaten = FALSE;
                return false;
            }
        } else {
            *pfEaten = TRUE;
            return true;
        }
    }

    return false;
}

STDMETHODIMP CTextInputProcessor::OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (!pfEaten) return E_POINTER;
    *pfEaten = FALSE;
    if (IsShiftKeyCode(wParam)) {
        UpdateShiftKeyState(wParam, lParam, false);
        bool allShiftsReleased = !m_leftShiftDown && !m_rightShiftDown && !m_genericShiftDown;
        bool otherMods = ((GetKeyState(VK_CONTROL) | GetKeyState(VK_MENU) |
                           GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
        uint64_t contextId = reinterpret_cast<uint64_t>(pic);
        if (m_shiftTapPending && allShiftsReleased && !otherMods &&
            !ShouldFilterKeystroke(wParam) && !IsContextReadOnly(pic) &&
            !HasQueuedEditSession(contextId) && !IsContextQuarantined(contextId)) {
            *pfEaten = TRUE;
        } else if (allShiftsReleased) {
            m_shiftTapPending = false;
        }
    }
    return S_OK;
}

STDMETHODIMP CTextInputProcessor::OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (!pfEaten) return E_POINTER;
    *pfEaten = FALSE;
    if (IsShiftKeyCode(wParam) && m_shiftTapPending) {
        m_shiftTapPending = false;
        BOOL dummyEaten = FALSE;
        ProcessKeyInternal(pic, VK_SHIFT, lParam, &dummyEaten, false, 0, true);
    }
    return S_OK;
}

STDMETHODIMP CTextInputProcessor::OnPreservedKey(ITfContext* pic, REFGUID rguid, BOOL* pfEaten) {
    if (!pfEaten) return E_POINTER;
    *pfEaten = FALSE;
    return S_OK;
}

STDMETHODIMP CTextInputProcessor::OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) {
    if (!pComposition) return S_OK;

    // 1. Check active composition
    if (m_pComposition == pComposition) {
        uint64_t terminatedContextId = m_compositionContextId;
        SuppressQueuedEditSessionsForContext(terminatedContextId, true);
        SetComposition(nullptr, nullptr, CompositionState::None);
        m_contextCandidateVersions[terminatedContextId] = 0;
        m_latestServerCandidateVersions[terminatedContextId] = 0;
        m_contextPageIndices[terminatedContextId] = 0;
        m_contextPageCounts[terminatedContextId] = 0;
        m_ipcClient.ResetContext(m_sessionId, terminatedContextId);
        HideCandidateWindowIfOwnedBy(terminatedContextId);
        return S_OK;
    }

    // 2. Check pending terminations
    for (auto it = m_pendingTerminations.begin(); it != m_pendingTerminations.end(); ++it) {
        if (it->pComp == pComposition) {
            uint64_t terminatedContextId = it->contextId;
            SuppressQueuedEditSessionsForContext(terminatedContextId, true);
            m_contextCandidateVersions[terminatedContextId] = 0;
            m_latestServerCandidateVersions[terminatedContextId] = 0;
            m_contextPageIndices[terminatedContextId] = 0;
            m_contextPageCounts[terminatedContextId] = 0;
            m_ipcClient.ResetContext(m_sessionId, terminatedContextId);
            HideCandidateWindowIfOwnedBy(terminatedContextId);
            m_pendingTerminations.erase(it);
            return S_OK;
        }
    }

    // 3. Stale or late notification for unknown / already-finalized composition: safely ignored
    s_hitLateNotificationIgnored.fetch_add(1, std::memory_order_relaxed);
    return S_OK;
}

uint64_t CTextInputProcessor::EnsureContextEpoch(uint64_t contextId) {
    if (contextId == 0) return 0;
    auto it = m_contextEpochs.find(contextId);
    if (it == m_contextEpochs.end() || it->second == 0) {
        uint64_t newEpoch = 1;
        m_contextEpochs[contextId] = newEpoch;
        return newEpoch;
    }
    return it->second;
}

uint64_t CTextInputProcessor::GetContextEpoch(uint64_t contextId) const {
    if (contextId == 0) return 0;
    auto it = m_contextEpochs.find(contextId);
    return (it != m_contextEpochs.end()) ? it->second : 0;
}

uint64_t CTextInputProcessor::AdvanceContextEpoch(uint64_t contextId) {
    if (contextId == 0) return 0;
    uint64_t& epoch = m_contextEpochs[contextId];
    epoch = (epoch == 0) ? 1 : (epoch + 1);
    return epoch;
}

uint64_t CTextInputProcessor::GetContextInputGeneration(uint64_t contextId) const {
    if (contextId == 0) return 0;
    auto it = m_contextInputGenerations.find(contextId);
    return (it != m_contextInputGenerations.end()) ? it->second : 0;
}

uint64_t CTextInputProcessor::AdvanceContextInputGeneration(uint64_t contextId) {
    if (contextId == 0) return 0;
    uint64_t& gen = m_contextInputGenerations[contextId];
    gen++;
    return gen;
}

CompositionState CTextInputProcessor::GetContextState(uint64_t contextId) const {
    if (contextId == 0) return CompositionState::None;
    auto it = m_contextStates.find(contextId);
    return (it != m_contextStates.end()) ? it->second : CompositionState::None;
}

void CTextInputProcessor::SetContextState(uint64_t contextId, CompositionState state) {
    if (contextId == 0) return;
    if (state == CompositionState::None) {
        m_contextStates.erase(contextId);
    } else {
        m_contextStates[contextId] = state;
    }
}

CandidateWindowMode CTextInputProcessor::GetContextMode(uint64_t contextId) const {
    if (contextId == 0) return CandidateWindowMode::StandardInline;
    auto it = m_contextModes.find(contextId);
    return (it != m_contextModes.end()) ? it->second : CandidateWindowMode::StandardInline;
}

void CTextInputProcessor::SetContextMode(uint64_t contextId, CandidateWindowMode mode) {
    if (contextId == 0) return;
    if (mode == CandidateWindowMode::StandardInline) {
        m_contextModes.erase(contextId);
    } else {
        m_contextModes[contextId] = mode;
    }
}

void CTextInputProcessor::EnterCompositionlessMode(ITfContext* pContext) {
    if (!pContext) return;
    uint64_t contextId = reinterpret_cast<uint64_t>(pContext);
    EnsureContextEpoch(contextId);
    m_compositionlessContextLeases[contextId] = pContext;
    SetContextState(contextId, CompositionState::CompositionlessComposing);
    SetContextMode(contextId, CandidateWindowMode::Compositionless);
    EnsureTextEditSinkAdvised(pContext);
}

void CTextInputProcessor::ExitCompositionlessMode(uint64_t contextId) {
    if (contextId == 0) return;
    if (GetContextState(contextId) == CompositionState::CompositionlessComposing) {
        SetContextState(contextId, CompositionState::None);
    }
    SetContextMode(contextId, CandidateWindowMode::StandardInline);
    m_compositionlessContextLeases.erase(contextId);
    if (!HasQueuedEditSession(contextId)) {
        UnadviseTextEditSink(contextId);
    }
}

bool CTextInputProcessor::EnsureTextEditSinkAdvised(ITfContext* pic) {
    if (!pic) return false;
    uint64_t contextId = reinterpret_cast<uint64_t>(pic);
    if (m_textEditSinks.find(contextId) != m_textEditSinks.end()) {
        return true;
    }
    Microsoft::WRL::ComPtr<ITfSource> pSource;
    if (SUCCEEDED(pic->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(pSource.GetAddressOf()))) && pSource) {
        DWORD cookie = TF_INVALID_COOKIE;
        if (SUCCEEDED(pSource->AdviseSink(IID_ITfTextEditSink, static_cast<ITfTextEditSink*>(this), &cookie)) &&
            cookie != TF_INVALID_COOKIE) {
            TextEditSinkBinding binding;
            binding.context = pic;
            binding.cookie = cookie;
            m_textEditSinks[contextId] = std::move(binding);
            return true;
        }
    }
    return false;
}

bool CTextInputProcessor::HasTextEditSinkAdvised(uint64_t contextId) const {
    return m_textEditSinks.find(contextId) != m_textEditSinks.end();
}

void CTextInputProcessor::UnadviseTextEditSink(uint64_t contextId) {
    auto it = m_textEditSinks.find(contextId);
    if (it == m_textEditSinks.end()) return;
    TextEditSinkBinding binding = std::move(it->second);
    m_textEditSinks.erase(it);
    if (binding.context && binding.cookie != TF_INVALID_COOKIE) {
        Microsoft::WRL::ComPtr<ITfSource> pSource;
        if (SUCCEEDED(binding.context->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(pSource.GetAddressOf()))) && pSource) {
            pSource->UnadviseSink(binding.cookie);
        }
    }
}

void CTextInputProcessor::UnadviseAllTextEditSinks() {
    auto sinks = std::move(m_textEditSinks);
    m_textEditSinks.clear();
    for (auto& kv : sinks) {
        if (kv.second.context && kv.second.cookie != TF_INVALID_COOKIE) {
            Microsoft::WRL::ComPtr<ITfSource> pSource;
            if (SUCCEEDED(kv.second.context->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(pSource.GetAddressOf()))) && pSource) {
                pSource->UnadviseSink(kv.second.cookie);
            }
        }
    }
}

STDMETHODIMP CTextInputProcessor::OnEndEdit(ITfContext* pic, TfEditCookie /*ecReadOnly*/, ITfEditRecord* pEditRecord) {
    if (!pic || !pEditRecord || m_internalEditDepth > 0) {
        return S_OK;
    }
    if (m_tfClientId != TF_CLIENTID_NULL) {
        BOOL fInWriteSession = FALSE;
        if (SUCCEEDED(pic->InWriteSession(m_tfClientId, &fInWriteSession)) && fInWriteSession) {
            return S_OK;
        }
    }
    uint64_t contextId = reinterpret_cast<uint64_t>(pic);
    bool isTracked =
        (contextId == m_activeContextId) ||
        HasQueuedEditSession(contextId) ||
        (GetContextState(contextId) == CompositionState::CompositionlessComposing);
    if (!isTracked) {
        return S_OK;
    }

    BOOL fSelectionChanged = FALSE;
    pEditRecord->GetSelectionStatus(&fSelectionChanged);

    bool hasExternalTextChange = false;
    IEnumTfRanges* pEnumRanges = nullptr;
    if (SUCCEEDED(pEditRecord->GetTextAndPropertyUpdates(TF_GTP_INCL_TEXT, nullptr, 0, &pEnumRanges)) && pEnumRanges) {
        ITfRange* pTmp = nullptr;
        ULONG fetched = 0;
        if (pEnumRanges->Next(1, &pTmp, &fetched) == S_OK && fetched > 0 && pTmp) {
            hasExternalTextChange = true;
            pTmp->Release();
        }
        pEnumRanges->Release();
    }

    if (fSelectionChanged || hasExternalTextChange) {
        Microsoft::WRL::ComPtr<ITfTextInputProcessorEx> selfGuard(this);
        AdvanceContextInputGeneration(contextId);
        SuppressQueuedEditSessionsForContext(contextId, true);
        if (GetContextState(contextId) == CompositionState::CompositionlessComposing) {
            ResetCompositionlessContext(contextId, false, true);
        } else {
            HideCandidateWindowIfOwnedBy(contextId);
        }
    }
    return S_OK;
}

bool CTextInputProcessor::HasCompositionlessLease(uint64_t contextId, ITfContext* pContext) const {
    if (contextId == 0 || !pContext) return false;
    auto it = m_compositionlessContextLeases.find(contextId);
    return it != m_compositionlessContextLeases.end() && it->second.Get() == pContext;
}

void CTextInputProcessor::ResetCompositionlessContext(uint64_t contextId, bool advanceEpoch, bool resetServer) {
    if (contextId == 0) return;
    SuppressQueuedEditSessionsForContext(contextId, true);
    HideCandidateWindowIfOwnedBy(contextId);
    ExitCompositionlessMode(contextId);
    m_cachedAnchors.erase(contextId);
    m_contextCandidateVersions[contextId] = 0;
    m_latestServerCandidateVersions[contextId] = 0;
    m_contextPageIndices[contextId] = 0;
    m_contextPageCounts[contextId] = 0;
    if (contextId == m_activeContextId) {
        m_currentPageIndex = 0;
        m_currentPageCount = 0;
    }
    AdvanceContextInputGeneration(contextId);
    if (advanceEpoch) {
        AdvanceContextEpoch(contextId);
    }
    if (resetServer) {
        m_ipcClient.ResetContext(m_sessionId, contextId);
    }
}

void CTextInputProcessor::ResetAllCompositionlessContexts(bool resetServer) {
    std::vector<uint64_t> ctxIds;
    for (const auto& kv : m_compositionlessContextLeases) {
        ctxIds.push_back(kv.first);
    }
    for (const auto& kv : m_contextStates) {
        if (kv.second == CompositionState::CompositionlessComposing) {
            if (std::find(ctxIds.begin(), ctxIds.end(), kv.first) == ctxIds.end()) {
                ctxIds.push_back(kv.first);
            }
        }
    }
    for (uint64_t ctxId : ctxIds) {
        ResetCompositionlessContext(ctxId, true, resetServer);
    }
}

void CTextInputProcessor::CacheContextAnchorPoint(uint64_t contextId, uint64_t epoch, uint64_t viewId, POINT pt) {
    if (contextId == 0 || epoch == 0) return;
    CachedAnchorPoint cap{};
    cap.contextEpoch = epoch;
    cap.viewId = viewId;
    cap.pt = pt;
    cap.valid = true;
    m_cachedAnchors[contextId] = cap;
}

bool CTextInputProcessor::ResolveAnchorPointForContext(
    ITfContext* pContext,
    TfEditCookie ec,
    ITfRange* pRange,
    bool hasValidEditCookie,
    POINT* outPt)
{
    if (!pContext || !outPt) return false;
    uint64_t contextId = reinterpret_cast<uint64_t>(pContext);
    uint64_t epoch = EnsureContextEpoch(contextId);

    ITfContextView* pView = nullptr;
    HRESULT hrView = pContext->GetActiveView(&pView);
    uint64_t viewId = reinterpret_cast<uint64_t>(pView);

    // 1. If in a valid edit session, try GetTextExt on pRange or current selection range
    if (hasValidEditCookie && SUCCEEDED(hrView) && pView) {
        RECT rc{};
        BOOL fClipped = FALSE;
        bool gotExt = false;
        if (pRange) {
            if (SUCCEEDED(pView->GetTextExt(ec, pRange, &rc, &fClipped)) &&
                rc.right >= rc.left && rc.bottom > rc.top) {
                gotExt = true;
            }
        } else {
            TF_SELECTION sel{};
            ULONG cFetched = 0;
            if (SUCCEEDED(pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &cFetched)) &&
                cFetched > 0 && sel.range) {
                if (SUCCEEDED(pView->GetTextExt(ec, sel.range, &rc, &fClipped)) &&
                    rc.right >= rc.left && rc.bottom > rc.top) {
                    gotExt = true;
                }
                sel.range->Release();
            }
        }
        if (gotExt) {
            POINT pt = { rc.left, rc.bottom + 2 };
            CacheContextAnchorPoint(contextId, epoch, viewId, pt);
            *outPt = pt;
            pView->Release();
            return true;
        }
    }

    // 2. Fallback to cached anchor point for the same (context_id, context_epoch, view)
    auto itCache = m_cachedAnchors.find(contextId);
    if (itCache != m_cachedAnchors.end() &&
        itCache->second.valid &&
        itCache->second.contextEpoch == epoch &&
        (viewId == 0 || itCache->second.viewId == 0 || itCache->second.viewId == viewId)) {
        *outPt = itCache->second.pt;
        if (pView) pView->Release();
        return true;
    }

    // 3. Fallback to view HWND or same-process focus HWND rectangle clamped to monitor work area
    HWND hWndView = nullptr;
    if (pView) {
        pView->GetWnd(&hWndView);
    }
    if (!hWndView || !IsWindow(hWndView) || !IsWindowVisible(hWndView)) {
        HWND hFocus = GetFocus();
        if (hFocus && IsWindow(hFocus) && IsWindowVisible(hFocus)) {
            DWORD focusPid = 0;
            GetWindowThreadProcessId(hFocus, &focusPid);
            if (focusPid == GetCurrentProcessId()) {
                hWndView = hFocus;
            }
        }
    }
    if (hWndView && IsWindow(hWndView) && IsWindowVisible(hWndView)) {
        RECT rcWnd{};
        if (GetWindowRect(hWndView, &rcWnd) && rcWnd.right > rcWnd.left && rcWnd.bottom > rcWnd.top) {
            HMONITOR hMon = MonitorFromWindow(hWndView, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(hMon, &mi);
            POINT pt = { rcWnd.left + 24, rcWnd.top + 48 };
            if (pt.x < mi.rcWork.left) pt.x = mi.rcWork.left;
            if (pt.x > mi.rcWork.right - 120) pt.x = mi.rcWork.right - 120;
            if (pt.y < mi.rcWork.top) pt.y = mi.rcWork.top;
            if (pt.y > mi.rcWork.bottom - 60) pt.y = mi.rcWork.bottom - 60;
            CacheContextAnchorPoint(contextId, epoch, viewId, pt);
            *outPt = pt;
            if (pView) pView->Release();
            return true;
        }
    }

    if (pView) {
        pView->Release();
    }

    // 4. Conservative default position within the active monitor's work area
    POINT cursorPt{ 0, 0 };
    GetCursorPos(&cursorPt);
    HMONITOR hMon = MonitorFromPoint(cursorPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi) && mi.rcWork.right > mi.rcWork.left && mi.rcWork.bottom > mi.rcWork.top) {
        POINT pt = { mi.rcWork.left + 80, mi.rcWork.bottom - 120 };
        *outPt = pt;
        return true;
    }

    *outPt = { 80, 80 };
    return true;
}
