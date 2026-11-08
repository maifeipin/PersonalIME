#include "text_input_processor.h"
#include "lang_bar_item.h"
#include "edit_session.h"
#include "globals.h"
#include "../common/ime_guids.h"
#include <wrl/client.h>
#include <io.h>
#include <sstream>
#include <shellapi.h>
#include <filesystem>
#include "receipt_journal.h"
#include "receipt_retry.h"

static LONG64 s_sessionCounter = 0;

CTextInputProcessor::CTextInputProcessor() {
    m_cRef = 1;
    uint64_t pid = static_cast<uint64_t>(GetCurrentProcessId());
    uint64_t counter = static_cast<uint64_t>(InterlockedIncrement64(&s_sessionCounter));
    m_sessionId = (pid << 32) | (counter & 0xFFFFFFFF);
    DllAddRef();

    m_candidateWindow.SetOnOpenManager([this]() {
        // Cleanly terminate in-progress composition so host document has no stray underline
        TerminateCompositionOnHost();
        OpenManagerFromTray();
    });

    m_candidateWindow.SetOnExecutePlugin([this](const CandidatePluginItem& item) {
        TerminateCompositionOnHost();
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
        SubmitCandidateSelectionFromSnapshot(snap, index);
    });

    m_candidateWindow.SetOnPageScrollSnapshot([this](const CandidateDisplaySnapshot& snap, bool nextPage) {
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
        m_compState = state;
        return;
    }
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

    if (result == EditSessionResult::Success) {
        if (pending.terminateAfterEdit && !pending.hasCommit) {
            HideCandidateWindowIfOwnedBy(contextId);
            applySnapshotState(0, 0, 0);
            m_ipcClient.ResetContext(m_sessionId, contextId);
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
        }

        if (pending.hasCommit && pending.commitEventId != 0) {
            EnqueueUnconfirmedCommit(contextId, pending.commitEventId, UnconfirmedAction::Success);
            DrainUnconfirmedCommits(true);
        }

        if (pending.terminateAfterEdit || pending.uiSuppressed) {
            HideCandidateWindowIfOwnedBy(contextId);
        }
    } else if (result == EditSessionResult::PartiallyModified) {
        HideCandidateWindowIfOwnedBy(contextId);
        if ((pending.hasComposition && pending.composition.empty()) || pending.hasCommit) {
            applySnapshotState(0, 0, 0);
        } else if (!pending.displaySnapshot.candidates.empty() ||
                   (pending.hasComposition && !pending.composition.empty())) {
            applySnapshotState(
                pending.displaySnapshot.candidate_version,
                pending.displaySnapshot.page_index,
                pending.displaySnapshot.page_count);
        }

        if (pending.hasCommit && pending.commitEventId != 0) {
            EnqueueUnconfirmedCommit(contextId, pending.commitEventId, UnconfirmedAction::Success);
            DrainUnconfirmedCommits(true);
        }
    } else {
        // Unmodified or RecoveryBlocked: current key text was NOT applied to host!
        if (result == EditSessionResult::RecoveryBlocked) {
            s_hitRecoveryBlocked.fetch_add(1, std::memory_order_relaxed);
        }
        HideCandidateWindowIfOwnedBy(contextId);

        IpcKeyResponse cancelOrRollbackResp{};
        if (pending.hasCommit && pending.commitEventId != 0) {
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
            } else {
                if (cancelOrRollbackResp.candidate_count > 0 ||
                    (cancelOrRollbackResp.has_composition && cancelOrRollbackResp.composition[0] != L'\0')) {
                    applySnapshotState(
                        cancelOrRollbackResp.candidate_version,
                        cancelOrRollbackResp.page_index,
                        cancelOrRollbackResp.page_count);
                } else {
                    applySnapshotState(0, 0, 0);
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
                }
            } else {
                applySnapshotState(0, 0, 0);
            }
        }

        if (pending.terminateAfterEdit && pending.pContext) {
            bool termOk = TerminateCompositionOnHost(pending.pContext);
            if (termOk && GetComposition(pending.pContext) == nullptr &&
                GetPendingStatus(contextId) == PendingTerminationStatus::Succeeded) {
                applySnapshotState(0, 0, 0);
                m_ipcClient.ResetContext(m_sessionId, contextId);
            }
        }
    }
}

