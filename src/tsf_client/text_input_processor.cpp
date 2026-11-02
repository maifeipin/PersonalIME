#include "text_input_processor.h"
#include "edit_session.h"
#include "globals.h"
#include "../common/ime_guids.h"
#include <wrl/client.h>

static LONG64 s_sessionCounter = 0;

CTextInputProcessor::CTextInputProcessor() {
    m_cRef = 1;
    m_sessionId = static_cast<uint64_t>(InterlockedIncrement64(&s_sessionCounter));
    DllAddRef();
}

CTextInputProcessor::~CTextInputProcessor() {
    // Final Release must not create sessions that AddRef a TIP already being destroyed.
    UninitKeyEventSink();
    SetComposition(nullptr, nullptr, CompositionState::None);
    m_pendingTerminations.clear();

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

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfTextInputProcessor)) {
        *ppvObj = static_cast<ITfTextInputProcessor*>(this);
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

    m_pThreadMgr = ptim;
    m_pThreadMgr->AddRef();
    m_tfClientId = tid;

    if (!InitKeyEventSink()) {
        Deactivate();
        return E_FAIL;
    }

    return S_OK;
}

STDMETHODIMP CTextInputProcessor::Deactivate() {
    UninitKeyEventSink();

    m_candidateWindow.Hide();
    m_candidateWindow.Destroy();

    if (m_pCompositionContext) {
        uint64_t oldContextId = m_compositionContextId;
        bool termOk = TerminateCompositionOnHost();
        if (termOk && m_pComposition == nullptr && GetPendingStatus(oldContextId) == PendingTerminationStatus::Succeeded) {
            m_ipcClient.ResetContext(m_sessionId, oldContextId);
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

static bool HasModifierDown() {
    bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool isAlt  = (GetKeyState(VK_MENU) & 0x8000) != 0;
    bool isWin  = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
    return isCtrl || isAlt || isWin;
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
    if (!fForeground) {
        m_candidateWindow.Hide();
        if (m_pCompositionContext) {
            uint64_t oldContextId = m_compositionContextId;
            bool termOk = TerminateCompositionOnHost();
            if (termOk && m_pComposition == nullptr && GetPendingStatus(oldContextId) == PendingTerminationStatus::Succeeded) {
                m_ipcClient.ResetContext(m_sessionId, oldContextId);
            }
        } else if (GetPendingStatus(m_activeContextId) == PendingTerminationStatus::Succeeded) {
            m_ipcClient.ResetContext(m_sessionId, m_activeContextId);
        }
    }
    return S_OK;
}

STDMETHODIMP CTextInputProcessor::OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (!pfEaten) return E_POINTER;
    *pfEaten = FALSE;

    if (HasModifierDown() || IsContextReadOnly(pic)) {
        return S_OK;
    }

    IpcKeyRequest req{};
    req.msg_type = static_cast<uint32_t>(IpcMsgType::TestKeyDown);
    req.session_id = m_sessionId;
    req.context_id = reinterpret_cast<uint64_t>(pic);
    req.vk_code = static_cast<uint32_t>(wParam);

    IpcKeyResponse resp{};
    if (m_ipcClient.SendRequest(req, resp, 50)) {
        *pfEaten = (resp.handled != 0) ? TRUE : FALSE;
    }

    return S_OK;
}

STDMETHODIMP CTextInputProcessor::OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (!pfEaten) return E_POINTER;
    *pfEaten = FALSE;

    if (HasModifierDown() || IsContextReadOnly(pic)) {
        return S_OK;
    }

    uint64_t contextId = reinterpret_cast<uint64_t>(pic);

    // If returning to a context with a pending/failed termination, retry finishing it before handling new input
    if (GetPendingStatus(contextId) != PendingTerminationStatus::Succeeded) {
        bool retryOk = TerminateCompositionOnHost(pic);
        if (retryOk && GetPendingStatus(contextId) == PendingTerminationStatus::Succeeded) {
            s_hitPendingOnKeyDownRecovery.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Cannot cleanly end previous composition on this context: block new composition to prevent overlapping!
            *pfEaten = FALSE;
            m_candidateWindow.Hide();
            return S_OK;
        }
    }

    if (m_activeContextId != contextId) {
        if (m_pCompositionContext && m_pCompositionContext != pic) {
            uint64_t oldContextId = m_compositionContextId;
            bool termOk = TerminateCompositionOnHost();
            if (termOk && m_pComposition == nullptr && GetPendingStatus(oldContextId) == PendingTerminationStatus::Succeeded) {
                m_ipcClient.ResetContext(m_sessionId, oldContextId);
            }
        }
        m_activeContextId = contextId;
    }

    IpcKeyRequest req{};
    req.msg_type = static_cast<uint32_t>(IpcMsgType::KeyDown);
    req.session_id = m_sessionId;
    req.context_id = contextId;
    req.vk_code = static_cast<uint32_t>(wParam);

    IpcKeyResponse resp{};
    if (m_ipcClient.SendRequest(req, resp, 50)) {
        if (!resp.handled) {
            return S_OK;
        }

        std::vector<std::wstring> cands;
        for (uint32_t i = 0; i < resp.candidate_count && i < 5; ++i) {
            cands.push_back(resp.candidates[i]);
        }

        if (resp.has_composition || resp.has_commit) {
            CEditSession* pSession = new CEditSession(
                this, pic, &m_candidateWindow,
                resp.has_composition != 0, resp.composition,
                resp.has_commit != 0, resp.commit_text,
                cands
            );

            HRESULT hrSession = S_OK;
            HRESULT hr = pic->RequestEditSession(m_tfClientId, pSession, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
            EditSessionResult result = pSession->GetResult();
            pSession->Release();

            if (SUCCEEDED(hr) && SUCCEEDED(hrSession) && result == EditSessionResult::Success) {
                *pfEaten = TRUE;
            } else if (result == EditSessionResult::PartiallyModified) {
                // THIS session modified host text; eat key to avoid duplication, do NOT rollback
                *pfEaten = TRUE;
                m_candidateWindow.Hide();
            } else {
                // Unmodified or RecoveryBlocked: current key text was NOT applied to host!
                // Do NOT eat the key (*pfEaten = FALSE), hide window, and roll back server for current key!
                if (result == EditSessionResult::RecoveryBlocked) {
                    s_hitRecoveryBlocked.fetch_add(1, std::memory_order_relaxed);
                }
                *pfEaten = FALSE;
                m_candidateWindow.Hide();
                m_ipcClient.Rollback(m_sessionId, contextId);
            }
        } else {
            *pfEaten = TRUE;
        }
    }

    return S_OK;
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
        SetComposition(nullptr, nullptr, CompositionState::None);
        m_ipcClient.ResetContext(m_sessionId, terminatedContextId);
        if (terminatedContextId == m_activeContextId) {
            m_candidateWindow.Hide();
        }
        return S_OK;
    }

    // 2. Check pending terminations
    for (auto it = m_pendingTerminations.begin(); it != m_pendingTerminations.end(); ++it) {
        if (it->pComp == pComposition) {
            uint64_t terminatedContextId = it->contextId;
            m_ipcClient.ResetContext(m_sessionId, terminatedContextId);
            m_pendingTerminations.erase(it);
            return S_OK;
        }
    }

    // 3. Stale or late notification for unknown / already-finalized composition: safely ignored
    s_hitLateNotificationIgnored.fetch_add(1, std::memory_order_relaxed);
    return S_OK;
}
