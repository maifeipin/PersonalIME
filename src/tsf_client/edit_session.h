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
            // If not already committed, clear the in-progress composition string
            if (!m_isCommittedText) {
                ITfRange* pRange = nullptr;
                hr = m_pComp->GetRange(&pRange);
                if (SUCCEEDED(hr) && pRange) {
                    hr = pRange->SetText(ec, 0, L"", 0);
                    pRange->Release();
                }
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

        ITfContextComposition* pContextComp = nullptr;
        HRESULT hr = m_pContext->QueryInterface(IID_ITfContextComposition, (void**)&pContextComp);
        if (FAILED(hr)) {
            m_result = EditSessionResult::Unmodified;
            m_finalHr = hr;
            return hr;
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
                    pContextComp->Release();
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
                hr = pComp->GetRange(&pRange);
                if (SUCCEEDED(hr) && pRange) {
                    hr = pRange->SetText(ec, 0, L"", 0);
                    pRange->Release();
                }
                if (SUCCEEDED(hr)) {
                    hr = pComp->EndComposition(ec);
                }
                pContextComp->Release();
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
                pContextComp->Release();
                m_result = EditSessionResult::Success;
                m_finalHr = S_OK;
                return S_OK;
            }
        }

        // 1. Commit Phase
        if (m_hasCommit && !m_commitText.empty()) {
            hideOwnedWindow();

            if (pComp) {
                ITfRange* pRange = nullptr;
                hr = pComp->GetRange(&pRange);
                if (FAILED(hr) || !pRange) {
                    pContextComp->Release();
                    m_result = EditSessionResult::Unmodified;
                    m_finalHr = FAILED(hr) ? hr : E_FAIL;
                    return m_finalHr;
                }

                hr = pRange->SetText(ec, 0, m_commitText.c_str(), static_cast<LONG>(m_commitText.length()));
                if (FAILED(hr)) {
                    pRange->Release();
                    pContextComp->Release();
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
                pContextComp->Release();

                if (FAILED(hrEndComp) || FAILED(hrSelection)) {
                    m_result = EditSessionResult::PartiallyModified;
                    m_finalHr = FAILED(hrEndComp) ? hrEndComp : hrSelection;
                    if (SUCCEEDED(hrEndComp)) {
                        m_pTip->SetComposition(nullptr, nullptr, CompositionState::None);
                    } else {
                        // Mark as CommitPendingEnd so next keystroke will NOT overwrite committed text!
                        m_pTip->SetCompositionState(CompositionState::CommitPendingEnd);
                    }
                    return m_finalHr;
                }

                m_pTip->SetComposition(nullptr, nullptr, CompositionState::None);
                m_result = EditSessionResult::Success;
                m_finalHr = S_OK;
                return S_OK;
            } else {
                TF_SELECTION sel{};
                ULONG cFetched = 0;
                hr = m_pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &cFetched);
                if (FAILED(hr) || cFetched == 0) {
                    pContextComp->Release();
                    m_result = EditSessionResult::Unmodified;
                    m_finalHr = FAILED(hr) ? hr : E_FAIL;
                    return m_finalHr;
                }

                hr = sel.range->SetText(ec, 0, m_commitText.c_str(), static_cast<LONG>(m_commitText.length()));
                if (FAILED(hr)) {
                    sel.range->Release();
                    pContextComp->Release();
                    m_result = EditSessionResult::Unmodified;
                    m_finalHr = hr;
                    return hr;
                }

                HRESULT hrCollapse = sel.range->Collapse(ec, TF_ANCHOR_END);
                HRESULT hrSelection = S_OK;
                if (SUCCEEDED(hrCollapse)) {
                    hrSelection = m_pContext->SetSelection(ec, 1, &sel);
                }
                sel.range->Release();
                pContextComp->Release();

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

        // 2. Composition Phase
        if (m_hasComp) {
            if (m_compText.empty()) {
                hideOwnedWindow();
                if (pComp) {
                    ITfRange* pRange = nullptr;
                    hr = pComp->GetRange(&pRange);
                    if (FAILED(hr) || !pRange) {
                        pContextComp->Release();
                        m_result = EditSessionResult::Unmodified;
                        m_finalHr = FAILED(hr) ? hr : E_FAIL;
                        return m_finalHr;
                    }

                    hr = pRange->SetText(ec, 0, L"", 0);
                    pRange->Release();
                    if (FAILED(hr)) {
                        pContextComp->Release();
                        m_result = EditSessionResult::Unmodified;
                        m_finalHr = hr; // Text could not be cleared: unmodified
                        return hr;
                    }

                    // Text was cleared; finish composition
                    HRESULT hrEndComp = pComp->EndComposition(ec);
                    pContextComp->Release();

                    if (FAILED(hrEndComp)) {
                        m_result = EditSessionResult::PartiallyModified;
                        m_finalHr = hrEndComp;
                        return hrEndComp;
                    }

                    m_pTip->SetComposition(nullptr, nullptr, CompositionState::None);
                    m_result = EditSessionResult::Success;
                    m_finalHr = S_OK;
                    return S_OK;
                } else {
                    pContextComp->Release();
                    m_result = EditSessionResult::Success;
                    m_finalHr = S_OK;
                    return S_OK;
                }
            } else {
                // Start composition if not active on this context
                if (!pComp) {
                    TF_SELECTION sel{};
                    ULONG cFetched = 0;
                    hr = m_pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &cFetched);
                    if (FAILED(hr) || cFetched == 0) {
                        pContextComp->Release();
                        m_result = EditSessionResult::Unmodified;
                        m_finalHr = FAILED(hr) ? hr : E_FAIL;
                        return m_finalHr;
                    }

                    ITfComposition* pNewComp = nullptr;
                    hr = pContextComp->StartComposition(ec, sel.range, m_pTip, &pNewComp);
                    sel.range->Release();
                    if (FAILED(hr) || !pNewComp) {
                        pContextComp->Release();
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
                hr = pComp->GetRange(&pRange);
                if (FAILED(hr) || !pRange) {
                    pContextComp->Release();
                    m_result = EditSessionResult::Unmodified;
                    m_finalHr = FAILED(hr) ? hr : E_FAIL;
                    return m_finalHr;
                }

                hr = pRange->SetText(ec, 0, m_compText.c_str(), static_cast<LONG>(m_compText.length()));
                if (FAILED(hr)) {
                    pRange->Release();
                    pContextComp->Release();
                    m_result = EditSessionResult::Unmodified;
                    m_finalHr = hr; // Text not modified: safe to rollback
                    return hr;
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

                // Locate Caret and display candidate window only if this context is still focused and current
                bool canShowUI = (m_generation == 0) ||
                                 (m_pTip && m_pTip->CanShowCandidateWindow(m_contextId, m_generation));
                if (!m_candidates.empty() && m_pCandWnd && canShowUI) {
                    ITfContextView* pView = nullptr;
                    HRESULT hrView = m_pContext->GetActiveView(&pView);
                    if (SUCCEEDED(hrView) && pView) {
                        RECT rc{};
                        BOOL fClipped = FALSE;
                        if (SUCCEEDED(pView->GetTextExt(ec, pRange, &rc, &fClipped))) {
                            if (rc.right >= rc.left && rc.bottom > rc.top) {
                                POINT pt = { rc.left, rc.bottom + 2 };
                                m_pCandWnd->Show(m_snapshot, pt);
                            } else {
                                hideOwnedWindow();
                            }
                        } else {
                            hideOwnedWindow();
                        }
                        pView->Release();
                    } else if (hrView == E_NOTIMPL) {
                        POINT pt = { 100, 100 };
                        m_pCandWnd->Show(m_snapshot, pt);
                    } else {
                        hideOwnedWindow();
                    }
                } else {
                    hideOwnedWindow();
                }

                pRange->Release();
                pContextComp->Release();

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

        pContextComp->Release();
        m_result = EditSessionResult::Success;
        m_finalHr = S_OK;
        return S_OK;
    }

private:
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