bool CTextInputProcessor::IsDisplayedSnapshotValid(
    const CandidateDisplaySnapshot& snap,
    ITfContext* expectedContext) const
{
    if (m_isDeactivated || !m_hasFocus) return false;
    if (snap.session_id == 0 || snap.session_id != m_sessionId) return false;
    if (snap.context_id == 0 || snap.context_id != m_activeContextId || snap.pContext == nullptr) return false;
    if (expectedContext != nullptr &&
        (snap.pContext != expectedContext || snap.context_id != reinterpret_cast<uint64_t>(expectedContext))) {
        return false;
    }
    if (m_pCompositionContext != snap.pContext || m_pComposition == nullptr) return false;
    if (m_candidateWindow.GetCandidateCount() <= 0) return false;

    const auto& winSnap = m_candidateWindow.GetSnapshot();
    if (winSnap.session_id != snap.session_id ||
        winSnap.context_id != snap.context_id ||
        winSnap.candidate_version != snap.candidate_version ||
        winSnap.page_index != snap.page_index) {
        return false;
    }

    if (HasPendingEditSession(snap.context_id)) return false;
    if (GetPendingStatus(snap.context_id) != PendingTerminationStatus::Succeeded) return false;
    if (snap.candidate_version == 0) return false;

    auto itVer = m_contextCandidateVersions.find(snap.context_id);
    if (itVer == m_contextCandidateVersions.end() || itVer->second != snap.candidate_version) {
        return false;
    }

    auto itSrvVer = m_latestServerCandidateVersions.find(snap.context_id);
    if (itSrvVer != m_latestServerCandidateVersions.end() && itSrvVer->second != snap.candidate_version) {
        return false;
    }

    auto itPage = m_contextPageIndices.find(snap.context_id);
    if (itPage != m_contextPageIndices.end() && itPage->second != snap.page_index) {
        return false;
    }

    return true;
}

bool CTextInputProcessor::SubmitCandidateSelectionFromSnapshot(const CandidateDisplaySnapshot& snap, int index) {
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
    }

    if (*ppvObj) {
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
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

    return S_OK;
}

STDMETHODIMP CTextInputProcessor::ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) {
    return Activate(ptim, tid);
}

