#pragma once
#include <windows.h>
#include <msctf.h>
#include <string>
#include <vector>
#include "text_input_processor.h"
#include "candidate_window.h"
#include "globals.h"

// Dedicated EditSession to terminate composition on the host document
class CTerminateCompositionSession : public ITfEditSession {
public:
    CTerminateCompositionSession(CTextInputProcessor* pTip,
                                 ITfComposition* pComp,
                                 uint64_t contextId,
                                 uint64_t generation,
                                 bool isCommittedText)
        : m_pTip(pTip), m_pComp(pComp), m_contextId(contextId),
          m_generation(generation), m_isCommittedText(isCommittedText) {
        DllAddRef();
        if (m_pTip) m_pTip->AddRef();
        if (m_pComp) m_pComp->AddRef();
    }

    CTerminateCompositionSession(ITfComposition* pComp, bool isCommittedText)
        : m_pTip(nullptr), m_pComp(pComp), m_contextId(0),
          m_generation(0), m_isCommittedText(isCommittedText) {
        DllAddRef();
        if (m_pComp) m_pComp->AddRef();
    }

    virtual ~CTerminateCompositionSession() {
        if (m_pComp) m_pComp->Release();
        if (m_pTip) m_pTip->Release();
        DllRelease();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppvObj = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG) Release() override {
        LONG cr = InterlockedDecrement(&m_cRef);
        if (cr == 0) delete this;
        return cr;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        HRESULT hr = S_OK;
        if (m_pComp) {
            ITfRange* pRange = nullptr;
            HRESULT hrRange = m_pComp->GetRange(&pRange);
            if (SUCCEEDED(hrRange) && pRange) {
                ITfContext* pCtx = nullptr;
                if (SUCCEEDED(pRange->GetContext(&pCtx)) && pCtx) {
                    ITfProperty* pProp = nullptr;
                    if (SUCCEEDED(pCtx->GetProperty(GUID_PROP_ATTRIBUTE, &pProp)) && pProp) {
                        pProp->Clear(ec, pRange);
                        pProp->Release();
                    }
                    pCtx->Release();
                }
                // If not already committed, clear the in-progress composition string
                if (!m_isCommittedText) {
                    hr = pRange->SetText(ec, 0, L"", 0);
                }
                pRange->Release();
            } else if (!m_isCommittedText) {
                hr = hrRange;
            }
            if (SUCCEEDED(hr)) {
                // If committed text, do NOT call SetText(L""), only call EndComposition to finalize
                hr = m_pComp->EndComposition(ec);
            }
        }

        if (m_pTip) {
            m_pTip->OnTerminationSessionCompleted(m_contextId, m_pComp, m_generation, hr);
        }
        return hr;
    }

private:
    LONG m_cRef = 1;
    CTextInputProcessor* m_pTip = nullptr;
    ITfComposition* m_pComp = nullptr;
    uint64_t m_contextId = 0;
    uint64_t m_generation = 0;
    bool m_isCommittedText = false;
};

// Main EditSession handling composition text updates and commit
class CEditSession : public ITfEditSession {
public:
    CEditSession(CTextInputProcessor* pTip, ITfContext* pContext, CCandidateWindow* pCandWnd,
                 bool hasComp, const std::wstring& compText,
                 bool hasCommit, const std::wstring& commitText,
                 const std::vector<std::wstring>& candidates,
                 uint64_t generation = 0,
                 const CandidateDisplaySnapshot* pSnapshot = nullptr)
        : m_pTip(pTip), m_pContext(pContext), m_pCandWnd(pCandWnd),
          m_hasComp(hasComp), m_compText(compText),
          m_hasCommit(hasCommit), m_commitText(commitText),
          m_candidates(candidates),
          m_contextId(reinterpret_cast<uint64_t>(pContext)),
          m_generation(generation) {
        if (pSnapshot) {
            m_snapshot = *pSnapshot;
        } else {
            m_snapshot.context_id = m_contextId;
            m_snapshot.pContext = pContext;
            m_snapshot.candidates = candidates;
            if (m_pTip) {
                m_snapshot.session_id = m_pTip->GetSessionIdForTest();
                m_snapshot.context_epoch = m_pTip->EnsureContextEpoch(m_contextId);
                uint64_t curGen = m_pTip->GetContextInputGeneration(m_contextId);
                m_snapshot.input_generation = (curGen != 0) ? curGen : m_pTip->AdvanceContextInputGeneration(m_contextId);
            }
            if (m_hasComp) {
                m_snapshot.raw_input = m_compText;
            }
            if (!candidates.empty()) {
                m_snapshot.preview_text = CCandidateWindow::ExtractCandidateCoreText(candidates[0]);
            }
        }
        DllAddRef();
        if (m_pTip) m_pTip->AddRef();
        if (m_pContext) m_pContext->AddRef();
    }

    virtual ~CEditSession() {
        if (m_pTip) m_pTip->Release();
        if (m_pContext) m_pContext->Release();
        DllRelease();
    }

    EditSessionResult GetResult() const { return m_result; }
    HRESULT GetFinalHResult() const { return m_finalHr; }
    bool HasExecuted() const { return m_executed; }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppvObj = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG) Release() override {
        LONG cr = InterlockedDecrement(&m_cRef);
        if (cr == 0) delete this;
        return cr;
    }

    // ITfEditSession
    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (m_executed) {
            return m_finalHr;
        }
        m_executed = true;

        struct CompletionGuard {
            CEditSession* self;
            ~CompletionGuard() {
                if (self->m_pTip && self->m_generation != 0) {
                    self->m_pTip->OnEditSessionCompleted(
                        self->m_contextId,
                        self->m_generation,
                        self->m_result,
                        self->m_finalHr);
                }
            }
        } guard{this};

        if (!m_pContext || !m_pTip) {
            m_result = EditSessionResult::Unmodified;
            m_finalHr = E_FAIL;
            return E_FAIL;
        }

        auto hideOwnedWindow = [&]() {
            if (m_pCandWnd) {
                if (m_generation != 0 && m_pTip) {
                    m_pTip->HideCandidateWindowIfOwnedBy(m_contextId);
                } else {
                    m_pCandWnd->Hide();
                }
            }
        };

        if (m_pTip->GetCompositionState(m_pContext) == CompositionState::Quarantined) {
            hideOwnedWindow();
            m_result = EditSessionResult::Unmodified;
            m_finalHr = E_FAIL;
            return E_FAIL;
        }

        // P1-1 RECOVERY: If the previous composition was in CommitPendingEnd,
        // we must finalize the old composition first before any new range modification.
        if (m_pTip->GetCompositionState(m_pContext) == CompositionState::CommitPendingEnd) {
            ITfComposition* pOldComp = m_pTip->GetComposition(m_pContext);
            if (pOldComp) {
                HRESULT hrEnd = pOldComp->EndComposition(ec);
                if (SUCCEEDED(hrEnd)) {
                    // Old composition successfully closed! Committed text is preserved.
                    m_pTip->SetComposition(nullptr, nullptr, CompositionState::None);
                } else {
                    // EndComposition failed again: DO NOT overwrite or touch the committed text!
                    // Report RecoveryBlocked so OnKeyDown does NOT eat the key and rolls back server for current key!
                    m_result = EditSessionResult::RecoveryBlocked;
                    m_finalHr = hrEnd;
                    return hrEnd;
                }
            } else {
                m_pTip->SetComposition(nullptr, nullptr, CompositionState::None);
            }
        }

        ITfComposition* pComp = m_pTip->GetComposition(m_pContext);

        // If this is a composition-only session that was unfocused / context-switched / deactivated
        // while queued in TF_S_ASYNC, do not start or leave an active composition or resurrect UI!
        if (!m_hasCommit && m_hasComp && m_generation != 0 &&
            m_pTip->ShouldTerminateAfterEdit(m_contextId, m_generation)) {
            hideOwnedWindow();
            if (pComp) {
                ITfRange* pRange = nullptr;
                HRESULT hr = pComp->GetRange(&pRange);
                if (SUCCEEDED(hr) && pRange) {
                    ClearDisplayAttribute(ec, pRange);
                    hr = pRange->SetText(ec, 0, L"", 0);
                    pRange->Release();
                }
                if (SUCCEEDED(hr)) {
                    hr = pComp->EndComposition(ec);
                }
                if (SUCCEEDED(hr)) {
                    m_pTip->SetComposition(nullptr, nullptr, CompositionState::None);
                    m_result = EditSessionResult::Success;
                    m_finalHr = S_OK;
                    return S_OK;
                } else {
                    m_result = EditSessionResult::Unmodified;
                    m_finalHr = hr;
                    return hr;
                }
            } else {
                m_pTip->ExitCompositionlessMode(m_contextId);
                m_result = EditSessionResult::Success;
                m_finalHr = S_OK;
                return S_OK;
            }
        }

        // 1. Commit Phase
        if (m_hasCommit && !m_commitText.empty()) {
            hideOwnedWindow();

            if (pComp) {
                // Stage 7.1b: When an inline composition exists on this context,
                // commit MUST ONLY replace pComp->GetRange; NEVER fall back to GetSelection!
                ITfRange* pRange = nullptr;
                HRESULT hr = pComp->GetRange(&pRange);
                if (FAILED(hr) || !pRange) {
                    m_result = EditSessionResult::Unmodified;
                    m_finalHr = FAILED(hr) ? hr : E_FAIL;
                    return m_finalHr;
                }

                ClearDisplayAttribute(ec, pRange);
                hr = pRange->SetText(ec, 0, m_commitText.c_str(), static_cast<LONG>(m_commitText.length()));
                if (FAILED(hr)) {
                    pRange->Release();
                    m_result = EditSessionResult::Unmodified;
                    m_finalHr = hr;
                    return hr; // Host text unmodified: safe to rollback server
                }

                // Text modified on host
                HRESULT hrCollapse = pRange->Collapse(ec, TF_ANCHOR_END);
                HRESULT hrSelection = S_OK;
                if (SUCCEEDED(hrCollapse)) {
                    TF_SELECTION sel{};
                    sel.range = pRange;
                    sel.style.ase = TF_AE_NONE;
                    sel.style.fInterimChar = FALSE;
                    hrSelection = m_pContext->SetSelection(ec, 1, &sel);
                }
                pRange->Release();

                HRESULT hrEndComp = pComp->EndComposition(ec);

                HRESULT hrTail = FAILED(hrEndComp) ? hrEndComp : (FAILED(hrCollapse) ? hrCollapse : hrSelection);
                if (FAILED(hrTail)) {
                    m_result = EditSessionResult::PartiallyModified;
                    m_finalHr = hrTail;
                    if (SUCCEEDED(hrEndComp)) {
                        m_pTip->SetComposition(nullptr, nullptr, CompositionState::None);
                    } else {
                        // Mark as CommitPendingEnd so next keystroke will NOT overwrite committed text!
                        m_pTip->SetCompositionState(CompositionState::CommitPendingEnd);
                    }
                    return hrTail;
                }

                m_pTip->SetComposition(nullptr, nullptr, CompositionState::None);
                m_result = EditSessionResult::Success;
                m_finalHr = S_OK;
                return S_OK;
            } else {
                // Compositionless commit (or direct punctuation/English commit without active inline composition)
                if (!m_pTip->IsCompositionlessCommitTargetValid(
                        m_contextId,
                        m_pContext,
                        m_generation,
                        m_snapshot.context_epoch,
                        m_snapshot.input_generation,
                        m_snapshot.mode)) {
                    m_result = EditSessionResult::Unmodified;
                    m_finalHr = E_ABORT;
                    return E_ABORT;
                }

                if (m_pTip->GetPendingStatus(m_contextId) != CTextInputProcessor::PendingTerminationStatus::Succeeded ||
                    m_pTip->GetCompositionState(m_pContext) == CompositionState::CommitPendingEnd ||
                    m_pTip->GetCompositionState(m_pContext) == CompositionState::Quarantined) {
                    m_result = EditSessionResult::RecoveryBlocked;
                    m_finalHr = E_FAIL;
                    return E_FAIL;
                }

                TF_SELECTION sel{};
                ULONG cFetched = 0;
                HRESULT hr = m_pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &cFetched);
                if (SUCCEEDED(hr) && cFetched > 0 && sel.range) {
                    hr = sel.range->SetText(ec, 0, m_commitText.c_str(), static_cast<LONG>(m_commitText.length()));
                    if (SUCCEEDED(hr)) {
                        HRESULT hrCollapse = sel.range->Collapse(ec, TF_ANCHOR_END);
                        HRESULT hrSelection = S_OK;
                        if (SUCCEEDED(hrCollapse)) {
                            hrSelection = m_pContext->SetSelection(ec, 1, &sel);
                        }
                        sel.range->Release();
                        m_pTip->ExitCompositionlessMode(m_contextId);

                        HRESULT hrTail = FAILED(hrCollapse) ? hrCollapse : hrSelection;
                        if (FAILED(hrTail)) {
                            // Stage 7.1b: Compositionless commit wrote text, Collapse/SetSelection failed ->
                            // PartiallyModified, do NOT fabricate composition or set CommitPendingEnd!
                            m_result = EditSessionResult::PartiallyModified;
                            m_finalHr = hrTail;
                            return hrTail;
                        }

                        m_result = EditSessionResult::Success;
                        m_finalHr = S_OK;
                        return S_OK;
                    }
                    sel.range->Release();
                } else {
                    // Fallback within Level 2: ITfInsertAtSelection when GetSelection is unavailable
                    ITfInsertAtSelection* pInsertAtSel = nullptr;
                    if (SUCCEEDED(m_pContext->QueryInterface(IID_ITfInsertAtSelection, (void**)&pInsertAtSel)) && pInsertAtSel) {
                        ITfRange* pInsertedRange = nullptr;
                        HRESULT hrIns = pInsertAtSel->InsertTextAtSelection(
                            ec, 0, m_commitText.c_str(), static_cast<LONG>(m_commitText.length()), &pInsertedRange);
                        pInsertAtSel->Release();
                        if (SUCCEEDED(hrIns)) {
                            HRESULT hrCollapse = S_OK;
                            HRESULT hrSelection = S_OK;
                            if (pInsertedRange) {
                                hrCollapse = pInsertedRange->Collapse(ec, TF_ANCHOR_END);
                                if (SUCCEEDED(hrCollapse)) {
                                    TF_SELECTION insSel{};
                                    insSel.range = pInsertedRange;
                                    insSel.style.ase = TF_AE_NONE;
                                    insSel.style.fInterimChar = FALSE;
                                    hrSelection = m_pContext->SetSelection(ec, 1, &insSel);
                                }
                                pInsertedRange->Release();
                            }
                            m_pTip->ExitCompositionlessMode(m_contextId);
                            HRESULT hrTail = FAILED(hrCollapse) ? hrCollapse : hrSelection;
                            if (FAILED(hrTail)) {
                                m_result = EditSessionResult::PartiallyModified;
                                m_finalHr = hrTail;
                                return hrTail;
                            }
                            m_result = EditSessionResult::Success;
                            m_finalHr = S_OK;
                            return S_OK;
                        }
                        hr = hrIns;
                    }
                }

                m_result = EditSessionResult::Unmodified;
                m_finalHr = FAILED(hr) ? hr : E_FAIL;
                return m_finalHr;
            }
        }

        // 2. Composition Phase
        if (m_hasComp) {
            if (m_compText.empty()) {
                hideOwnedWindow();
                if (pComp) {
                    ITfRange* pRange = nullptr;
                    HRESULT hr = pComp->GetRange(&pRange);
                    if (FAILED(hr) || !pRange) {
                        m_result = EditSessionResult::Unmodified;
                        m_finalHr = FAILED(hr) ? hr : E_FAIL;
                        return m_finalHr;
                    }

                    ClearDisplayAttribute(ec, pRange);
                    hr = pRange->SetText(ec, 0, L"", 0);
                    pRange->Release();
                    if (FAILED(hr)) {
                        m_result = EditSessionResult::Unmodified;
                        m_finalHr = hr; // Text could not be cleared: unmodified
                        return hr;
                    }

                    // Text was cleared; finish composition
                    HRESULT hrEndComp = pComp->EndComposition(ec);

                    if (FAILED(hrEndComp)) {
                        m_result = EditSessionResult::PartiallyModified;
                        m_finalHr = hrEndComp;
                        return hrEndComp;
                    }

                    m_pTip->SetComposition(nullptr, nullptr, CompositionState::None);
                    m_result = EditSessionResult::Success;
                    m_finalHr = S_OK;
                    return S_OK;
                } else if (m_pTip->GetCompositionState(m_pContext) == CompositionState::CompositionlessComposing) {
                    m_pTip->ExitCompositionlessMode(m_contextId);
                    m_result = EditSessionResult::UiOnlyHandled;
                    m_finalHr = S_OK;
                    return S_OK;
                } else {
                    m_result = EditSessionResult::Success;
                    m_finalHr = S_OK;
                    return S_OK;
                }
            } else {
                // Start composition if not active on this context
                if (!pComp) {
                    if (m_pTip->GetCompositionState(m_pContext) == CompositionState::CompositionlessComposing) {
                        return EnterCompositionlessUiOnly(ec);
                    }

                    ITfContextComposition* pContextComp = nullptr;
                    HRESULT hr = m_pContext->QueryInterface(IID_ITfContextComposition, (void**)&pContextComp);
                    if (FAILED(hr) || !pContextComp) {
                        if (CanDegradeToCompositionless(hr)) {
                            return EnterCompositionlessUiOnly(ec);
                        }
                        m_result = EditSessionResult::Unmodified;
                        m_finalHr = FAILED(hr) ? hr : E_NOINTERFACE;
                        return m_finalHr;
                    }

                    TF_SELECTION sel{};
                    ULONG cFetched = 0;
                    hr = m_pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &cFetched);
                    if (FAILED(hr) || cFetched == 0 || !sel.range) {
                        pContextComp->Release();
                        if (CanDegradeToCompositionless(hr)) {
                            return EnterCompositionlessUiOnly(ec);
                        }
                        m_result = EditSessionResult::Unmodified;
                        m_finalHr = FAILED(hr) ? hr : E_FAIL;
                        return m_finalHr;
                    }

                    ITfComposition* pNewComp = nullptr;
                    hr = pContextComp->StartComposition(ec, sel.range, m_pTip, &pNewComp);
                    sel.range->Release();
                    pContextComp->Release();
                    if (FAILED(hr) || !pNewComp) {
                        if (CanDegradeToCompositionless(hr)) {
                            return EnterCompositionlessUiOnly(ec);
                        }
                        m_result = EditSessionResult::Unmodified;
                        m_finalHr = FAILED(hr) ? hr : E_FAIL;
                        return m_finalHr;
                    }

                    // Pair StartComposition's AddRef: SetComposition AddRefs, so release pNewComp
                    m_pTip->SetComposition(pNewComp, m_pContext, CompositionState::Composing);
                    pNewComp->Release();

                    pComp = m_pTip->GetComposition(m_pContext);
                }

                // Update text
                ITfRange* pRange = nullptr;
                HRESULT hr = pComp->GetRange(&pRange);
                if (FAILED(hr) || !pRange) {
                    m_result = EditSessionResult::Unmodified;
                    m_finalHr = FAILED(hr) ? hr : E_FAIL;
                    return m_finalHr;
                }

                hr = pRange->SetText(ec, 0, m_compText.c_str(), static_cast<LONG>(m_compText.length()));
                if (FAILED(hr)) {
                    pRange->Release();
                    m_result = EditSessionResult::Unmodified;
                    m_finalHr = hr; // Text not modified: safe to rollback
                    return hr;
                }

                // Apply standard inline composition underline display attribute (GUID_PROP_ATTRIBUTE)
                ApplyDisplayAttribute(ec, pRange);

                // Text modified on host
                HRESULT hrCollapse = pRange->Collapse(ec, TF_ANCHOR_END);
                HRESULT hrSelection = S_OK;
                if (SUCCEEDED(hrCollapse)) {
                    TF_SELECTION sel{};
                    sel.range = pRange;
                    sel.style.ase = TF_AE_NONE;
                    sel.style.fInterimChar = FALSE;
                    hrSelection = m_pContext->SetSelection(ec, 1, &sel);
                }

                // Locate Caret and display candidate window only if this context is still focused and current
                bool canShowUI = (m_generation == 0) ||
                                 (m_pTip && m_pTip->CanShowCandidateWindow(m_contextId, m_generation));
                if (m_pCandWnd && canShowUI && m_pCandWnd->ShouldShowWindow(m_snapshot)) {
                    ITfContextView* pView = nullptr;
                    HRESULT hrView = m_pContext->GetActiveView(&pView);
                    if (SUCCEEDED(hrView) && pView) {
                        RECT rc{};
                        BOOL fClipped = FALSE;
                        if (SUCCEEDED(pView->GetTextExt(ec, pRange, &rc, &fClipped))) {
                            if (rc.right >= rc.left && rc.bottom > rc.top) {
                                POINT pt = { rc.left, rc.bottom + 2 };
                                m_pTip->CacheContextAnchorPoint(
                                    m_contextId,
                                    m_pTip->EnsureContextEpoch(m_contextId),
                                    reinterpret_cast<uint64_t>(pView),
                                    pt);
                                m_pCandWnd->Show(m_snapshot, pt);
                            } else {
                                hideOwnedWindow();
                            }
                        } else {
                            hideOwnedWindow();
                        }
                        pView->Release();
                    } else if (hrView == E_NOTIMPL) {
                        POINT pt{};
                        if (m_pTip->ResolveAnchorPointForContext(m_pContext, ec, pRange, true, &pt)) {
                            m_pCandWnd->Show(m_snapshot, pt);
                        } else {
                            hideOwnedWindow();
                        }
                    } else {
                        hideOwnedWindow();
                    }
                } else {
                    hideOwnedWindow();
                }

                pRange->Release();

                if (FAILED(hrSelection)) {
                    m_result = EditSessionResult::PartiallyModified;
                    m_finalHr = hrSelection;
                    return hrSelection;
                }

                m_result = EditSessionResult::Success;
                m_finalHr = S_OK;
                return S_OK;
            }
        }

        m_result = EditSessionResult::Success;
        m_finalHr = S_OK;
        return S_OK;
    }