STDMETHODIMP CTextInputProcessor::Deactivate() {
    m_isDeactivated = true;
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

STDMETHODIMP CTextInputProcessor::OnSetFocus(BOOL fForeground) {
    m_hasFocus = (fForeground != FALSE);
    if (!fForeground) {
        m_candidateWindow.Hide();
        SuppressAllQueuedEditSessions(true);
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
        return S_OK;
    }

    uint64_t contextId = reinterpret_cast<uint64_t>(pic);
    if (HasQueuedEditSession(contextId)) {
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
    req.vk_code = static_cast<uint32_t>(wParam);
    req.scan_code = static_cast<uint32_t>(lParam & 0xFF);
    req.modifiers = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) req.modifiers |= 1;
    if (GetKeyState(VK_CONTROL) & 0x8000) req.modifiers |= 2;
    if (GetKeyState(VK_MENU) & 0x8000) req.modifiers |= 4;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) req.modifiers |= 8;
    req.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;

    IpcKeyResponse resp{};
    if (m_ipcClient.SendRequest(req, resp, 50)) {
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
void CTextInputProcessor::DrainUnconfirmedCommits(bool forceNow) {
    constexpr size_t MAX_QUEUE_SIZE = 512;
    if (m_unconfirmedCommits.empty()) {
        if (!HasPendingSpilloverRecords()) {
            StopDrainTimer();
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

    if (m_unconfirmedCommits.empty() && !HasPendingSpilloverRecords()) {
        StopDrainTimer();
    } else {
        StartDrainTimer();
    }
}

STDMETHODIMP CTextInputProcessor::OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (!pfEaten) return E_POINTER;
    *pfEaten = FALSE;
    ProcessKeyInternal(pic, wParam, lParam, pfEaten, false, 0);
    return S_OK;
}

bool CTextInputProcessor::ProcessKeyInternal(
    ITfContext* pic,
    WPARAM wParam,
    LPARAM lParam,
    BOOL* pfEaten,
    bool hasExplicitVersion,
    uint32_t explicitCandidateVersion)
{
    if (!pfEaten) return false;
    *pfEaten = FALSE;

    DrainUnconfirmedCommits();
    if (receipt_retry::Pending()) return false;

    if (ShouldFilterKeystroke(wParam) || IsContextReadOnly(pic)) {
        return false;
    }

    uint64_t contextId = reinterpret_cast<uint64_t>(pic);

    // Do not overlap new keystrokes while an asynchronous edit session is queued on this context
    if (HasQueuedEditSession(contextId)) {
        *pfEaten = FALSE;
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
    req.vk_code = static_cast<uint32_t>(wParam);
    req.scan_code = static_cast<uint32_t>(lParam & 0xFF);
    req.modifiers = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) req.modifiers |= 1;
    if (GetKeyState(VK_CONTROL) & 0x8000) req.modifiers |= 2;
    if (GetKeyState(VK_MENU) & 0x8000) req.modifiers |= 4;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) req.modifiers |= 8;
    req.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
    if (isExplicitCandidateSelect) {
        req.mode_arg |= IPC_KEYDOWN_EXPLICIT_SELECT;
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
    if (m_ipcClient.SendRequest(req, resp, 80)) {
        if (!resp.handled) {
            return false;
        }

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
            if (resp.has_commit && resp.commit_event_id != 0 &&
                !receipt_journal::Prepare({m_sessionId, contextId, resp.commit_event_id, 0, 0})) {
                IpcKeyResponse restored{};
                if (m_ipcClient.NotifyCommitCancel(m_sessionId, contextId, resp.commit_event_id, &restored)) {
                    m_contextCandidateVersions[contextId] = restored.candidate_version;
                    m_latestServerCandidateVersions[contextId] = restored.candidate_version;
                    m_contextPageIndices[contextId] = restored.page_index;
                    m_contextPageCounts[contextId] = restored.page_count;
                }
                return false; // No host edit when the preflight barrier cannot persist.
            }

            CandidateDisplaySnapshot snap{};
            snap.session_id = m_sessionId;
            snap.context_id = contextId;
            snap.pContext = pic;
            snap.candidate_version = resp.candidate_version;
            snap.page_index = resp.page_index;
            snap.page_count = resp.page_count;
            snap.candidates = cands;
            snap.initial_selected_index =
                (selectLastOnPageUp && !cands.empty()) ? (static_cast<int>(cands.size()) - 1) : 0;

            uint64_t editGen = ++m_editSessionGeneration;
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
                PendingEditStatus::PendingSync);

            CEditSession* pSession = new CEditSession(
                this, pic, &m_candidateWindow,
                resp.has_composition != 0, resp.composition,
                resp.has_commit != 0, resp.commit_text,
                cands,
                editGen,
                &snap
            );

            HRESULT hrSession = S_OK;
            HRESULT hr = pic->RequestEditSession(m_tfClientId, pSession, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
            bool isSyncBlocked = (hr == TF_E_SYNCHRONOUS || hrSession == TF_E_SYNCHRONOUS);
            if (isSyncBlocked) {
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
                    pSession->Release();
                    *pfEaten = FALSE;
                    return false;
                }
                if (!pSession->HasExecuted()) {
                    // Request is queued in host (TF_S_ASYNC); completion will be handled when DoEditSession runs!
                    pSession->Release();
                    *pfEaten = TRUE;
                    return true;
                }
                EditSessionResult asyncResult = pSession->GetResult();
                pSession->Release();
                if (asyncResult == EditSessionResult::Success || asyncResult == EditSessionResult::PartiallyModified) {
                    *pfEaten = TRUE;
                    syncPluginsPostEditIfNeeded();
                    return true;
                }
                *pfEaten = FALSE;
                return false;
            }

            if (!pSession->HasExecuted()) {
                OnEditSessionCompleted(contextId, editGen, EditSessionResult::Unmodified, FAILED(hr) ? hr : hrSession);
                pSession->Release();
                *pfEaten = FALSE;
                return false;
            }

            EditSessionResult result = pSession->GetResult();
            pSession->Release();

            if (SUCCEEDED(hr) && SUCCEEDED(hrSession) && result == EditSessionResult::Success) {
                *pfEaten = TRUE;
                syncPluginsPostEditIfNeeded();
                return true;
            } else if (result == EditSessionResult::PartiallyModified) {
                *pfEaten = TRUE;
                syncPluginsPostEditIfNeeded();
                return true;
            } else {
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
    return S_OK;
}

STDMETHODIMP CTextInputProcessor::OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (!pfEaten) return E_POINTER;
    *pfEaten = FALSE;
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