private:
    void ApplyDisplayAttribute(TfEditCookie ec, ITfRange* pRange) {
        if (!m_pContext || !m_pTip || !pRange) return;
        TfGuidAtom atom = m_pTip->GetDisplayAttributeAtom();
        if (atom == TF_INVALID_GUIDATOM) return;
        ITfProperty* pProp = nullptr;
        if (SUCCEEDED(m_pContext->GetProperty(GUID_PROP_ATTRIBUTE, &pProp)) && pProp) {
            VARIANT var;
            VariantInit(&var);
            var.vt = VT_I4;
            var.lVal = static_cast<LONG>(atom);
            pProp->SetValue(ec, pRange, &var);
            pProp->Release();
        }
    }

    void ClearDisplayAttribute(TfEditCookie ec, ITfRange* pRange) {
        if (!m_pContext || !pRange) return;
        ITfProperty* pProp = nullptr;
        if (SUCCEEDED(m_pContext->GetProperty(GUID_PROP_ATTRIBUTE, &pProp)) && pProp) {
            pProp->Clear(ec, pRange);
            pProp->Release();
        }
    }

    bool CanDegradeToCompositionless(HRESULT hrFail) const {
        if (!m_pContext || !m_pTip) return false;
        // Reject lock, disconnect, read-only, memory, or abort errors
        if (hrFail == E_OUTOFMEMORY ||
            hrFail == TF_E_DISCONNECTED ||
            hrFail == TF_E_NOLOCK ||
            hrFail == TF_E_READONLY ||
            hrFail == E_ABORT ||
            hrFail == TF_E_INVALIDPOINT) {
            return false;
        }
        TF_STATUS status{};
        if (SUCCEEDED(m_pContext->GetStatus(&status))) {
            if (status.dwDynamicFlags & TS_SD_READONLY) {
                return false;
            }
        }
        if (m_pTip->GetComposition(m_pContext) != nullptr) {
            return false;
        }
        if (m_pTip->GetPendingStatus(m_contextId) != CTextInputProcessor::PendingTerminationStatus::Succeeded) {
            return false;
        }
        CompositionState st = m_pTip->GetCompositionState(m_pContext);
        if (st == CompositionState::CommitPendingEnd || st == CompositionState::Quarantined) {
            return false;
        }
        if (m_generation != 0 && !m_pTip->CanShowCandidateWindow(m_contextId, m_generation)) {
            return false;
        }
        return true;
    }

    HRESULT EnterCompositionlessUiOnly(TfEditCookie ec) {
        m_pTip->EnterCompositionlessMode(m_pContext);
        m_snapshot.mode = CandidateWindowMode::Compositionless;
        m_snapshot.context_epoch = m_pTip->EnsureContextEpoch(m_contextId);
        uint64_t gen = m_pTip->GetContextInputGeneration(m_contextId);
        m_snapshot.input_generation = (gen != 0) ? gen : m_pTip->AdvanceContextInputGeneration(m_contextId);
        if (m_snapshot.raw_input.empty()) {
            m_snapshot.raw_input = m_compText;
        }

        bool canShowUI = (m_generation == 0) ||
                         (m_pTip && m_pTip->CanShowCandidateWindow(m_contextId, m_generation));
        if (m_pCandWnd && canShowUI && m_pCandWnd->ShouldShowWindow(m_snapshot)) {
            POINT pt{};
            if (m_pTip->ResolveAnchorPointForContext(m_pContext, ec, nullptr, true, &pt)) {
                m_pCandWnd->Show(m_snapshot, pt);
            } else {
                m_pCandWnd->Hide();
            }
        } else if (m_pCandWnd) {
            if (m_generation != 0 && m_pTip) {
                m_pTip->HideCandidateWindowIfOwnedBy(m_contextId);
            } else {
                m_pCandWnd->Hide();
            }
        }

        m_result = EditSessionResult::UiOnlyHandled;
        m_finalHr = S_OK;
        return S_OK;
    }
    LONG m_cRef = 1;
    CTextInputProcessor* m_pTip = nullptr;
    ITfContext* m_pContext = nullptr;
    CCandidateWindow* m_pCandWnd = nullptr;
    bool m_hasComp = false;
    std::wstring m_compText;
    bool m_hasCommit = false;
    std::wstring m_commitText;
    std::vector<std::wstring> m_candidates;
    uint64_t m_contextId = 0;
    uint64_t m_generation = 0;
    CandidateDisplaySnapshot m_snapshot;
    bool m_executed = false;
    EditSessionResult m_result = EditSessionResult::Unmodified;
    HRESULT m_finalHr = S_OK;
};