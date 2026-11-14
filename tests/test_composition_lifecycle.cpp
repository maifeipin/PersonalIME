#include <windows.h>
#include <msctf.h>
#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <fstream>
#include "../src/common/ime_guids.h"
#include "../src/tsf_client/globals.h"
#include "../src/tsf_client/edit_session.h"
#include "../src/tsf_client/text_input_processor.h"
#include "../src/tsf_client/receipt_journal_v2.h"

LONG g_cDllRef = 0;
HINSTANCE g_hInst = nullptr;

#define CHECK_EQUAL(a, b, desc) \
    do { \
        if ((a) != (b)) { \
            std::wcerr << L"[TEST FAILED] " << desc << L" (expected " << (b) << L", got " << (a) << L") at line " << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (false)

#define CHECK_TRUE(cond, desc) \
    do { \
        if (!(cond)) { \
            std::wcerr << L"[TEST FAILED] " << desc << L" at line " << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (false)

// Mock IPC Server for in-process realistic IPC pipeline tests
class MockIpcServer {
public:
    std::wstring m_pipeName;
    HANDLE m_hStopEvent = nullptr;
    HANDLE m_hReadyEvent = nullptr;
    std::vector<HANDLE> m_threads;
    std::vector<HANDLE> m_pipes;
    CRITICAL_SECTION m_csPipes;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stallRequests{false};
    std::atomic<int> m_instancesReady{0};
    std::wstring m_bufferedKeys;
    std::atomic<int> m_rollbackCount{0};
    std::atomic<int> m_keyDownCount{0};

    MockIpcServer(const std::wstring& pipeName) : m_pipeName(pipeName) {
        m_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        InitializeCriticalSection(&m_csPipes);
    }

    ~MockIpcServer() {
        Stop();
        DeleteCriticalSection(&m_csPipes);
        if (m_hStopEvent) CloseHandle(m_hStopEvent);
    }

    bool Start(int numInstances = 5) {
        m_running.store(true);
        m_instancesReady.store(0);
        for (int i = 0; i < numInstances; ++i) {
            HANDLE hThread = CreateThread(nullptr, 0, InstanceThreadProc, this, 0, nullptr);
            if (hThread) {
                m_threads.push_back(hThread);
            }
        }
        while (m_instancesReady.load() < numInstances) {
            Sleep(5);
        }
        return !m_threads.empty();
    }

    void Stop() {
        if (m_running.exchange(false)) {
            SetEvent(m_hStopEvent);
            EnterCriticalSection(&m_csPipes);
            for (HANDLE h : m_pipes) {
                if (h != INVALID_HANDLE_VALUE) {
                    CancelIoEx(h, nullptr);
                }
            }
            LeaveCriticalSection(&m_csPipes);

            for (HANDLE h : m_threads) {
                WaitForSingleObject(h, INFINITE);
                CloseHandle(h);
            }
            m_threads.clear();

            EnterCriticalSection(&m_csPipes);
            for (HANDLE h : m_pipes) {
                if (h != INVALID_HANDLE_VALUE) {
                    CloseHandle(h);
                }
            }
            m_pipes.clear();
            LeaveCriticalSection(&m_csPipes);
        }
    }

    static DWORD WINAPI InstanceThreadProc(LPVOID lpParam) {
        auto* pThis = static_cast<MockIpcServer*>(lpParam);
        HANDLE hPipe = INVALID_HANDLE_VALUE;
        while (pThis->m_running.load()) {
            hPipe = CreateNamedPipeW(
                pThis->m_pipeName.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                16384, 16384, 0, nullptr
            );
            if (hPipe == INVALID_HANDLE_VALUE) {
                Sleep(10);
                continue;
            }

            EnterCriticalSection(&pThis->m_csPipes);
            pThis->m_pipes.push_back(hPipe);
            LeaveCriticalSection(&pThis->m_csPipes);

            pThis->m_instancesReady.fetch_add(1);

            OVERLAPPED ovConnect{};
            ovConnect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            BOOL connected = ConnectNamedPipe(hPipe, &ovConnect);
            if (!connected) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    HANDLE handles[2] = { ovConnect.hEvent, pThis->m_hStopEvent };
                    DWORD wr = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                    if (wr != WAIT_OBJECT_0) {
                        CloseHandle(ovConnect.hEvent);
                        break;
                    }
                } else if (err != ERROR_PIPE_CONNECTED) {
                    CloseHandle(ovConnect.hEvent);
                    CloseHandle(hPipe);
                    hPipe = INVALID_HANDLE_VALUE;
                    continue;
                }
            }
            CloseHandle(ovConnect.hEvent);

            if (!pThis->m_running.load()) {
                break;
            }

            // Connection loop
            while (pThis->m_running.load()) {
                IpcKeyRequest req{};
                DWORD bytesRead = 0;
                OVERLAPPED ovRead{};
                ovRead.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                BOOL okRead = ReadFile(hPipe, &req, sizeof(req), &bytesRead, &ovRead);
                if (!okRead && GetLastError() == ERROR_IO_PENDING) {
                    HANDLE handles[2] = { ovRead.hEvent, pThis->m_hStopEvent };
                    DWORD wr = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                    if (wr != WAIT_OBJECT_0) {
                        CloseHandle(ovRead.hEvent);
                        break;
                    }
                    GetOverlappedResult(hPipe, &ovRead, &bytesRead, FALSE);
                }
                CloseHandle(ovRead.hEvent);

                if (bytesRead != sizeof(req)) {
                    break;
                }

                if (pThis->m_stallRequests.load()) {
                    WaitForSingleObject(pThis->m_hStopEvent, INFINITE);
                    break;
                }

                IpcKeyResponse resp{};
                resp.msg_type = req.msg_type;
                resp.session_id = req.session_id;
                resp.context_id = req.context_id;
                resp.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
                resp.handled = 1;

                if (req.msg_type == static_cast<uint32_t>(IpcMsgType::Ping)) {
                    // pong
                } else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::KeyDown)) {
                    pThis->m_keyDownCount.fetch_add(1);
                    wchar_t ch = static_cast<wchar_t>(req.vk_code);
                    if (ch >= L'A' && ch <= L'Z') ch = towlower(ch);
                    EnterCriticalSection(&pThis->m_csPipes);
                    pThis->m_bufferedKeys.push_back(ch);
                    resp.has_composition = 1;
                    wcsncpy_s(resp.composition, pThis->m_bufferedKeys.c_str(), _TRUNCATE);
                    LeaveCriticalSection(&pThis->m_csPipes);
                } else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::Rollback)) {
                    pThis->m_rollbackCount.fetch_add(1);
                    EnterCriticalSection(&pThis->m_csPipes);
                    if (!pThis->m_bufferedKeys.empty()) {
                        pThis->m_bufferedKeys.pop_back();
                    }
                    LeaveCriticalSection(&pThis->m_csPipes);
                } else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::Reset)) {
                    EnterCriticalSection(&pThis->m_csPipes);
                    pThis->m_bufferedKeys.clear();
                    LeaveCriticalSection(&pThis->m_csPipes);
                }

                DWORD written = 0;
                OVERLAPPED ovWrite{};
                ovWrite.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                BOOL okWrite = WriteFile(hPipe, &resp, sizeof(resp), &written, &ovWrite);
                if (!okWrite && GetLastError() == ERROR_IO_PENDING) {
                    WaitForSingleObject(ovWrite.hEvent, INFINITE);
                }
                CloseHandle(ovWrite.hEvent);
            }

            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            hPipe = INVALID_HANDLE_VALUE;
        }
        if (hPipe != INVALID_HANDLE_VALUE) {
            CloseHandle(hPipe);
        }
        return 0;
    }
};

// Global failure injection switches
static bool g_failSetText = false;
static bool g_failEndComposition = false;
static bool g_failSetSelection = false;
static bool g_failCollapse = false;
static HRESULT g_collapseFailHr = E_ABORT;
static std::wstring g_lastCommittedText = L"";

class MockContext;
static MockContext* g_lastActiveMockContext = nullptr;

// 1. Mock ITfRange
class MockRange : public ITfRange {
public:
    MockRange() : m_cRef(1) {}
    virtual ~MockRange() {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfRange)) {
            *ppvObj = static_cast<ITfRange*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG cr = InterlockedDecrement(&m_cRef);
        if (cr == 0) delete this;
        return cr;
    }

    STDMETHODIMP GetText(TfEditCookie ec, DWORD dwFlags, WCHAR *pchText, ULONG cchMax, ULONG *pcch) override {
        if (pcch) *pcch = static_cast<ULONG>(m_text.length());
        if (pchText && cchMax > 0) {
            ULONG toCopy = (std::min)(cchMax - 1, static_cast<ULONG>(m_text.length()));
            wcsncpy_s(pchText, cchMax, m_text.c_str(), toCopy);
        }
        return S_OK;
    }

    STDMETHODIMP SetText(TfEditCookie ec, DWORD dwFlags, const WCHAR* pchText, LONG cch) override;

    STDMETHODIMP GetFormattedText(TfEditCookie ec, IDataObject **ppDataObject) override { return E_NOTIMPL; }
    STDMETHODIMP GetEmbedded(TfEditCookie ec, REFGUID rguidService, REFIID riid, IUnknown **ppunk) override { return E_NOTIMPL; }
    STDMETHODIMP InsertEmbedded(TfEditCookie ec, DWORD dwFlags, IDataObject *pDataObject) override { return E_NOTIMPL; }
    STDMETHODIMP ShiftStart(TfEditCookie ec, LONG cchReq, LONG *pcch, const TF_HALTCOND *pHalt) override { return E_NOTIMPL; }
    STDMETHODIMP ShiftEnd(TfEditCookie ec, LONG cchReq, LONG *pcch, const TF_HALTCOND *pHalt) override { return E_NOTIMPL; }
    STDMETHODIMP ShiftStartToRange(TfEditCookie ec, ITfRange *pRange, TfAnchor aPos) override { return E_NOTIMPL; }
    STDMETHODIMP ShiftEndToRange(TfEditCookie ec, ITfRange *pRange, TfAnchor aPos) override { return E_NOTIMPL; }
    STDMETHODIMP ShiftStartRegion(TfEditCookie ec, TfShiftDir dir, BOOL *pfNoRegion) override { return E_NOTIMPL; }
    STDMETHODIMP ShiftEndRegion(TfEditCookie ec, TfShiftDir dir, BOOL *pfNoRegion) override { return E_NOTIMPL; }
    STDMETHODIMP IsEmpty(TfEditCookie ec, BOOL *pfEmpty) override { return E_NOTIMPL; }
    STDMETHODIMP Collapse(TfEditCookie ec, TfAnchor aPos) override {
        if (g_failCollapse) return g_collapseFailHr;
        return S_OK;
    }
    STDMETHODIMP IsEqualStart(TfEditCookie ec, ITfRange *pWith, TfAnchor aPos, BOOL *pfEqual) override { return E_NOTIMPL; }
    STDMETHODIMP IsEqualEnd(TfEditCookie ec, ITfRange *pWith, TfAnchor aPos, BOOL *pfEqual) override { return E_NOTIMPL; }
    STDMETHODIMP CompareStart(TfEditCookie ec, ITfRange *pWith, TfAnchor aPos, LONG *plResult) override { return E_NOTIMPL; }
    STDMETHODIMP CompareEnd(TfEditCookie ec, ITfRange *pWith, TfAnchor aPos, LONG *plResult) override { return E_NOTIMPL; }
    STDMETHODIMP AdjustForInsert(TfEditCookie ec, ULONG cchInsert, BOOL *pfInsertOk) override { return E_NOTIMPL; }
    STDMETHODIMP GetGravity(TfGravity *pgStart, TfGravity *pgEnd) override { return E_NOTIMPL; }
    STDMETHODIMP SetGravity(TfEditCookie ec, TfGravity gStart, TfGravity gEnd) override { return E_NOTIMPL; }
    STDMETHODIMP Clone(ITfRange **ppClone) override { return E_NOTIMPL; }
    STDMETHODIMP GetContext(ITfContext **ppContext) override { return E_NOTIMPL; }

    std::wstring m_text;
    LONG m_cRef = 1;
};

// 2. Mock ITfComposition with exact ref count inspection
class MockComposition : public ITfComposition {
public:
    static inline std::atomic<int> s_aliveCount{0};
    bool m_endCompositionCalled = false;

    MockComposition() : m_cRef(1) {
        s_aliveCount.fetch_add(1);
    }

    virtual ~MockComposition() {
        s_aliveCount.fetch_sub(1);
    }

    LONG GetRefCount() const { return m_cRef; }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfComposition)) {
            *ppvObj = static_cast<ITfComposition*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG cr = InterlockedDecrement(&m_cRef);
        if (cr == 0) delete this;
        return cr;
    }

    STDMETHODIMP GetRange(ITfRange** ppRange) override {
        if (!ppRange) return E_POINTER;
        *ppRange = new MockRange();
        return S_OK;
    }

    STDMETHODIMP ShiftStart(TfEditCookie ecWrite, ITfRange *pNewStart) override { return E_NOTIMPL; }
    STDMETHODIMP ShiftEnd(TfEditCookie ecWrite, ITfRange *pNewEnd) override { return E_NOTIMPL; }

    STDMETHODIMP EndComposition(TfEditCookie ec) override {
        m_endCompositionCalled = true;
        if (g_failEndComposition) return E_FAIL;
        return S_OK;
    }

private:
    LONG m_cRef = 1;
};

// 3. Mock ITfEditRecord, ITfContext, ITfContextComposition, ITfInsertAtSelection, and ITfSource
class MockEditRecord : public ITfEditRecord {
public:
    MockEditRecord(BOOL selChanged) : m_selChanged(selChanged) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditRecord)) {
            *ppvObj = static_cast<ITfEditRecord*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG cr = InterlockedDecrement(&m_cRef);
        if (cr == 0) delete this;
        return cr;
    }
    STDMETHODIMP GetSelectionStatus(BOOL* pfChanged) override {
        if (!pfChanged) return E_POINTER;
        *pfChanged = m_selChanged;
        return S_OK;
    }
    STDMETHODIMP GetTextAndPropertyUpdates(DWORD dwFlags, const GUID** prgProperties, ULONG cProperties, IEnumTfRanges** ppEnum) override {
        if (ppEnum) *ppEnum = nullptr;
        return S_OK;
    }
private:
    LONG m_cRef = 1;
    BOOL m_selChanged = FALSE;
};

class MockContext : public ITfContext, public ITfContextComposition, public ITfInsertAtSelection, public ITfSource {
public:
    MockContext() : m_cRef(1) {}
    virtual ~MockContext() {
        if (g_lastActiveMockContext == this) {
            g_lastActiveMockContext = nullptr;
        }
        if (m_pDeferredSession) {
            m_pDeferredSession->Release();
            m_pDeferredSession = nullptr;
        }
        if (m_pTextEditSink) {
            m_pTextEditSink->Release();
            m_pTextEditSink = nullptr;
        }
    }

    std::wstring m_activeCompRange;
    bool m_forceSyncBlocked = false;
    bool m_failAsyncRequest = false;
    bool m_deferAsyncSession = false;
    bool m_failStartComposition = false;
    bool m_failGetSelection = false;
    bool m_failAdviseSink = false;
    bool m_inOwnWriteSession = false;
    bool m_fireOnEndEditAfterDoEditSession = false;
    HRESULT m_startCompositionHr = E_FAIL;
    DWORD m_dynamicStatusFlags = 0;
    int m_editSessionRequestCount = 0;
    int m_asyncRequestCount = 0;
    ITfEditSession* m_pDeferredSession = nullptr;
    ITfTextEditSink* m_pTextEditSink = nullptr;
    DWORD m_textEditCookie = 0;

    void SimulateExternalSelectionMove() {
        if (m_pTextEditSink) {
            MockEditRecord rec(TRUE);
            m_pTextEditSink->OnEndEdit(this, 1002, &rec);
        }
    }

    void ExecuteDeferredSession(TfEditCookie ec = 1001) {
        if (m_pDeferredSession) {
            ITfEditSession* pes = m_pDeferredSession;
            m_pDeferredSession = nullptr;
            m_inOwnWriteSession = true;
            pes->DoEditSession(ec);
            if (m_fireOnEndEditAfterDoEditSession && m_pTextEditSink) {
                MockEditRecord rec(TRUE);
                m_pTextEditSink->OnEndEdit(this, ec, &rec);
            }
            m_inOwnWriteSession = false;
            pes->Release();
        }
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfContext)) {
            *ppvObj = static_cast<ITfContext*>(this);
            AddRef();
            return S_OK;
        } else if (IsEqualIID(riid, IID_ITfContextComposition)) {
            *ppvObj = static_cast<ITfContextComposition*>(this);
            AddRef();
            return S_OK;
        } else if (IsEqualIID(riid, IID_ITfInsertAtSelection)) {
            *ppvObj = static_cast<ITfInsertAtSelection*>(this);
            AddRef();
            return S_OK;
        } else if (IsEqualIID(riid, IID_ITfSource)) {
            *ppvObj = static_cast<ITfSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG cr = InterlockedDecrement(&m_cRef);
        if (cr == 0) delete this;
        return cr;
    }

    // ITfContext methods
    STDMETHODIMP RequestEditSession(TfClientId tid, ITfEditSession* pes, DWORD dwFlags, HRESULT* phrSession) override {
        g_lastActiveMockContext = this;
        ++m_editSessionRequestCount;
        if (!pes || !phrSession) return E_INVALIDARG;
        if ((dwFlags & TF_ES_SYNC) && m_forceSyncBlocked) {
            *phrSession = TF_E_SYNCHRONOUS;
            return S_OK; // The session result is separate from the API return value.
        }
        if (dwFlags & TF_ES_ASYNC) {
            ++m_asyncRequestCount;
            if (m_failAsyncRequest) {
                *phrSession = E_FAIL;
                return E_FAIL;
            }
            if (m_deferAsyncSession) {
                if (m_pDeferredSession) m_pDeferredSession->Release();
                m_pDeferredSession = pes;
                pes->AddRef();
                *phrSession = TF_S_ASYNC;
                return S_OK;
            }
        }
        m_inOwnWriteSession = true;
        *phrSession = pes->DoEditSession(1001); // Invoke directly with mock cookie
        if (m_fireOnEndEditAfterDoEditSession && m_pTextEditSink) {
            MockEditRecord rec(TRUE);
            m_pTextEditSink->OnEndEdit(this, 1001, &rec);
        }
        m_inOwnWriteSession = false;
        return S_OK;
    }

    STDMETHODIMP InWriteSession(TfClientId tid, BOOL *pfWriteSession) override {
        if (pfWriteSession) *pfWriteSession = m_inOwnWriteSession ? TRUE : FALSE;
        return S_OK;
    }

    STDMETHODIMP GetSelection(TfEditCookie ec, ULONG ulIndex, ULONG ulCount, TF_SELECTION* pSelection, ULONG* pcFetched) override {
        if (!pSelection || !pcFetched) return E_POINTER;
        if (m_failGetSelection) {
            *pcFetched = 0;
            return E_FAIL;
        }
        pSelection[0].range = new MockRange();
        pSelection[0].style.ase = TF_AE_NONE;
        pSelection[0].style.fInterimChar = FALSE;
        *pcFetched = 1;
        return S_OK;
    }

    STDMETHODIMP SetSelection(TfEditCookie ec, ULONG ulCount, const TF_SELECTION* pSelection) override {
        if (g_failSetSelection) return E_FAIL;
        return S_OK;
    }

    STDMETHODIMP GetStart(TfEditCookie ec, ITfRange **ppStart) override { return E_NOTIMPL; }
    STDMETHODIMP GetEnd(TfEditCookie ec, ITfRange **ppEnd) override { return E_NOTIMPL; }
    STDMETHODIMP GetActiveView(ITfContextView** ppView) override { return E_NOTIMPL; }
    STDMETHODIMP EnumViews(IEnumTfContextViews **ppEnum) override { return E_NOTIMPL; }
    STDMETHODIMP GetStatus(TF_STATUS* pdcs) override {
        if (!pdcs) return E_POINTER;
        pdcs->dwDynamicFlags = m_dynamicStatusFlags;
        pdcs->dwStaticFlags = 0;
        return S_OK;
    }
    STDMETHODIMP GetProperty(REFGUID guidProp, ITfProperty** ppProp) override { return E_NOTIMPL; }
    STDMETHODIMP GetAppProperty(REFGUID guidProp, ITfReadOnlyProperty** ppProp) override { return E_NOTIMPL; }
    STDMETHODIMP TrackProperties(const GUID **prgProp, ULONG cProp, const GUID **prgAppProp, ULONG cAppProp, ITfReadOnlyProperty** ppProperty) override { return E_NOTIMPL; }
    STDMETHODIMP EnumProperties(IEnumTfProperties **ppEnum) override { return E_NOTIMPL; }
    STDMETHODIMP GetDocumentMgr(ITfDocumentMgr **ppDm) override { return E_NOTIMPL; }
    STDMETHODIMP CreateRangeBackup(TfEditCookie ec, ITfRange *pRange, ITfRangeBackup **ppBackup) override { return E_NOTIMPL; }

    // ITfContextComposition methods
    STDMETHODIMP StartComposition(TfEditCookie ecWrite, ITfRange* pCompositionRange, ITfCompositionSink* pSink, ITfComposition** ppComposition) override {
        if (!ppComposition) return E_POINTER;
        if (m_failStartComposition) {
            *ppComposition = nullptr;
            return m_startCompositionHr;
        }
        auto* comp = new MockComposition();
        m_lastCreatedComposition = comp;
        *ppComposition = comp; // COM contract: returns AddRef'd pointer (ref count = 1)
        return S_OK;
    }

    STDMETHODIMP EnumCompositions(IEnumITfCompositionView** ppEnum) override { return E_NOTIMPL; }
    STDMETHODIMP FindComposition(TfEditCookie ecRead, ITfRange *pTestRange, IEnumITfCompositionView **ppEnum) override { return E_NOTIMPL; }
    STDMETHODIMP TakeOwnership(TfEditCookie ecWrite, ITfCompositionView *pComposition, ITfCompositionSink *pSink, ITfComposition** ppComposition) override { return E_NOTIMPL; }

    // ITfInsertAtSelection methods
    STDMETHODIMP InsertTextAtSelection(TfEditCookie ec, DWORD dwFlags, const WCHAR* pchText, LONG cch, ITfRange** ppRange) override {
        if (g_failSetText) return E_FAIL;
        auto* rng = new MockRange();
        rng->m_text = (pchText && cch > 0) ? std::wstring(pchText, cch) : L"";
        g_lastCommittedText = rng->m_text;
        m_activeCompRange = rng->m_text;
        if (ppRange) {
            *ppRange = rng;
        } else {
            rng->Release();
        }
        return S_OK;
    }

    STDMETHODIMP InsertEmbeddedAtSelection(TfEditCookie ec, DWORD dwFlags, IDataObject* pDataObject, ITfRange** ppRange) override {
        return E_NOTIMPL;
    }

    // ITfSource methods
    STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD* pdwCookie) override {
        if (!punk || !pdwCookie) return E_POINTER;
        if (m_failAdviseSink) return E_FAIL;
        if (IsEqualIID(riid, IID_ITfTextEditSink)) {
            ITfTextEditSink* pSink = nullptr;
            if (SUCCEEDED(punk->QueryInterface(IID_ITfTextEditSink, reinterpret_cast<void**>(&pSink))) && pSink) {
                if (m_pTextEditSink) m_pTextEditSink->Release();
                m_pTextEditSink = pSink;
                m_textEditCookie = 42;
                *pdwCookie = m_textEditCookie;
                return S_OK;
            }
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP UnadviseSink(DWORD dwCookie) override {
        if (dwCookie == m_textEditCookie && m_pTextEditSink) {
            m_pTextEditSink->Release();
            m_pTextEditSink = nullptr;
            m_textEditCookie = 0;
            return S_OK;
        }
        return E_INVALIDARG;
    }

    MockComposition* m_lastCreatedComposition = nullptr;
    LONG m_cRef = 1;
};

inline HRESULT MockRange::SetText(TfEditCookie ec, DWORD dwFlags, const WCHAR* pchText, LONG cch) {
    if (g_failSetText) return E_FAIL;
    m_text = (pchText && cch > 0) ? std::wstring(pchText, cch) : L"";
    g_lastCommittedText = m_text;
    if (g_lastActiveMockContext) {
        g_lastActiveMockContext->m_activeCompRange = m_text;
    }
    return S_OK;
}

// =========================================================================
// Tests
// =========================================================================

// Test 1: StartComposition Reference Count Pairing and Zero-Leak Cleanup (P2)
int TestStartCompositionRefcountPairing() {
    std::wcout << L"[TEST] Verifying StartComposition RefCount Pairing and Cleanup..." << std::endl;
    CHECK_EQUAL(MockComposition::s_aliveCount.load(), 0, L"Initial composition count must be 0");

    CTextInputProcessor tip;
    MockContext ctx;

    // 1. First keystroke 'a': StartComposition called
    {
        CEditSession session(&tip, &ctx, nullptr, true, L"a", false, L"", {});
        TfEditCookie ec = 1;
        HRESULT hr = session.DoEditSession(ec);
        CHECK_EQUAL(hr, S_OK, L"DoEditSession for StartComposition must succeed");
        CHECK_EQUAL(session.GetResult() == EditSessionResult::Success, true, L"Result must be Success");
    }

    MockComposition* pComp = ctx.m_lastCreatedComposition;
    CHECK_TRUE(pComp != nullptr, L"Composition must have been created");
    CHECK_EQUAL(pComp->GetRefCount(), 1, L"Composition ref count must be exactly 1 (held by TIP)");
    CHECK_EQUAL(MockComposition::s_aliveCount.load(), 1, L"Alive composition count must be 1");

    // 2. Commit keystroke: EndComposition + SetComposition(nullptr, nullptr)
    {
        CEditSession session(&tip, &ctx, nullptr, false, L"", true, L"测试", {});
        TfEditCookie ec = 1;
        HRESULT hr = session.DoEditSession(ec);
        CHECK_EQUAL(hr, S_OK, L"DoEditSession for commit must succeed");
        CHECK_EQUAL(session.GetResult() == EditSessionResult::Success, true, L"Commit result must be Success");
    }

    CHECK_EQUAL(MockComposition::s_aliveCount.load(), 0, L"Composition must be destroyed with 0 leaks after commit");
    CHECK_TRUE(tip.GetComposition(&ctx) == nullptr, L"TIP composition pointer must be nullptr");

    std::wcout << L"[PASS] StartComposition ref count pairing & cleanup verified with 0 leaks!" << std::endl;
    return 0;
}

// Test 2: Self-assignment Safety in SetComposition (P2)
int TestSetCompositionSelfAssignment() {
    std::wcout << L"[TEST] Verifying SetComposition Self-Assignment Safety..." << std::endl;
    CTextInputProcessor tip;
    MockContext ctx;
    MockComposition comp;

    CHECK_EQUAL(comp.GetRefCount(), 1, L"Initial ref count");

    // Assign once
    tip.SetComposition(&comp, &ctx);
    CHECK_EQUAL(comp.GetRefCount(), 2, L"Ref count after first SetComposition");

    // Self-assignment: assign exact same pointers again
    tip.SetComposition(&comp, &ctx);
    CHECK_EQUAL(comp.GetRefCount(), 2, L"Ref count after redundant SetComposition must remain 2");

    // Clear
    tip.SetComposition(nullptr, nullptr);
    CHECK_EQUAL(comp.GetRefCount(), 1, L"Ref count after clear");

    std::wcout << L"[PASS] SetComposition self-assignment safety verified!" << std::endl;
    return 0;
}

// Test 3: Cancel (Esc) SetText Failure Injection -> Unmodified (P1)
int TestCancelSetTextFailure() {
    std::wcout << L"[TEST] Testing Cancel SetText Failure Injection..." << std::endl;
    CTextInputProcessor tip;
    MockContext ctx;

    // Start composition "a"
    {
        CEditSession session(&tip, &ctx, nullptr, true, L"a", false, L"", {});
        session.DoEditSession(1);
    }
    CHECK_TRUE(tip.GetComposition(&ctx) != nullptr, L"Composition must be active");

    // Inject SetText failure
    g_failSetText = true;
    {
        CEditSession session(&tip, &ctx, nullptr, true, L"", false, L"", {});
        HRESULT hr = session.DoEditSession(1);
        CHECK_TRUE(FAILED(hr), L"DoEditSession must fail when SetText fails");
        CHECK_EQUAL(session.GetResult() == EditSessionResult::Unmodified, true, L"Result must be Unmodified");
    }
    g_failSetText = false;

    CHECK_TRUE(tip.GetComposition(&ctx) != nullptr, L"Composition tracking must be retained after unmodified failure");

    tip.SetComposition(nullptr, nullptr);
    std::wcout << L"[PASS] Cancel SetText failure safely recorded as Unmodified with composition preserved!" << std::endl;
    return 0;
}

// Test 4: Commit SetText Succeeded + EndComposition Failed -> PartiallyModified (P1)
int TestCommitEndCompositionFailure() {
    std::wcout << L"[TEST] Testing Commit SetText Success + EndComposition Failure..." << std::endl;
    CTextInputProcessor tip;
    MockContext ctx;

    // Start composition "a"
    {
        CEditSession session(&tip, &ctx, nullptr, true, L"a", false, L"", {});
        session.DoEditSession(1);
    }

    // Inject EndComposition failure, but let SetText succeed
    g_failEndComposition = true;
    g_lastCommittedText.clear();

    CEditSession session(&tip, &ctx, nullptr, false, L"", true, L"测试", {});
    HRESULT hr = session.DoEditSession(1);
    g_failEndComposition = false;

    CHECK_TRUE(FAILED(hr), L"DoEditSession must report failure when EndComposition fails");
    CHECK_EQUAL(session.GetResult() == EditSessionResult::PartiallyModified, true, L"Result must be PartiallyModified");
    CHECK_EQUAL(g_lastCommittedText, L"测试", L"Host document text was already modified to commit text");
    CHECK_EQUAL(tip.GetCompositionState(&ctx) == CompositionState::CommitPendingEnd, true, L"State must be CommitPendingEnd");

    tip.SetComposition(nullptr, nullptr);
    std::wcout << L"[PASS] Commit EndComposition failure correctly categorized as PartiallyModified & CommitPendingEnd!" << std::endl;
    return 0;
}

// Test 5: SetSelection Failure Injection -> PartiallyModified (P1)
int TestSetSelectionFailure() {
    std::wcout << L"[TEST] Testing SetSelection Failure Injection..." << std::endl;
    CTextInputProcessor tip;
    MockContext ctx;

    // Start composition
    {
        CEditSession session(&tip, &ctx, nullptr, true, L"a", false, L"", {});
        session.DoEditSession(1);
    }

    // Inject SetSelection failure
    g_failSetSelection = true;
    CEditSession session(&tip, &ctx, nullptr, true, L"ab", false, L"", {});
    HRESULT hr = session.DoEditSession(1);
    g_failSetSelection = false;

    CHECK_TRUE(FAILED(hr), L"DoEditSession must report failure when SetSelection fails");
    CHECK_EQUAL(session.GetResult() == EditSessionResult::PartiallyModified, true, L"Result must be PartiallyModified");

    tip.SetComposition(nullptr, nullptr);
    std::wcout << L"[PASS] SetSelection failure correctly categorized as PartiallyModified!" << std::endl;
    return 0;
}

// Test 6: Host Composition Termination upon Context Switch (P1)
int TestContextSwitchCompositionTermination() {
    std::wcout << L"[TEST] Testing Host Composition Termination on Context Switch..." << std::endl;
    CTextInputProcessor tip;
    MockContext ctxA;

    // Start composition on ctxA
    {
        CEditSession session(&tip, &ctxA, nullptr, true, L"a", false, L"", {});
        session.DoEditSession(1);
    }

    MockComposition* pCompA = ctxA.m_lastCreatedComposition;
    CHECK_TRUE(pCompA != nullptr, L"Comp A created");
    CHECK_EQUAL(pCompA->m_endCompositionCalled, false, L"Comp A not yet ended");

    // In uncommitted state, termination clears text and ends composition
    {
        CTerminateCompositionSession termSession(pCompA, false /* isCommittedText */);
        HRESULT hr = termSession.DoEditSession(1);
        CHECK_EQUAL(hr, S_OK, L"CTerminateCompositionSession must succeed");
        CHECK_EQUAL(pCompA->m_endCompositionCalled, true, L"EndComposition must have been executed on ctxA");
    }

    tip.SetComposition(nullptr, nullptr);
    CHECK_TRUE(tip.GetComposition(&ctxA) == nullptr, L"ctxA composition cleared from TIP");

    std::wcout << L"[PASS] Context switch correctly terminates host composition!" << std::endl;
    return 0;
}

// Persistent Range Composition for Round 5 & 6 Scenarios
class LifecyclePersistentComposition : public MockComposition {
public:
    MockRange* range = new MockRange();
    ~LifecyclePersistentComposition() override { range->Release(); }
    STDMETHODIMP GetRange(ITfRange** output) override {
        if (!output) return E_POINTER;
        *output = range;
        range->AddRef();
        return S_OK;
    }
};

// Test 7: Persistent Range - Commit with EndComposition failure followed by next key 'b' (P1-1 Review Round 5)
int TestPersistentRangeCommitFailureRecovery() {
    std::wcout << L"[TEST] Testing Persistent Range Commit Failure & Next Key Recovery (P1-1)..." << std::endl;
    MockContext context;
    CTextInputProcessor tip;
    auto* composition = new LifecyclePersistentComposition();
    composition->range->m_text = L"a";
    tip.SetComposition(composition, &context);

    // 1. Commit "测试" with EndComposition failure
    g_failEndComposition = true;
    CEditSession commit(&tip, &context, nullptr, false, L"", true, L"测试", {});
    HRESULT commitHr = commit.DoEditSession(1);
    g_failEndComposition = false;

    CHECK_TRUE(FAILED(commitHr), L"Commit must report failure when EndComposition fails");
    CHECK_EQUAL(commit.GetResult() == EditSessionResult::PartiallyModified, true, L"Result must be PartiallyModified");
    CHECK_EQUAL(composition->range->m_text, L"测试", L"Committed text '测试' must be present in document");
    CHECK_EQUAL(tip.GetCompositionState(&context) == CompositionState::CommitPendingEnd, true, L"State must be CommitPendingEnd");

    // 2. Next key 'b' arrives!
    // Transient TSF lock is now gone (g_failEndComposition is false).
    CEditSession nextKey(&tip, &context, nullptr, true, L"b", false, L"", {});
    HRESULT nextHr = nextKey.DoEditSession(1);

    // Assert: Old composition was safely ended, text '测试' was NOT overwritten!
    CHECK_EQUAL(composition->range->m_text, L"测试", L"CRITICAL: Committed text '测试' must NEVER be overwritten!");
    CHECK_EQUAL(composition->m_endCompositionCalled, true, L"Old composition EndComposition must have been finalized");
    CHECK_TRUE(tip.GetComposition(&context) != composition, L"Old composition must no longer be bound to context");

    tip.SetComposition(nullptr, nullptr);
    composition->Release();
    std::wcout << L"[PASS] Next key safely finalized old composition without overwriting committed text!" << std::endl;
    return 0;
}

// Test 8: Persistent Range - Next key arrives while EndComposition STILL fails -> RecoveryBlocked (P1-1 Review Round 6)
int TestPersistentRangeCommitFailureStillFailing() {
    std::wcout << L"[TEST] Testing Persistent Range Next Key When EndComposition Still Fails (RecoveryBlocked)..." << std::endl;
    MockContext context;
    CTextInputProcessor tip;
    auto* composition = new LifecyclePersistentComposition();
    composition->range->m_text = L"a";
    tip.SetComposition(composition, &context);

    // 1. Commit "测试" with EndComposition failure
    g_failEndComposition = true;
    CEditSession commit(&tip, &context, nullptr, false, L"", true, L"测试", {});
    commit.DoEditSession(1);

    CHECK_EQUAL(composition->range->m_text, L"测试", L"Text '测试' in range");

    // 2. Next key 'b' arrives, BUT EndComposition STILL fails!
    CEditSession nextKey(&tip, &context, nullptr, true, L"b", false, L"", {});
    HRESULT nextHr = nextKey.DoEditSession(1);
    g_failEndComposition = false;

    CHECK_TRUE(FAILED(nextHr), L"nextKey must fail when old composition cannot be ended");
    CHECK_EQUAL(composition->range->m_text, L"测试", L"CRITICAL: Text '测试' must NOT be overwritten when EndComposition fails!");
    // Must be RecoveryBlocked, NOT PartiallyModified (so key is NOT eaten and server is rolled back!)
    CHECK_EQUAL(nextKey.GetResult() == EditSessionResult::RecoveryBlocked, true, L"Result must be RecoveryBlocked!");

    tip.SetComposition(nullptr, nullptr);
    composition->Release();
    std::wcout << L"[PASS] Persistent text preserved and RecoveryBlocked reported when EndComposition fails!" << std::endl;
    return 0;
}

// Test 9: Context Switch with CommitPendingEnd does NOT erase committed text (P1-1 & P1-2)
int TestContextSwitchCommitPendingEndPreservesText() {
    std::wcout << L"[TEST] Testing Context Switch with CommitPendingEnd preserves text..." << std::endl;
    MockContext context;
    CTextInputProcessor tip;
    auto* composition = new LifecyclePersistentComposition();
    composition->range->m_text = L"a";
    tip.SetComposition(composition, &context);

    // Commit fails EndComposition
    g_failEndComposition = true;
    CEditSession commit(&tip, &context, nullptr, false, L"", true, L"测试", {});
    commit.DoEditSession(1);
    g_failEndComposition = false;

    CHECK_EQUAL(composition->range->m_text, L"测试", L"Text '测试' in range");

    // Terminate composition on host (e.g. on context switch)
    CTerminateCompositionSession termSession(&tip, composition, reinterpret_cast<uint64_t>(&context), 1, true /* isCommittedText */);
    HRESULT hr = termSession.DoEditSession(1);
    CHECK_EQUAL(hr, S_OK, L"CTerminateCompositionSession must succeed");
    CHECK_EQUAL(composition->range->m_text, L"测试", L"CRITICAL: Committed text must NOT be cleared by TerminateComposition!");
    CHECK_EQUAL(composition->m_endCompositionCalled, true, L"EndComposition was executed");

    tip.SetComposition(nullptr, nullptr);
    composition->Release();
    std::wcout << L"[PASS] TerminateCompositionOnHost with committed text safely preserves text!" << std::endl;
    return 0;
}

// Test 10: Full Review Round 6 sequence: Commit -> EndComp fails -> 'b' (RecoveryBlocked) -> Space (RecoveryBlocked) -> Recovery -> 'c'
int TestRound6PersistentRecoverySequence() {
    std::wcout << L"[TEST] Testing Round 6 Sequence: Commit -> Fail -> 'b' -> Space -> Recovery -> 'c'..." << std::endl;
    CTextInputProcessor::ResetBranchCounters();

    const std::wstring testPipeName = L"\\\\.\\pipe\\PersonalIME_MockPipe_T10";
    MockIpcServer server(testPipeName);
    CHECK_TRUE(server.Start(1), L"Mock server must start");
    IpcClient::SetPipeNameForTest(testPipeName.c_str());

    MockContext context;
    CTextInputProcessor tip;
    tip.SetClientIdForTest(1);

    auto* composition = new LifecyclePersistentComposition();
    composition->range->m_text = L"a";
    tip.SetComposition(composition, &context);

    // 1. Commit "测试" with EndComposition failure
    g_failEndComposition = true;
    CEditSession commit(&tip, &context, nullptr, false, L"", true, L"测试", {});
    HRESULT hr1 = commit.DoEditSession(1);
    CHECK_TRUE(FAILED(hr1), L"Commit must fail");
    CHECK_EQUAL(commit.GetResult() == EditSessionResult::PartiallyModified, true, L"Commit result must be PartiallyModified");
    CHECK_EQUAL(composition->range->m_text, L"测试", L"Text must be '测试'");

    // 2. User presses 'b' via OnKeyDown while EndComposition still fails!
    BOOL fEatenB = TRUE;
    HRESULT hrKeyDownB = tip.OnKeyDown(&context, 'B', 0, &fEatenB);
    CHECK_EQUAL(hrKeyDownB, S_OK, L"OnKeyDown must return S_OK");
    CHECK_EQUAL(fEatenB, FALSE, L"Key 'b' must NOT be eaten (*pfEaten = FALSE) when recovery blocked!");
    CHECK_EQUAL(composition->range->m_text, L"测试", L"'测试' must not be touched by 'b'");
    CHECK_EQUAL(server.m_rollbackCount.load(), 1, L"Server must have received Rollback for 'b'");
    CHECK_EQUAL(server.m_bufferedKeys, L"", L"Server buffer must be rolled back (empty)");
    CHECK_TRUE(CTextInputProcessor::s_hitRecoveryBlocked.load() > 0, L"RecoveryBlocked branch must be hit");

    // 3. User presses Space via OnKeyDown while EndComposition still fails!
    BOOL fEatenSpace = TRUE;
    HRESULT hrKeyDownSpace = tip.OnKeyDown(&context, VK_SPACE, 0, &fEatenSpace);
    CHECK_EQUAL(hrKeyDownSpace, S_OK, L"OnKeyDown must return S_OK");
    CHECK_EQUAL(fEatenSpace, FALSE, L"Space must NOT be eaten (*pfEaten = FALSE) when recovery blocked!");
    CHECK_EQUAL(composition->range->m_text, L"测试", L"'测试' must not be touched by space");
    CHECK_TRUE(CTextInputProcessor::s_hitRecoveryBlocked.load() >= 2, L"RecoveryBlocked branch hit on Space");

    // 4. Host lock clears! User presses 'c' via OnKeyDown
    g_failEndComposition = false;
    BOOL fEatenC = FALSE;
    HRESULT hrKeyDownC = tip.OnKeyDown(&context, 'C', 0, &fEatenC);
    CHECK_EQUAL(hrKeyDownC, S_OK, L"OnKeyDown must succeed once host lock clears");
    CHECK_EQUAL(fEatenC, TRUE, L"Key 'c' must be eaten (*pfEaten = TRUE)");
    CHECK_EQUAL(server.m_bufferedKeys, L"c", L"Server buffer must contain 'c'");
    CHECK_EQUAL(composition->range->m_text, L"测试", L"Original committed '测试' permanently preserved");
    CHECK_EQUAL(composition->m_endCompositionCalled, true, L"Old composition cleanly ended");

    tip.SetComposition(nullptr, nullptr);
    composition->Release();
    server.Stop();
    IpcClient::SetPipeNameForTest(nullptr);

    CHECK_TRUE(CTextInputProcessor::s_hitRecoveryBlocked.load() > 0, L"Target branch s_hitRecoveryBlocked must be > 0");
    std::wcout << L"[PASS] Round 6 recovery sequence passed: zero lost commits, pfEaten=FALSE verified, rollback verified!" << std::endl;
    return 0;
}

// Test 11: Multi-Context Asynchronous Termination, Pending Tracking & 5 Failure Branches (P1-2)
int TestMultiContextPendingTerminationTracking() {
    std::wcout << L"[TEST] Testing Multi-Context Termination Branches (P1-2)..." << std::endl;
    CTextInputProcessor::ResetBranchCounters();

    CTextInputProcessor tip;
    tip.SetClientIdForTest(1);

    // Keep all mock contexts in outer scope so each has a guaranteed unique context address
    MockContext ctxA;
    MockContext ctxB;
    MockContext ctxC;
    MockContext ctxD;
    MockContext ctxE;
    MockContext ctxF;

    // Branch 1: A 结束同步 E_FAIL 后进入 B (Sync E_FAIL, Context A retained in pending, Context B not disturbed)
    {
        auto* compA = new LifecyclePersistentComposition();
        compA->range->m_text = L"a";
        tip.SetComposition(compA, &ctxA);

        g_failEndComposition = true;
        bool okTermA = tip.TerminateCompositionOnHost(&ctxA);
        g_failEndComposition = false;

        CHECK_EQUAL(okTermA, false, L"Sync termination with failing EndComposition must return false");
        CHECK_TRUE(CTextInputProcessor::s_hitTermSyncFail.load() > 0, L"Branch s_hitTermSyncFail must be hit");
        CHECK_EQUAL(tip.GetPendingTerminationCount(), 1, L"Context A must be retained in pending list");
        CHECK_TRUE(tip.GetPendingStatus(reinterpret_cast<uint64_t>(&ctxA)) == CTextInputProcessor::PendingTerminationStatus::FailedSync, L"Status must be FailedSync");

        // Context B starts a new composition - Context A is NOT clobbered!
        auto* compB = new LifecyclePersistentComposition();
        compB->range->m_text = L"b";
        tip.SetComposition(compB, &ctxB);
        CHECK_TRUE(tip.GetComposition(&ctxB) == compB, L"Context B active in m_pComposition");
        CHECK_EQUAL(tip.GetPendingTerminationCount(), 1, L"Context A remains tracked without collision");

        tip.SetComposition(nullptr, nullptr);
        compA->Release();
        compB->Release();
    }

    // Branch 2: 异步请求立即失败 (Sync blocked with TF_E_SYNCHRONOUS, Async RequestEditSession returns E_FAIL)
    {
        auto* compC = new LifecyclePersistentComposition();
        tip.SetComposition(compC, &ctxC);

        ctxC.m_forceSyncBlocked = true;
        ctxC.m_failAsyncRequest = true;

        bool okTermC = tip.TerminateCompositionOnHost(&ctxC);
        CHECK_EQUAL(okTermC, false, L"TerminateCompositionOnHost returns false on async request fail");
        CHECK_TRUE(CTextInputProcessor::s_hitTermAsyncQueued.load() > 0, L"Branch s_hitTermAsyncQueued must be hit");
        CHECK_TRUE(CTextInputProcessor::s_hitTermAsyncReqFail.load() > 0, L"Branch s_hitTermAsyncReqFail must be hit");
        CHECK_TRUE(tip.GetPendingStatus(reinterpret_cast<uint64_t>(&ctxC)) == CTextInputProcessor::PendingTerminationStatus::FailedRequest, L"Status must be FailedRequest");

        tip.SetComposition(nullptr, nullptr);
        compC->Release();
    }

    // Branch 3: 异步执行稍后失败 (Async queued successfully, DoEditSession later returns E_FAIL)
    {
        auto* compD = new LifecyclePersistentComposition();
        tip.SetComposition(compD, &ctxD);

        ctxD.m_forceSyncBlocked = true;
        ctxD.m_deferAsyncSession = true;

        bool okTermD = tip.TerminateCompositionOnHost(&ctxD);
        CHECK_EQUAL(okTermD, true, L"TerminateCompositionOnHost returns true when async queued");
        CHECK_TRUE(tip.GetPendingStatus(reinterpret_cast<uint64_t>(&ctxD)) == CTextInputProcessor::PendingTerminationStatus::QueuedAsync, L"Status must be QueuedAsync");

        // Later: Host dequeues edit session and executes, but EndComposition fails
        g_failEndComposition = true;
        ctxD.ExecuteDeferredSession();
        g_failEndComposition = false;

        CHECK_TRUE(CTextInputProcessor::s_hitTermAsyncExecFail.load() > 0, L"Branch s_hitTermAsyncExecFail must be hit");
        CHECK_TRUE(tip.GetPendingStatus(reinterpret_cast<uint64_t>(&ctxD)) == CTextInputProcessor::PendingTerminationStatus::FailedAsync, L"Status must be FailedAsync");

        tip.SetComposition(nullptr, nullptr);
        compD->Release();
    }

    // Branch 4: 异步执行稍后成功 (Async queued successfully, DoEditSession succeeds)
    {
        auto* compE = new LifecyclePersistentComposition();
        tip.SetComposition(compE, &ctxE);

        ctxE.m_forceSyncBlocked = true;
        ctxE.m_deferAsyncSession = true;

        bool okTermE = tip.TerminateCompositionOnHost(&ctxE);
        CHECK_EQUAL(okTermE, true, L"Async queued");
        CHECK_TRUE(tip.GetPendingStatus(reinterpret_cast<uint64_t>(&ctxE)) == CTextInputProcessor::PendingTerminationStatus::QueuedAsync, L"Status QueuedAsync");

        // Host executes async edit session successfully
        ctxE.ExecuteDeferredSession();
        CHECK_TRUE(CTextInputProcessor::s_hitTermAsyncExecSuccess.load() > 0, L"Branch s_hitTermAsyncExecSuccess must be hit");
        CHECK_TRUE(tip.GetPendingStatus(reinterpret_cast<uint64_t>(&ctxE)) == CTextInputProcessor::PendingTerminationStatus::Succeeded, L"Status Succeeded");

        tip.SetComposition(nullptr, nullptr);
        compE->Release();
    }

    // Branch 5: 旧通知迟到 (Stale or unknown composition notification safely ignored)
    {
        auto* compStray = new LifecyclePersistentComposition();
        TfEditCookie ecWrite = 10;
        HRESULT hrLate = tip.OnCompositionTerminated(ecWrite, compStray);
        CHECK_EQUAL(hrLate, S_OK, L"OnCompositionTerminated must return S_OK on late notification");
        CHECK_TRUE(CTextInputProcessor::s_hitLateNotificationIgnored.load() > 0, L"Branch s_hitLateNotificationIgnored must be hit");
        compStray->Release();
    }

    // Branch 6: Deactivate 发生于排队期间 (Deactivate preserves in-flight async pending)
    {
        auto* compF = new LifecyclePersistentComposition();
        tip.SetComposition(compF, &ctxF);

        ctxF.m_forceSyncBlocked = true;
        ctxF.m_deferAsyncSession = true;

        tip.TerminateCompositionOnHost(&ctxF);
        CHECK_TRUE(tip.GetPendingStatus(reinterpret_cast<uint64_t>(&ctxF)) == CTextInputProcessor::PendingTerminationStatus::QueuedAsync, L"Status QueuedAsync before deactivate");

        // Deactivate called while edit session is queued in host
        tip.Deactivate();
        CHECK_TRUE(CTextInputProcessor::s_hitDeactivatePreservedPending.load() > 0, L"Branch s_hitDeactivatePreservedPending must be hit");
        CHECK_TRUE(tip.GetPendingStatus(reinterpret_cast<uint64_t>(&ctxF)) == CTextInputProcessor::PendingTerminationStatus::QueuedAsync, L"Pending item was preserved without premature reset");

        // Host async session completes after Deactivate
        ctxF.ExecuteDeferredSession();
        CHECK_TRUE(tip.GetPendingStatus(reinterpret_cast<uint64_t>(&ctxF)) == CTextInputProcessor::PendingTerminationStatus::Succeeded, L"Finalized cleanly after Deactivate");

        compF->Release();
    }

    // Assert that ALL target branch counters were hit (> 0)
    CHECK_TRUE(CTextInputProcessor::s_hitTermSyncFail.load() > 0, L"s_hitTermSyncFail must be > 0");
    CHECK_TRUE(CTextInputProcessor::s_hitTermAsyncQueued.load() > 0, L"s_hitTermAsyncQueued must be > 0");
    CHECK_TRUE(CTextInputProcessor::s_hitTermAsyncReqFail.load() > 0, L"s_hitTermAsyncReqFail must be > 0");
    CHECK_TRUE(CTextInputProcessor::s_hitTermAsyncExecFail.load() > 0, L"s_hitTermAsyncExecFail must be > 0");
    CHECK_TRUE(CTextInputProcessor::s_hitTermAsyncExecSuccess.load() > 0, L"s_hitTermAsyncExecSuccess must be > 0");
    CHECK_TRUE(CTextInputProcessor::s_hitLateNotificationIgnored.load() > 0, L"s_hitLateNotificationIgnored must be > 0");
    CHECK_TRUE(CTextInputProcessor::s_hitDeactivatePreservedPending.load() > 0, L"s_hitDeactivatePreservedPending must be > 0");

    std::wcout << L"[PASS] Multi-context termination branches verified with all branch counters > 0!" << std::endl;
    return 0;
}

// Test 12: Worker Thread Creation Failure Fallback, Concurrent Drain & CAS Retry (P1-3)
int TestWorkerThreadCreationFailureFallback() {
    std::wcout << L"[TEST] Testing Worker Thread Creation Failure Fallback, Concurrent Drain & CAS Retry (P1-3)..." << std::endl;

    IpcClient::ResetBranchCounters();
    CHECK_EQUAL(IpcClient::GetOrphanCount(), 0, L"Initial orphan count must be 0");

    const std::wstring testPipeName = L"\\\\.\\pipe\\PersonalIME_MockPipe_T12";
    MockIpcServer server(testPipeName);
    server.m_stallRequests.store(true); // Intentionally stall: accept connection, never reply!
    CHECK_TRUE(server.Start(1), L"Mock server must start");
    IpcClient::SetPipeNameForTest(testPipeName.c_str());

    // Enable thread creation failure injection
    IpcClient::SetFaultInjectCreateThread(true);

    // 1. Single-client timed-out request triggering CancelIoEx and CreateThread failure
    {
        IpcClient client1;
        IpcKeyRequest req{};
        req.msg_type = static_cast<uint32_t>(IpcMsgType::Ping);
        IpcKeyResponse resp{};
        // Stalled pipe will time out in 25ms, trigger CancelIoEx, and take fallback path
        bool ok = client1.SendRequest(req, resp, 25);
        CHECK_EQUAL(ok, false, L"SendRequest must return false on stalled pipe");
        CHECK_EQUAL(IpcClient::GetOrphanCount(), 1, L"Orphan count must be 1 after CreateThread failure fallback");
        CHECK_TRUE(IpcClient::s_hitFallbackEnqueue.load() > 0, L"Branch s_hitFallbackEnqueue must be hit");
    }

    // 2. Multi-threaded Concurrent Stalled Requests & CAS Retry Verification
    {
        // Produce uncompleted orphans with delayed completion signal
        auto* op1 = new IpcClient::AsyncIoOperation();
        auto* op2 = new IpcClient::AsyncIoOperation();
        ResetEvent(op1->hEvent);
        ResetEvent(op2->hEvent);

        IpcClient::EnqueueOrphanForTest(op1);
        IpcClient::EnqueueOrphanForTest(op2);

        // Force CAS retry on next drain pass
        IpcClient::SetForceCasRetryOnce(true);

        // Concurrently invoke DrainOrphanQueue while I/O is still pending (waitRes != WAIT_OBJECT_0)
        // to verify CAS retry and remaining queue preservation without node corruption
        std::thread tDrain1([]() {
            IpcClient::DrainOrphanQueue();
        });
        std::thread tDrain2([]() {
            IpcClient::DrainOrphanQueue();
        });
        tDrain1.join();
        tDrain2.join();

        CHECK_TRUE(IpcClient::s_hitDrainPendingRemaining.load() > 0, L"s_hitDrainPendingRemaining must be hit");
        CHECK_TRUE(IpcClient::s_hitCasRetry.load() > 0, L"s_hitCasRetry must be hit under concurrent drain contention");

        // Now signal the delayed completion events
        SetEvent(op1->hEvent);
        SetEvent(op2->hEvent);
        op1->Release();
        op2->Release();
    }

    // 3. Stop server / abort pipe: signals all kernel I/O completions (WAIT_OBJECT_0)
    server.Stop();
    Sleep(20);

    // Drain completed orphans
    IpcClient::DrainOrphanQueue();

    CHECK_EQUAL(IpcClient::GetOrphanCount(), 0, L"All orphans must be fully drained and freed (count == 0)");
    CHECK_TRUE(IpcClient::s_hitDrainCompleted.load() >= 2, L"s_hitDrainCompleted must be >= 2");
    CHECK_EQUAL(g_cDllRef, 0, L"All COM and module references fully restored (ref count == 0)");

    IpcClient::SetFaultInjectCreateThread(false);
    IpcClient::SetPipeNameForTest(nullptr);

    // Assert that ALL branch counters were hit (> 0)
    CHECK_TRUE(IpcClient::s_hitFallbackEnqueue.load() > 0, L"s_hitFallbackEnqueue must be > 0");
    CHECK_TRUE(IpcClient::s_hitCasRetry.load() > 0, L"s_hitCasRetry must be > 0");
    CHECK_TRUE(IpcClient::s_hitDrainPendingRemaining.load() > 0, L"s_hitDrainPendingRemaining must be > 0");
    CHECK_TRUE(IpcClient::s_hitDrainCompleted.load() > 0, L"s_hitDrainCompleted must be > 0");

    std::wcout << L"[PASS] Worker thread fallback, concurrent drain & CAS retry verified with all branch counters > 0!" << std::endl;
    return 0;
}

// Test 13: Pending COM RefCount Exact Pairing, Destruction Safety & Recovery Loop (P1 & P2)
class LifecycleCountingComposition : public MockComposition {
public:
    LONG refs = 1;
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override { return --refs; }
};

class LifecycleCountingContext : public MockContext {
public:
    LONG refs = 1;
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override { return --refs; }
};

int TestPendingRefCountAndDestructionSafety() {
    std::wcout << L"[TEST] Testing Pending RefCount Exact Pairing & Destruction Safety (P1 & P2)..." << std::endl;

    // Subtest 1: Failed pending held inside TIP directly destructed (Round 8 Repro verification)
    {
        auto* comp = new LifecycleCountingComposition();
        auto* ctx = new LifecycleCountingContext();
        {
            CTextInputProcessor tip;
            tip.SetClientIdForTest(1);
            tip.SetComposition(comp, ctx);
            g_failEndComposition = true;
            bool ok = tip.TerminateCompositionOnHost(ctx);
            g_failEndComposition = false;
            CHECK_EQUAL(ok, false, L"Must fail sync");
            CHECK_EQUAL(tip.GetPendingTerminationCount(), 1, L"Must have 1 failed pending");

            comp->Release();
            ctx->Release();
            CHECK_EQUAL(comp->refs, 1, L"Held by pending inside TIP");
            CHECK_EQUAL(ctx->refs, 1, L"Held by pending inside TIP");
        } // tip goes out of scope here!
        CHECK_EQUAL(comp->refs, 0, L"Comp refs must be exactly 0 after TIP destruction (no double-free)");
        CHECK_EQUAL(ctx->refs, 0, L"Context refs must be exactly 0 after TIP destruction (no leak)");
        delete comp;
        delete ctx;
    }

    // Subtest 2: Same scenario with pContextOverride = nullptr
    {
        auto* comp = new LifecycleCountingComposition();
        auto* ctx = new LifecycleCountingContext();
        {
            CTextInputProcessor tip;
            tip.SetClientIdForTest(1);
            tip.SetComposition(comp, ctx);
            g_failEndComposition = true;
            bool ok = tip.TerminateCompositionOnHost(nullptr);
            g_failEndComposition = false;
            CHECK_EQUAL(ok, false, L"Must fail sync");

            comp->Release();
            ctx->Release();
            CHECK_EQUAL(comp->refs, 1, L"Held by pending");
            CHECK_EQUAL(ctx->refs, 1, L"Held by pending");
        }
        CHECK_EQUAL(comp->refs, 0, L"Comp refs must be 0 with nullptr override");
        CHECK_EQUAL(ctx->refs, 0, L"Context refs must be 0 with nullptr override");
        delete comp;
        delete ctx;
    }

    // Subtest 3: Full Recovery Loop (Fail -> Switch -> Switch Back via OnKeyDown -> Recover -> Succeeded)
    {
        const std::wstring testPipeName = L"\\\\.\\pipe\\PersonalIME_MockPipe_T13";
        MockIpcServer server(testPipeName);
        CHECK_TRUE(server.Start(1), L"Mock server must start");
        IpcClient::SetPipeNameForTest(testPipeName.c_str());

        CTextInputProcessor tip;
        tip.SetClientIdForTest(1);
        MockContext ctxA;
        MockContext ctxB;

        auto* compA = new LifecyclePersistentComposition();
        compA->range->m_text = L"a";
        tip.SetComposition(compA, &ctxA);

        // Terminate on ctxA fails (e.g. host lock)
        g_failEndComposition = true;
        tip.TerminateCompositionOnHost(&ctxA);
        g_failEndComposition = false;

        CHECK_EQUAL(tip.GetPendingTerminationCount(), 1, L"Context A held in pending");

        // User works on Context B
        BOOL fEatenB = FALSE;
        tip.OnKeyDown(&ctxB, 'B', 0, &fEatenB);

        // Now lock on ctxA clears, user switches back to ctxA!
        BOOL fEatenA = FALSE;
        HRESULT hrA = tip.OnKeyDown(&ctxA, 'A', 0, &fEatenA);
        CHECK_EQUAL(hrA, S_OK, L"OnKeyDown must succeed on return to ctxA");
        CHECK_TRUE(CTextInputProcessor::s_hitPendingOnKeyDownRecovery.load() > 0, L"s_hitPendingOnKeyDownRecovery must be hit");
        CHECK_EQUAL(compA->m_endCompositionCalled, true, L"Context A composition must have ended");
        CHECK_EQUAL(tip.GetPendingTerminationCount(), 0, L"Pending terminations must be 0 after successful recovery");

        tip.SetComposition(nullptr, nullptr);
        compA->Release();
        server.Stop();
        IpcClient::SetPipeNameForTest(nullptr);
    }

    // Subtest 4: Deactivate Recovery (Fail -> Deactivate with cleared lock -> Clean finish)
    {
        CTextInputProcessor tip;
        tip.SetClientIdForTest(1);
        MockContext ctx;
        auto* comp = new LifecyclePersistentComposition();
        tip.SetComposition(comp, &ctx);

        g_failEndComposition = true;
        tip.TerminateCompositionOnHost(&ctx);
        g_failEndComposition = false;

        CHECK_EQUAL(tip.GetPendingTerminationCount(), 1, L"Pending has 1 item");

        // Deactivate called while lock is cleared -> recovers to terminal state!
        tip.Deactivate();
        CHECK_TRUE(CTextInputProcessor::s_hitDeactivateRetrySuccess.load() > 0, L"s_hitDeactivateRetrySuccess must be hit");
        CHECK_EQUAL(tip.GetPendingTerminationCount(), 0, L"Pending count must be 0 after Deactivate recovery");

        comp->Release();
    }

    std::wcout << L"[PASS] Pending RefCount exact pairing, destruction safety & recovery loop verified!" << std::endl;
    return 0;
}

// A queued termination owns its context until completion. Returning to that
// context must not replace the host's queued request or start overlapping input.
int TestQueuedTerminationSingleFlight() {
    MockContext context;
    CTextInputProcessor tip;
    tip.SetClientIdForTest(1);
    auto* comp = new LifecyclePersistentComposition();
    tip.SetComposition(comp, &context);
    context.m_forceSyncBlocked = true;
    context.m_deferAsyncSession = true;
    CHECK_TRUE(tip.TerminateCompositionOnHost(&context), L"Initial request must be queued");
    CHECK_EQUAL(context.m_asyncRequestCount, 1, L"Exactly one host request queued");
    const auto generation = tip.GetPendingTerminations()[0].generation;
    CHECK_EQUAL(tip.TerminateCompositionOnHost(&context), false, L"Queued work must not be resubmitted");
    BOOL eaten = TRUE;
    tip.OnKeyDown(&context, 'B', 0, &eaten);
    CHECK_EQUAL(eaten, FALSE, L"New input must pass through while old termination is queued");
    CHECK_EQUAL(context.m_asyncRequestCount, 1, L"Returning to context must not replace deferred session");
    CHECK_EQUAL(tip.GetPendingTerminations()[0].generation, generation, L"Queued generation must remain unchanged");
    context.ExecuteDeferredSession();
    CHECK_EQUAL(tip.GetPendingTerminationCount(), 0, L"Completion must release pending ownership");
    CHECK_EQUAL(context.m_cRef, 1, L"Context references return to caller after callback");
    comp->Release();
    tip.Deactivate();
    std::wcout << L"[PASS] Queued termination remains single-flight until completion!" << std::endl;
    return 0;
}

int TestDeactivateFailureRemainsRecoverable() {
    MockContext context;
    CTextInputProcessor tip;
    tip.SetClientIdForTest(1);
    auto* comp = new LifecyclePersistentComposition();
    tip.SetComposition(comp, &context);
    g_failEndComposition = true;
    tip.TerminateCompositionOnHost(&context);
    tip.Deactivate();
    CHECK_EQUAL(tip.GetPendingTerminationCount(), 1, L"Failed termination is not host completion");
    tip.Deactivate();
    CHECK_EQUAL(tip.GetPendingTerminationCount(), 1, L"Repeated deactivate must neither spin nor discard ownership");
    g_failEndComposition = false;
    tip.SetClientIdForTest(1);
    CHECK_TRUE(tip.TerminateCompositionOnHost(&context), L"Reactivation can retry failed host termination");
    CHECK_EQUAL(tip.GetPendingTerminationCount(), 0, L"Recovery reaches completion");
    CHECK_EQUAL(context.m_cRef, 1, L"Recovery releases the context ownership");
    comp->Release();
    tip.Deactivate();
    std::wcout << L"[PASS] Persistent deactivate failure retains ownership and recovers on reactivation!" << std::endl;
    return 0;
}

// Stateful Mock IPC Server supporting candidate snapshots, commits, CommitSuccess/CommitCancel, and paging
class SnapshotMockIpcServer {
public:
    std::wstring m_pipeName;
    HANDLE m_hStopEvent = nullptr;
    std::vector<HANDLE> m_threads;
    std::vector<HANDLE> m_pipes;
    CRITICAL_SECTION m_cs;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_instancesReady{0};

    struct ContextMockState {
        std::wstring composition;
        std::wstring prev_composition;
        uint32_t candidate_version = 0;
        uint32_t page_index = 0;
        uint32_t page_count = 1;
        std::vector<std::wstring> candidates;
    };

    struct PendingCommitRecord {
        uint64_t session_id = 0;
        uint64_t context_id = 0;
        std::wstring text;
        std::wstring comp_before;
        uint32_t page_before = 0;
        std::vector<std::wstring> cands_before;
        uint32_t generation = 0;
        int state = 0; // 0 = Pending, 1 = Recorded (Success), 2 = Cancelled
    };

    std::map<uint64_t, ContextMockState> m_ctxStates;
    std::map<uint64_t, PendingCommitRecord> m_pendingCommits;
    std::map<uint64_t, IpcCommitDisposition> m_terminalDispositions;
    uint64_t m_nextEventId = 5000;
    std::atomic<int> m_commitSuccessCount{0};
    std::atomic<int> m_commitCancelCount{0};
    std::atomic<int> m_commitTerminalCount{0};
    std::atomic<int> m_dropNextCommitTerminal{0};
    std::atomic<int> m_queryCommitStatusCount{0};
    std::atomic<int> m_learnedCount{0};
    std::atomic<int> m_resetCount{0};
    std::atomic<uint32_t> m_lastSelectVersion{0};
    std::atomic<uint32_t> m_pluginsRevTag{0}; // If > 0, sets IPC_STATUS_HAS_PLUGINS_REV in status_flags
    std::atomic<bool> m_advertiseStage7{false};
    std::atomic<bool> m_rejectStage7KeyDown{false};
    std::atomic<uint32_t> m_lastKeyDownProtocol{0};
    std::wstring m_lastSelectedText;

    SnapshotMockIpcServer(const std::wstring& pipeName) : m_pipeName(pipeName) {
        m_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        InitializeCriticalSection(&m_cs);
    }

    ~SnapshotMockIpcServer() {
        Stop();
        DeleteCriticalSection(&m_cs);
        if (m_hStopEvent) CloseHandle(m_hStopEvent);
    }

    bool Start(int numInstances = 2) {
        m_running.store(true);
        m_instancesReady.store(0);
        for (int i = 0; i < numInstances; ++i) {
            HANDLE hThread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
            if (hThread) m_threads.push_back(hThread);
        }
        while (m_instancesReady.load() < numInstances) Sleep(5);
        return !m_threads.empty();
    }

    void Stop() {
        if (m_running.exchange(false)) {
            SetEvent(m_hStopEvent);
            EnterCriticalSection(&m_cs);
            for (HANDLE h : m_pipes) {
                if (h != INVALID_HANDLE_VALUE) CancelIoEx(h, nullptr);
            }
            LeaveCriticalSection(&m_cs);
            for (HANDLE h : m_threads) {
                WaitForSingleObject(h, INFINITE);
                CloseHandle(h);
            }
            m_threads.clear();
            EnterCriticalSection(&m_cs);
            for (HANDLE h : m_pipes) {
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            }
            m_pipes.clear();
            LeaveCriticalSection(&m_cs);
        }
    }

    int GetPendingCommitState(uint64_t evId) {
        EnterCriticalSection(&m_cs);
        int st = -1;
        auto it = m_pendingCommits.find(evId);
        if (it != m_pendingCommits.end()) st = it->second.state;
        LeaveCriticalSection(&m_cs);
        return st;
    }

    static void FillCandidates(IpcKeyResponse& resp, const ContextMockState& st) {
        resp.has_composition = 1;
        wcsncpy_s(resp.composition, st.composition.c_str(), _TRUNCATE);
        resp.candidate_version = st.candidate_version;
        resp.page_index = st.page_index;
        resp.page_count = st.page_count;
        resp.candidate_count = static_cast<uint32_t>((std::min)(st.candidates.size(), size_t(9)));
        for (uint32_t i = 0; i < resp.candidate_count; ++i) {
            std::wstring label = std::to_wstring(i + 1) + L". " + st.candidates[i];
            wcsncpy_s(resp.candidates[i], label.c_str(), _TRUNCATE);
        }
    }

    static DWORD WINAPI ThreadProc(LPVOID lpParam) {
        auto* pThis = static_cast<SnapshotMockIpcServer*>(lpParam);
        while (pThis->m_running.load()) {
            HANDLE hPipe = CreateNamedPipeW(
                pThis->m_pipeName.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                16384, 16384, 0, nullptr);
            if (hPipe == INVALID_HANDLE_VALUE) {
                Sleep(10);
                continue;
            }
            EnterCriticalSection(&pThis->m_cs);
            pThis->m_pipes.push_back(hPipe);
            LeaveCriticalSection(&pThis->m_cs);
            pThis->m_instancesReady.fetch_add(1);

            OVERLAPPED ovConnect{};
            ovConnect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ConnectNamedPipe(hPipe, &ovConnect)) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    HANDLE handles[2] = { ovConnect.hEvent, pThis->m_hStopEvent };
                    if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0) {
                        CloseHandle(ovConnect.hEvent);
                        break;
                    }
                } else if (err != ERROR_PIPE_CONNECTED) {
                    CloseHandle(ovConnect.hEvent);
                    continue;
                }
            }
            CloseHandle(ovConnect.hEvent);

            while (pThis->m_running.load()) {
                IpcKeyRequest req{};
                DWORD bytesRead = 0;
                OVERLAPPED ovRead{};
                ovRead.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                BOOL okRead = ReadFile(hPipe, &req, sizeof(req), &bytesRead, &ovRead);
                if (!okRead && GetLastError() == ERROR_IO_PENDING) {
                    HANDLE handles[2] = { ovRead.hEvent, pThis->m_hStopEvent };
                    if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0) {
                        CloseHandle(ovRead.hEvent);
                        break;
                    }
                    GetOverlappedResult(hPipe, &ovRead, &bytesRead, FALSE);
                }
                CloseHandle(ovRead.hEvent);
                if (bytesRead != sizeof(req)) break;

                IpcKeyResponse resp{};
                resp.msg_type = req.msg_type;
                resp.session_id = req.session_id;
                resp.context_id = req.context_id;
                resp.protocol_version = req.protocol_version;
                resp.handled = 1;
                uint32_t pRev = pThis->m_pluginsRevTag.load();
                if (pRev > 0) {
                    resp.status_flags |= IPC_STATUS_HAS_PLUGINS_REV | ((pRev & 0xFFu) << IPC_PLUGINS_REV_SHIFT);
                }
                if (pThis->m_advertiseStage7.load() &&
                    (req.protocol_version >= IPC_PROTOCOL_VERSION_STAGE7 ||
                     req.msg_type == static_cast<uint32_t>(IpcMsgType::Ping))) {
                    resp.status_flags |= IPC_STATUS_STAGE7_CAPABLE;
                }

                EnterCriticalSection(&pThis->m_cs);
                auto& st = pThis->m_ctxStates[req.context_id];
                if (req.msg_type == static_cast<uint32_t>(IpcMsgType::KeyDown)) {
                    pThis->m_lastKeyDownProtocol.store(req.protocol_version);
                    if (pThis->m_rejectStage7KeyDown.load() &&
                        req.protocol_version >= IPC_PROTOCOL_VERSION_STAGE7) {
                        resp.status_flags &= ~IPC_STATUS_STAGE7_CAPABLE;
                        resp.handled = 0;
                    } else {
                    uint32_t vk = req.vk_code;
                    if (vk >= 'A' && vk <= 'Z') {
                        st.prev_composition = st.composition;
                        st.composition.push_back(static_cast<wchar_t>(L'a' + (vk - 'A')));
                        st.candidate_version++;
                        st.page_index = 0;
                        if (vk == 'A') {
                            st.page_count = 2;
                            st.candidates = { L"工", L"戈", L"阿", L"啊", L"呵" };
                        } else if (vk == 'B') {
                            st.page_count = 1;
                            st.candidates = { L"节", L"芭", L"吧" };
                        } else if (vk == 'V') {
                            st.page_count = 1;
                            st.candidates = { L"版本 [beta]", L"测试 [alpha]" };
                        } else if (vk == 'L') {
                            st.page_count = 1;
                            // 123 UTF-16 units (> 122 limit with "1. " prefix) alongside "版本 [beta]" on the same page
                            std::wstring longCore(116, L'汉');
                            longCore += L" [beta]"; // 116 + 7 = 123 UTF-16 units
                            st.candidates = { longCore, L"版本 [beta]" };
                        } else {
                            st.page_count = 1;
                            st.candidates = { L"华", L"花", L"化" };
                        }
                        FillCandidates(resp, st);
                        if (req.protocol_version >= IPC_PROTOCOL_VERSION_STAGE7) {
                            resp.status_flags |= IPC_STATUS_HAS_QUERY_CODE | IPC_STATUS_HAS_CAND_CORES;
                            wcsncpy_s(resp.commit_text, L"da'jia", _TRUNCATE);
                            resp.commit_length = 0;
                            for (uint32_t i = 0; i < resp.candidate_count; ++i) {
                                size_t labelLen = wcslen(resp.candidates[i]);
                                if (labelLen + 3 <= 128 && i < st.candidates.size()) {
                                    resp.candidates[i][labelLen + 1] = static_cast<wchar_t>(1 + st.candidates[i].length());
                                    resp.candidates[i][labelLen + 2] = L'\0';
                                } else if (labelLen + 1 < 128) {
                                    resp.candidates[i][labelLen + 1] = L'\0';
                                }
                            }
                        }
                    } else if (vk == VK_NEXT) {
                        if (req.candidate_version != st.candidate_version) {
                            resp.handled = 0;
                        } else {
                            st.page_index = 1;
                            st.candidate_version++;
                            st.candidates = { L"腌", L"锕", L"錒" };
                            FillCandidates(resp, st);
                        }
                    } else if (vk == VK_PRIOR) {
                        if (req.candidate_version != st.candidate_version) {
                            resp.handled = 0;
                        } else {
                            st.page_index = 0;
                            st.candidate_version++;
                            st.candidates = { L"工", L"戈", L"阿", L"啊", L"呵" };
                            FillCandidates(resp, st);
                        }
                    } else if (vk == VK_BACK) {
                        if (st.composition.empty()) {
                            resp.handled = 0;
                        } else {
                            st.prev_composition = st.composition;
                            st.composition.pop_back();
                            if (!st.composition.empty()) {
                                st.candidate_version++;
                                st.page_index = 0;
                                st.page_count = 2;
                                st.candidates = { L"工", L"戈", L"阿", L"啊", L"呵" };
                                FillCandidates(resp, st);
                            } else {
                                st.candidates.clear();
                                resp.has_composition = 1;
                                resp.composition[0] = L'\0';
                                resp.candidate_count = 0;
                            }
                        }
                    } else if (vk == VK_ESCAPE) {
                        if (st.composition.empty()) {
                            resp.handled = 0;
                        } else {
                            st.composition.clear();
                            st.candidates.clear();
                            resp.has_composition = 1;
                            resp.composition[0] = L'\0';
                            resp.candidate_count = 0;
                        }
                    } else if (vk == VK_SPACE || (vk >= '1' && vk <= '9')) {
                        pThis->m_lastSelectVersion.store(req.candidate_version);
                        if (st.candidates.empty() || req.candidate_version != st.candidate_version) {
                            resp.handled = 0;
                        } else {
                            size_t idx = (vk == VK_SPACE) ? 0 : static_cast<size_t>(vk - '1');
                            if (idx < st.candidates.size()) {
                                uint64_t evId = ++pThis->m_nextEventId;
                                PendingCommitRecord rec{};
                                rec.session_id = req.session_id;
                                rec.context_id = req.context_id;
                                rec.text = st.candidates[idx];
                                rec.comp_before = st.composition;
                                rec.page_before = st.page_index;
                                rec.cands_before = st.candidates;
                                rec.generation = st.candidate_version;
                                rec.state = 0; // Pending
                                pThis->m_pendingCommits[evId] = rec;
                                pThis->m_lastSelectedText = rec.text;

                                resp.has_commit = 1;
                                wcsncpy_s(resp.commit_text, rec.text.c_str(), _TRUNCATE);
                                resp.commit_length = static_cast<uint32_t>(rec.text.length());
                                resp.commit_event_id = evId;
                                resp.candidate_version = rec.generation;
                                resp.has_composition = 1;
                                resp.composition[0] = L'\0';
                                resp.candidate_count = 0;

                                st.composition.clear();
                                st.candidates.clear();
                            } else {
                                resp.handled = 0;
                            }
                        }
                    }
                    }
                } else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::CommitTerminal)) {
                    pThis->m_commitTerminalCount.fetch_add(1);
                    if (pThis->m_dropNextCommitTerminal.load() > 0) {
                        pThis->m_dropNextCommitTerminal.fetch_sub(1);
                        resp.handled = 0;
                    } else {
                        IpcCommitDisposition reqDisp = static_cast<IpcCommitDisposition>(req.mode_arg);
                        auto termIt = pThis->m_terminalDispositions.find(req.event_id);
                        if (termIt != pThis->m_terminalDispositions.end()) {
                            resp.commit_length = static_cast<uint32_t>(termIt->second);
                            if (termIt->second != reqDisp) {
                                resp.status_flags |= IPC_STATUS_CONFLICT_ERROR;
                                resp.handled = 0;
                            } else {
                                resp.handled = 1;
                            }
                        } else {
                            auto pendIt = pThis->m_pendingCommits.find(req.event_id);
                            if (pendIt != pThis->m_pendingCommits.end()) {
                                if (pendIt->second.generation != 0 &&
                                    req.candidate_version != pendIt->second.generation) {
                                    resp.status_flags |= IPC_STATUS_CONFLICT_ERROR;
                                    resp.handled = 0;
                                } else {
                                    pThis->m_terminalDispositions[req.event_id] = reqDisp;
                                    pendIt->second.state = (reqDisp == IpcCommitDisposition::ConfirmedWritten) ? 1 : 2;
                                    if (reqDisp == IpcCommitDisposition::ConfirmedWritten) {
                                        pThis->m_commitSuccessCount.fetch_add(1);
                                        pThis->m_learnedCount.fetch_add(1);
                                    } else {
                                        pThis->m_commitCancelCount.fetch_add(1);
                                    }
                                    resp.commit_length = static_cast<uint32_t>(reqDisp);
                                    resp.handled = 1;
                                }
                            } else {
                                resp.handled = 0;
                            }
                        }
                    }
                } else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::QueryCommitStatus)) {
                    pThis->m_queryCommitStatusCount.fetch_add(1);
                    auto termIt = pThis->m_terminalDispositions.find(req.event_id);
                    if (termIt != pThis->m_terminalDispositions.end()) {
                        resp.commit_length = static_cast<uint32_t>(termIt->second);
                        resp.handled = 1;
                    } else {
                        resp.commit_length = static_cast<uint32_t>(IpcCommitDisposition::None);
                        resp.handled = 1;
                    }
                } else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::CommitSuccess)) {
                    auto it = pThis->m_pendingCommits.find(req.event_id);
                    if (it != pThis->m_pendingCommits.end() &&
                        it->second.session_id == req.session_id &&
                        it->second.context_id == req.context_id &&
                        it->second.state != 2) {
                        if (it->second.state == 0) {
                            it->second.state = 1;
                            pThis->m_commitSuccessCount.fetch_add(1);
                            pThis->m_learnedCount.fetch_add(1);
                        }
                        resp.handled = 1;
                    } else {
                        resp.handled = 0;
                    }
                } else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::CommitCancel)) {
                    auto it = pThis->m_pendingCommits.find(req.event_id);
                    if (it != pThis->m_pendingCommits.end() &&
                        it->second.session_id == req.session_id &&
                        it->second.context_id == req.context_id &&
                        it->second.state == 0) {
                        it->second.state = 2;
                        pThis->m_commitCancelCount.fetch_add(1);
                        st.composition = it->second.comp_before;
                        st.page_index = it->second.page_before;
                        st.candidates = it->second.cands_before;
                        st.candidate_version++;
                        FillCandidates(resp, st);
                        resp.handled = 1;
                    } else {
                        resp.handled = 0;
                    }
                } else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::Rollback)) {
                    st.composition = st.prev_composition;
                    if (!st.composition.empty()) {
                        st.candidate_version++;
                        FillCandidates(resp, st);
                    } else {
                        st.candidates.clear();
                        resp.has_composition = 0;
                        resp.candidate_count = 0;
                    }
                } else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::Reset)) {
                    pThis->m_resetCount.fetch_add(1);
                    st.composition.clear();
                    st.candidates.clear();
                    resp.has_composition = 1;
                    resp.composition[0] = L'\0';
                }
                LeaveCriticalSection(&pThis->m_cs);

                DWORD written = 0;
                OVERLAPPED ovWrite{};
                ovWrite.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                BOOL okWrite = WriteFile(hPipe, &resp, sizeof(resp), &written, &ovWrite);
                if (!okWrite && GetLastError() == ERROR_IO_PENDING) {
                    WaitForSingleObject(ovWrite.hEvent, INFINITE);
                }
                CloseHandle(ovWrite.hEvent);
            }
            DisconnectNamedPipe(hPipe);
        }
        return 0;
    }
};

// Test 16: P1 #1 - TF_S_ASYNC Deferred EditSession Lifecycle & Receipt / Pending Verification
int TestAsyncQueuedEditSessionCommitAndFailureMatrix() {
    std::wcout << L"[TEST] Testing TF_S_ASYNC Deferred EditSession Lifecycle & Receipt Matrix (P1-Async)..." << std::endl;

    const std::wstring pipeName = L"\\\\.\\pipe\\PersonalIME_MockPipe_AsyncEdit_" + std::to_wstring(GetCurrentProcessId());
    SnapshotMockIpcServer server(pipeName);
    CHECK_TRUE(server.Start(2), L"SnapshotMockIpcServer must start");
    IpcClient::SetPipeNameForTest(pipeName.c_str());

    // 1. Sync Blocked -> Async Queued (TF_S_ASYNC) -> Unexecuted has NO ACK / NO Learning -> Execute Success
    {
        CTextInputProcessor tip;
        tip.SetClientIdForTest(1);
        MockContext ctx;
        auto* comp = new LifecyclePersistentComposition();

        // Step 1a: Type 'A' synchronously to establish composition "a" and candidates (ver=1)
        BOOL eaten = FALSE;
        tip.OnKeyDown(&ctx, 'A', 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"Sync key 'A' eaten");
        // Attach persistent composition so we can inspect range text after commit
        tip.SetComposition(comp, &ctx, CompositionState::Composing);
        comp->range->m_text = L"a";

        // Step 1b: Force sync failure -> async deferral for Space commit
        ctx.m_forceSyncBlocked = true;
        ctx.m_deferAsyncSession = true;
        int learnedBefore = server.m_learnedCount.load();
        int successBefore = server.m_commitSuccessCount.load();

        BOOL eatenSpace = FALSE;
        tip.OnKeyDown(&ctx, VK_SPACE, 0, &eatenSpace);
        CHECK_EQUAL(eatenSpace, TRUE, L"Queued async commit key is eaten on submission");
        CHECK_EQUAL(tip.GetPendingEditSessionCount(), 1, L"Must have 1 queued pending edit session");
        CHECK_TRUE(tip.HasQueuedEditSession(reinterpret_cast<uint64_t>(&ctx)), L"Context must report HasQueuedEditSession=true");

        uint64_t evId = tip.GetPendingEditSessions()[0].commitEventId;
        CHECK_TRUE(evId != 0, L"Queued commit must have valid commitEventId");

        // CRITICAL P1 ASSERTIONS BEFORE EXECUTION:
        // Host text has NOT been written yet; NO CommitSuccess ACK; NO learning; server pending is still Pending (0)!
        CHECK_EQUAL(comp->range->m_text, L"a", L"Host range text must still be 'a' before deferred DoEditSession runs");
        CHECK_EQUAL(server.m_commitSuccessCount.load(), successBefore, L"MUST have 0 CommitSuccess ACK while TF_S_ASYNC is only queued!");
        CHECK_EQUAL(server.m_learnedCount.load(), learnedBefore, L"MUST have 0 learning while TF_S_ASYNC is only queued!");
        CHECK_EQUAL(server.GetPendingCommitState(evId), 0, L"Server event must remain Pending (0) while queued");

        // While queued, subsequent keystroke on same context must NOT overlap or replace m_pDeferredSession
        BOOL eatenNext = TRUE;
        tip.OnKeyDown(&ctx, 'B', 0, &eatenNext);
        CHECK_EQUAL(eatenNext, FALSE, L"Subsequent keystroke while queued must not be eaten or overlap");
        CHECK_EQUAL(ctx.m_asyncRequestCount, 1, L"Only 1 async session requested (no overlap)");

        // Step 1c: Now host executes the deferred edit session -> Success!
        ITfEditSession* rawSession = ctx.m_pDeferredSession;
        rawSession->AddRef();
        ctx.ExecuteDeferredSession();

        CHECK_EQUAL(comp->range->m_text, L"工", L"Host range text must now be '工' after DoEditSession executes");
        CHECK_EQUAL(comp->m_endCompositionCalled, true, L"EndComposition called on commit");
        CHECK_EQUAL(tip.GetPendingEditSessionCount(), 0, L"Pending edit session cleared on completion");
        CHECK_EQUAL(server.m_commitSuccessCount.load(), successBefore + 1, L"CommitSuccess sent exactly once after actual DoEditSession execution");
        CHECK_EQUAL(server.m_learnedCount.load(), learnedBefore + 1, L"Learning recorded after actual DoEditSession execution");
        CHECK_EQUAL(server.GetPendingCommitState(evId), 1, L"Server event state is now Recorded (1)");

        // Duplicate DoEditSession execution on the same session must be a strict no-op (single terminal state)
        rawSession->DoEditSession(1001);
        rawSession->Release();
        CHECK_EQUAL(server.m_commitSuccessCount.load(), successBefore + 1, L"Duplicate DoEditSession must NOT send duplicate CommitSuccess");

        comp->Release();
        tip.Deactivate();
    }

    // 2. Sync Blocked -> Async Queued -> Deferred SetText Fails (Unmodified) -> CommitCancel & Composition Restored
    {
        CTextInputProcessor tip;
        tip.SetClientIdForTest(1);
        MockContext ctx;
        auto* comp = new LifecyclePersistentComposition();

        BOOL eaten = FALSE;
        tip.OnKeyDown(&ctx, 'A', 0, &eaten);
        tip.SetComposition(comp, &ctx, CompositionState::Composing);
        comp->range->m_text = L"a";

        ctx.m_forceSyncBlocked = true;
        ctx.m_deferAsyncSession = true;
        int learnedBefore = server.m_learnedCount.load();
        int cancelBefore = server.m_commitCancelCount.load();

        tip.OnKeyDown(&ctx, VK_SPACE, 0, &eaten);
        uint64_t evId = tip.GetPendingEditSessions()[0].commitEventId;
        CHECK_EQUAL(server.GetPendingCommitState(evId), 0, L"Server event is Pending before execution");

        // Inject SetText failure when deferred session executes!
        g_failSetText = true;
        ctx.ExecuteDeferredSession();
        g_failSetText = false;

        CHECK_EQUAL(comp->range->m_text, L"a", L"Host text untouched on SetText failure");
        CHECK_EQUAL(server.m_learnedCount.load(), learnedBefore, L"Failed async SetText must NEVER learn!");
        CHECK_EQUAL(server.m_commitCancelCount.load(), cancelBefore + 1, L"Failed async SetText must send CommitCancel!");
        CHECK_EQUAL(server.GetPendingCommitState(evId), 2, L"Server event state must be Cancelled (2)");
        CHECK_EQUAL(tip.GetPendingEditSessionCount(), 0, L"Pending edit session removed after failure handling");

        // And because CommitCancel synchronized the restored candidate version, immediate retry when lock clears succeeds!
        ctx.m_forceSyncBlocked = false;
        ctx.m_deferAsyncSession = false;
        BOOL eatenRetry = FALSE;
        tip.OnKeyDown(&ctx, VK_SPACE, 0, &eatenRetry);
        CHECK_EQUAL(eatenRetry, TRUE, L"Immediate retry after async CommitCancel snapshot sync must succeed");
        CHECK_EQUAL(comp->range->m_text, L"工", L"Retry committed '工'");

        comp->Release();
        tip.Deactivate();
    }

    // 3. Sync Blocked -> Async Queued -> Deferred EndComposition Fails (PartiallyModified)
    {
        CTextInputProcessor tip;
        tip.SetClientIdForTest(1);
        MockContext ctx;
        auto* comp = new LifecyclePersistentComposition();

        BOOL eaten = FALSE;
        tip.OnKeyDown(&ctx, 'A', 0, &eaten);
        tip.SetComposition(comp, &ctx, CompositionState::Composing);
        comp->range->m_text = L"a";

        ctx.m_forceSyncBlocked = true;
        ctx.m_deferAsyncSession = true;
        int successBefore = server.m_commitSuccessCount.load();

        tip.OnKeyDown(&ctx, VK_SPACE, 0, &eaten);
        uint64_t evId = tip.GetPendingEditSessions()[0].commitEventId;
        CHECK_EQUAL(server.m_commitSuccessCount.load(), successBefore, L"No CommitSuccess before execution");

        g_failEndComposition = true;
        ctx.ExecuteDeferredSession();
        g_failEndComposition = false;

        CHECK_EQUAL(comp->range->m_text, L"工", L"SetText modified host text to '工'");
        CHECK_EQUAL(server.m_commitSuccessCount.load(), successBefore + 1, L"PartiallyModified confirms commit because text is on host");
        CHECK_EQUAL(server.GetPendingCommitState(evId), 1, L"Server event is Recorded");
        CHECK_TRUE(tip.GetCompositionState(&ctx) == CompositionState::CommitPendingEnd, L"State marked CommitPendingEnd for recovery");

        tip.SetComposition(nullptr, nullptr);
        comp->Release();
        tip.Deactivate();
    }

    // 4. Sync Blocked -> Async Queued -> Unfocus / Context Switch BEFORE execution (No UI Resurrection!)
    {
        CTextInputProcessor tip;
        tip.SetClientIdForTest(1);
        MockContext ctxA;
        MockContext ctxB;

        // Queue async composition 'A' on ctxA
        ctxA.m_forceSyncBlocked = true;
        ctxA.m_deferAsyncSession = true;
        BOOL eatenA = FALSE;
        tip.OnKeyDown(&ctxA, 'A', 0, &eatenA);
        CHECK_EQUAL(tip.GetPendingEditSessionCount(), 1, L"ctxA has 1 queued composition session");

        // Switch to ctxB synchronously and type 'B' -> candidate window now shows ctxB's candidates!
        BOOL eatenB = FALSE;
        tip.OnKeyDown(&ctxB, 'B', 0, &eatenB);
        CHECK_EQUAL(eatenB, TRUE, L"ctxB key 'B' handled");
        CHECK_EQUAL(IsWindowVisible(tip.GetCandidateWindowForTest().GetHwnd()), TRUE, L"Candidate window visible for ctxB");
        CHECK_EQUAL(tip.GetCandidateWindowForTest().GetSnapshot().context_id, reinterpret_cast<uint64_t>(&ctxB), L"Window belongs to ctxB");

        int resetBefore = server.m_resetCount.load();
        // Now ctxA's late async session executes! It MUST NOT resurrect ctxA's UI or hide ctxB's active window!
        ctxA.ExecuteDeferredSession();
        CHECK_EQUAL(IsWindowVisible(tip.GetCandidateWindowForTest().GetHwnd()), TRUE, L"ctxB candidate window must remain visible");
        CHECK_EQUAL(tip.GetCandidateWindowForTest().GetSnapshot().context_id, reinterpret_cast<uint64_t>(&ctxB), L"Window still belongs to ctxB, not resurrected by ctxA!");
        CHECK_TRUE(server.m_resetCount.load() > resetBefore, L"ctxA server state was cleanly reset after aborted async composition");

        // Now queue an async session on ctxB, then call OnSetFocus(FALSE) before it executes!
        ctxB.m_forceSyncBlocked = true;
        ctxB.m_deferAsyncSession = true;
        tip.OnKeyDown(&ctxB, 'A', 0, &eatenB);
        tip.OnSetFocus(FALSE);
        CHECK_EQUAL(IsWindowVisible(tip.GetCandidateWindowForTest().GetHwnd()), FALSE, L"Window hidden on OnSetFocus(FALSE)");

        // Late execution after OnSetFocus(FALSE) must NOT resurrect candidate window!
        ctxB.ExecuteDeferredSession();
        CHECK_EQUAL(IsWindowVisible(tip.GetCandidateWindowForTest().GetHwnd()), FALSE, L"Late async session after focus loss must NEVER resurrect candidate window!");

        tip.Deactivate();
    }

    server.Stop();
    IpcClient::SetPipeNameForTest(nullptr);
    std::wcout << L"[PASS] TF_S_ASYNC deferred EditSession lifecycle, receipts, failure rollback & focus safety verified!" << std::endl;
    return 0;
}

// Test 17: P1 #2 - Snapshot-Bound Mouse Selection, Press-to-Release Mutation, Cross-Context & Late Paging
int TestSnapshotBoundMouseSelectionAndLatePaging() {
    std::wcout << L"[TEST] Testing Snapshot-Bound Mouse Selection, Cross-Context & Late Paging (P1-Snapshot)..." << std::endl;

    const std::wstring pipeName = L"\\\\.\\pipe\\PersonalIME_MockPipe_SnapMouse_" + std::to_wstring(GetCurrentProcessId());
    SnapshotMockIpcServer server(pipeName);
    CHECK_TRUE(server.Start(2), L"SnapshotMockIpcServer must start");
    IpcClient::SetPipeNameForTest(pipeName.c_str());

    CTextInputProcessor tip;
    tip.SetClientIdForTest(1);
    MockContext ctxA;
    MockContext ctxB;
    CCandidateWindow& wnd = tip.GetCandidateWindowForTest();

    // 1. Old page displayed while new result is queued asynchronously (TF_S_ASYNC): stale click rejected!
    {
        BOOL eaten = FALSE;
        tip.OnKeyDown(&ctxA, 'A', 0, &eaten); // Displays version 1: "1. 工", "2. 戈", ...
        SendMessageW(wnd.GetHwnd(), WM_PAINT, 0, 0);
        uint32_t ver1 = wnd.GetSnapshot().candidate_version;
        CHECK_TRUE(ver1 > 0, L"Initial snapshot version > 0");
        CHECK_EQUAL(wnd.GetSnapshot().context_id, reinterpret_cast<uint64_t>(&ctxA), L"Snapshot bound to ctxA");

        // Queue 'B' asynchronously on ctxA: server advances to version 2 ("1. 节", "2. 芭"),
        // while wnd still displays version 1!
        ctxA.m_forceSyncBlocked = true;
        ctxA.m_deferAsyncSession = true;
        tip.OnKeyDown(&ctxA, 'B', 0, &eaten);
        CHECK_EQUAL(wnd.GetSnapshot().candidate_version, ver1, L"Window still displays old version 1 while version 2 is queued");

        // User clicks candidate 1 ("2. 戈" on screen) while version 2 is queued!
        // MUST BE REJECTED: must NOT commit version 2's candidate 1 ("芭")!
        int successBefore = server.m_commitSuccessCount.load();
        const auto& rects = wnd.GetCandidateRects();
        POINT ptCand1 = { (rects[1].left + rects[1].right) / 2, (rects[1].top + rects[1].bottom) / 2 };
        SendMessageW(wnd.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ptCand1.x, ptCand1.y));
        SendMessageW(wnd.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(ptCand1.x, ptCand1.y));
        CHECK_EQUAL(server.m_commitSuccessCount.load(), successBefore, L"Click on stale window while new edit session is queued MUST be rejected!");

        // Now execute deferred session so version 2 is actually displayed!
        ctxA.m_forceSyncBlocked = false;
        ctxA.m_deferAsyncSession = false;
        ctxA.ExecuteDeferredSession();
        SendMessageW(wnd.GetHwnd(), WM_PAINT, 0, 0);
        uint32_t ver2 = wnd.GetSnapshot().candidate_version;
        CHECK_TRUE(ver2 > ver1, L"Window snapshot updated to version 2 after async session completed");

        // 2. Candidate changes between WM_LBUTTONDOWN and WM_LBUTTONUP (点按之间候选改变): click rejected!
        const auto& rects2 = wnd.GetCandidateRects();
        POINT pt2 = { (rects2[1].left + rects2[1].right) / 2, (rects2[1].top + rects2[1].bottom) / 2 };
        SendMessageW(wnd.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pt2.x, pt2.y));

        // Before WM_LBUTTONUP, user types 'C' -> candidates change to version 3 ("1. 华", "2. 花", "3. 化")!
        tip.OnKeyDown(&ctxA, 'C', 0, &eaten);
        SendMessageW(wnd.GetHwnd(), WM_PAINT, 0, 0);
        uint32_t ver3 = wnd.GetSnapshot().candidate_version;
        CHECK_TRUE(ver3 > ver2, L"Window updated to version 3 while mouse button held down");

        // Now user releases mouse button (WM_LBUTTONUP) -> MUST NOT commit version 3's candidate!
        SendMessageW(wnd.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(pt2.x, pt2.y));
        CHECK_EQUAL(server.m_commitSuccessCount.load(), successBefore, L"Click where candidates mutated between LButtonDown and LButtonUp MUST be rejected!");

        // Now a fresh press + release on version 3 candidate 1 ("2. 花") succeeds with version 3!
        const auto& rects3 = wnd.GetCandidateRects();
        POINT pt3 = { (rects3[1].left + rects3[1].right) / 2, (rects3[1].top + rects3[1].bottom) / 2 };
        SendMessageW(wnd.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pt3.x, pt3.y));
        SendMessageW(wnd.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(pt3.x, pt3.y));
        CHECK_EQUAL(server.m_commitSuccessCount.load(), successBefore + 1, L"Valid fresh click on current snapshot commits cleanly");
        CHECK_EQUAL(server.m_lastSelectVersion.load(), ver3, L"Mouse click submitted exact displayed snapshot version");
        CHECK_EQUAL(server.m_lastSelectedText, L"花", L"Mouse click committed exact candidate '花'");
    }

    // 3. Cross-context rejection (跨输入框): stale snapshot from ctxA rejected when ctxB is active
    {
        BOOL eaten = FALSE;
        tip.OnKeyDown(&ctxA, 'A', 0, &eaten);
        CandidateDisplaySnapshot staleCtxASnap = wnd.GetSnapshot();
        CHECK_EQUAL(staleCtxASnap.context_id, reinterpret_cast<uint64_t>(&ctxA), L"Captured ctxA snapshot");

        // Switch active input to ctxB
        tip.OnKeyDown(&ctxB, 'B', 0, &eaten);
        CHECK_EQUAL(wnd.GetSnapshot().context_id, reinterpret_cast<uint64_t>(&ctxB), L"Window now shows ctxB snapshot");

        // Attempting to submit ctxA's snapshot while ctxB is active MUST be rejected!
        CHECK_EQUAL(tip.SubmitCandidateSelectionFromSnapshot(staleCtxASnap, 0), false, L"Cross-context snapshot selection MUST be rejected!");
        CHECK_EQUAL(tip.SubmitPageScrollFromSnapshot(staleCtxASnap, true), false, L"Cross-context snapshot page scroll MUST be rejected!");
    }

    // 4. Late async PageUp (`selectLastOnPageUp` / 翻页晚到): only selects last item AFTER new page renders!
    {
        BOOL eaten = FALSE;
        // Start on ctxA with 'A' (Page 0, 5 candidates, page_count = 2)
        tip.OnKeyDown(&ctxA, 'A', 0, &eaten);
        // Flip to Page 1 synchronously via VK_NEXT (Page 1 has 3 candidates: "1. 腌", "2. 锕", "3. 錒", selectedIndex = 0)
        tip.OnKeyDown(&ctxA, VK_NEXT, 0, &eaten);
        CHECK_EQUAL(wnd.GetSnapshot().page_index, 1u, L"On Page 1");
        CHECK_EQUAL(wnd.GetCandidateCount(), 3, L"Page 1 has 3 candidates");
        CHECK_EQUAL(wnd.GetSelectedIndex(), 0, L"Page 1 selectedIndex is 0");

        // Now defer async session on ctxA and press VK_UP at index 0 -> triggers PageUp (VK_PRIOR) with selectLastOnPageUp!
        ctxA.m_forceSyncBlocked = true;
        ctxA.m_deferAsyncSession = true;
        tip.OnKeyDown(&ctxA, VK_UP, 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"VK_UP paging request eaten");

        // CRITICAL P1 ASSERTION: While Page 0 edit session is still queued in TF_S_ASYNC,
        // the old Page 1 window MUST NOT have its selectedIndex mutated to 2!
        CHECK_EQUAL(wnd.GetSnapshot().page_index, 1u, L"Still displaying Page 1 before deferred execution");
        CHECK_EQUAL(wnd.GetSelectedIndex(), 0, L"Old Page 1 selectedIndex MUST remain 0 while PageUp edit session is queued!");

        // Now execute deferred PageUp edit session -> Page 0 (5 candidates) renders with selectedIndex = 4 (last item)!
        ctxA.m_forceSyncBlocked = false;
        ctxA.m_deferAsyncSession = false;
        ctxA.ExecuteDeferredSession();
        CHECK_EQUAL(wnd.GetSnapshot().page_index, 0u, L"Now displaying Page 0 after deferred execution");
        CHECK_EQUAL(wnd.GetCandidateCount(), 5, L"Page 0 has 5 candidates");
        CHECK_EQUAL(wnd.GetSelectedIndex(), 4, L"New Page 0 selectedIndex is now 4 (last item) after async completion!");
    }

    tip.Deactivate();
    server.Stop();
    IpcClient::SetPipeNameForTest(nullptr);
    std::wcout << L"[PASS] Snapshot-bound mouse selection, press-release mutation guard, cross-context & late paging verified!" << std::endl;
    return 0;
}

int TestPluginSyncNonBlockingAndDisabledClickRejection() {
    std::wcout << L"[TEST 18] Testing Plugin Sync Non-Blocking, Fault Resilience & Disabled Click Rejection..." << std::endl;

    const std::wstring testPipeName = L"\\\\.\\pipe\\PersonalIME_MockPipe_T18_" + std::to_wstring(GetCurrentProcessId());
    SnapshotMockIpcServer server(testPipeName);
    server.m_pluginsRevTag.store(5); // Advertise plugins revision tag = 5
    CHECK_TRUE(server.Start(3), L"Snapshot mock server must start");
    IpcClient::SetPipeNameForTest(testPipeName.c_str());

    CTextInputProcessor tip;
    tip.SetClientIdForTest(1);
    MockContext ctx;
    auto& wnd = tip.GetCandidateWindowForTest();

    // 1. Inject plugin query failure/delay during keystroke -> verify composition, commit, and receipt still succeed!
    int pluginQueryCalls = 0;
    bool injectQueryFailure = true;
    std::vector<CandidatePluginItem> serverPlugins = {
        { 10, L"数据库计算器", L"🧮", L"calc.exe", L"" },
        { 20, L"数据库笔记", L"📝", L"notepad.exe", L"" }
    };
    tip.SetPluginQueryOverrideForTest([&](std::vector<CandidatePluginItem>& outPlugins, bool* outTrunc, uint32_t* outRev) -> bool {
        pluginQueryCalls++;
        // Verify that host composition has ALREADY been updated BEFORE plugin sync runs!
        if (ctx.m_activeCompRange.empty() && pluginQueryCalls == 1) {
            return false; // Would fail if plugin sync ran before RequestEditSession!
        }
        if (injectQueryFailure) {
            return false; // Simulate DB busy / plugin query failure
        }
        outPlugins = serverPlugins;
        if (outTrunc) *outTrunc = false;
        if (outRev) *outRev = server.m_pluginsRevTag.load();
        return true;
    });

    BOOL eaten = FALSE;
    tip.OnKeyDown(&ctx, 'A', 0, &eaten);
    CHECK_EQUAL(eaten, TRUE, L"Key 'A' eaten even when plugin query fails");
    CHECK_EQUAL(ctx.m_activeCompRange, L"a", L"Host composition updated to 'a' despite plugin query failure");
    CHECK_EQUAL(wnd.GetCandidateCount(), 5, L"Candidate window displayed 5 candidates despite plugin query failure");
    CHECK_EQUAL(pluginQueryCalls, 1, L"Real plugin sync branch executed without test-pipe bypass");

    // Commit with VK_SPACE while plugin query is still failing -> commit and receipt MUST succeed!
    int commitsBefore = server.m_commitSuccessCount.load();
    tip.OnKeyDown(&ctx, VK_SPACE, 0, &eaten);
    CHECK_EQUAL(eaten, TRUE, L"VK_SPACE commit eaten");
    CHECK_EQUAL(ctx.m_activeCompRange, L"工", L"Committed '工' to host document cleanly");
    CHECK_EQUAL(server.m_commitSuccessCount.load(), commitsBefore + 1, L"CommitSuccess receipt delivered cleanly despite plugin query failure");

    // 2. Now allow plugin query to succeed -> next composition syncs revision 5 plugins into CandidateWindow!
    injectQueryFailure = false;
    tip.OnKeyDown(&ctx, 'A', 0, &eaten);
    CHECK_EQUAL(tip.GetSyncedPluginsRevisionForTest(), 5u, L"Synced plugins revision 5 after recovery");
    CHECK_EQUAL(wnd.GetPlugins().size(), size_t(2), L"CandidateWindow updated to 2 DB plugins");
    CHECK_EQUAL(wnd.GetPlugins()[0].name, L"数据库计算器", L"Plugin 0 is 数据库计算器");

    // 3. Subsequent keystroke with unchanged revision tag (5) uses revision cache without re-querying!
    int callsAfterSync = pluginQueryCalls;
    tip.OnKeyDown(&ctx, 'A', 0, &eaten);
    CHECK_EQUAL(pluginQueryCalls, callsAfterSync, L"Revision cache hit: no redundant GetPlugins call when revision unchanged");

    // 4. Clicking a disabled/deleted plugin button (ExecutePlugin returns false) MUST NOT launch local cached exec_path,
    //    and MUST refresh CandidateWindow plugins from server!
    int execCalls = 0;
    tip.SetPluginExecuteOverrideForTest([&](uint32_t pluginId, uint32_t* outStatus) -> bool {
        execCalls++;
        if (outStatus) *outStatus = 2; // PluginExecStatus::Disabled
        // Server disabled plugin 10, so only plugin 20 remains enabled on server
        serverPlugins = {
            { 20, L"数据库笔记", L"📝", L"notepad.exe", L"" }
        };
        server.m_pluginsRevTag.store(6);
        return false; // Rejected by server!
    });

    wnd.TriggerPluginButton(10);
    CHECK_EQUAL(execCalls, 1, L"ExecutePlugin called on server");
    CHECK_EQUAL(wnd.GetPlugins().size(), size_t(1), L"Rejected plugin click refreshed CandidateWindow plugins from server (removed disabled plugin 10)");
    CHECK_EQUAL(wnd.GetPlugins()[0].id, 20u, L"Remaining plugin is id 20");

    tip.Deactivate();
    server.Stop();
    IpcClient::SetPipeNameForTest(nullptr);
    std::wcout << L"[PASS] Plugin sync non-blocking post-edit, fault resilience, revision cache & disabled click rejection verified!" << std::endl;
    return 0;
}

// Test 19: Stage 7.1b - Compositionless Degradation (UiOnlyHandled), Non-Degradable Reject,
// Direct Key Updates without Per-Key Write Locks, Commit/Cancel/Unfocus Leases & Display Attributes
int TestStage71bCompositionlessAndDisplayAttributes() {
    std::wcout << L"[TEST 19] Testing Stage 7.1b Compositionless Degradation, Leases, Selection Validation & Display Attributes..." << std::endl;

    const std::wstring testPipeName = L"\\\\.\\pipe\\PersonalIME_MockPipe_T19_" + std::to_wstring(GetCurrentProcessId());
    SnapshotMockIpcServer server(testPipeName);
    CHECK_TRUE(server.Start(3), L"Snapshot mock server must start");
    IpcClient::SetPipeNameForTest(testPipeName.c_str());

    CTextInputProcessor tip;
    tip.SetClientIdForTest(1);

    // 1. Verify ITfDisplayAttributeProvider implementation
    {
        ITfDisplayAttributeProvider* pProvider = nullptr;
        HRESULT hrQi = tip.QueryInterface(IID_ITfDisplayAttributeProvider, reinterpret_cast<void**>(&pProvider));
        CHECK_EQUAL(hrQi, S_OK, L"QueryInterface(IID_ITfDisplayAttributeProvider) must succeed");
        CHECK_TRUE(pProvider != nullptr, L"pProvider non-null");

        ITfDisplayAttributeInfo* pInfo = nullptr;
        HRESULT hrGet = pProvider->GetDisplayAttributeInfo(GUID_PersonalIME_DisplayAttributeInput, &pInfo);
        CHECK_EQUAL(hrGet, S_OK, L"GetDisplayAttributeInfo for GUID_PersonalIME_DisplayAttributeInput must succeed");
        CHECK_TRUE(pInfo != nullptr, L"pInfo non-null");

        TF_DISPLAYATTRIBUTE attr{};
        CHECK_EQUAL(pInfo->GetAttributeInfo(&attr), S_OK, L"GetAttributeInfo must succeed");
        CHECK_EQUAL(attr.lsStyle, TF_LS_DOT, L"Display attribute underline style must be TF_LS_DOT");
        CHECK_EQUAL(attr.bAttr, TF_ATTR_INPUT, L"Display attribute bAttr must be TF_ATTR_INPUT");
        pInfo->Release();

        IEnumTfDisplayAttributeInfo* pEnum = nullptr;
        CHECK_EQUAL(pProvider->EnumDisplayAttributeInfo(&pEnum), S_OK, L"EnumDisplayAttributeInfo must succeed");
        ITfDisplayAttributeInfo* pEnumItem = nullptr;
        ULONG fetched = 0;
        CHECK_EQUAL(pEnum->Next(1, &pEnumItem, &fetched), S_OK, L"Enum Next(1) must return S_OK");
        CHECK_EQUAL(fetched, 1u, L"Fetched 1 display attribute item");
        pEnumItem->Release();
        pEnum->Release();
        pProvider->Release();
    }

    // 2. Non-degradable failures (TS_SD_READONLY, TF_E_DISCONNECTED, E_OUTOFMEMORY, TF_E_NOLOCK)
    //    must return Unmodified and rollback server, NEVER entering CompositionlessComposing!
    {
        MockContext roCtx;
        roCtx.m_dynamicStatusFlags = TS_SD_READONLY;
        roCtx.m_failStartComposition = true;
        roCtx.m_startCompositionHr = E_FAIL;

        BOOL eaten = TRUE;
        tip.OnKeyDown(&roCtx, 'A', 0, &eaten);
        CHECK_EQUAL(eaten, FALSE, L"Readonly context must NOT eat key or degrade to CompositionlessComposing");
        CHECK_TRUE(tip.GetCompositionState(&roCtx) == CompositionState::None, L"Readonly context stays in CompositionState::None");

        MockContext discCtx;
        discCtx.m_failStartComposition = true;
        discCtx.m_startCompositionHr = TF_E_DISCONNECTED;
        eaten = TRUE;
        tip.OnKeyDown(&discCtx, 'A', 0, &eaten);
        CHECK_EQUAL(eaten, FALSE, L"TF_E_DISCONNECTED must NOT degrade to CompositionlessComposing");
        CHECK_TRUE(tip.GetCompositionState(&discCtx) == CompositionState::None, L"Disconnected context stays in CompositionState::None");
    }

    // 3. Eligible StartComposition failure degrades to CompositionlessComposing (UiOnlyHandled):
    //    - Key is eaten (*pfEaten = TRUE), server is NOT rolled back
    //    - Strong COM reference lease is held on ITfContext
    //    - Subsequent coding keys, Backspace, paging update UI directly WITHOUT requesting per-key write locks!
    //    - Preedit Header is shown with raw_input and pure preview_text
    {
        auto* ctx = new MockContext();
        LONG initialRef = ctx->m_cRef; // 1
        ctx->m_failStartComposition = true;
        ctx->m_startCompositionHr = E_FAIL;

        CCandidateWindow& wnd = tip.GetCandidateWindowForTest();
        BOOL eaten = FALSE;
        tip.OnKeyDown(ctx, 'A', 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"StartComposition failure in eligible context degrades to UiOnlyHandled and eats key 'A'");
        CHECK_TRUE(tip.GetCompositionState(ctx) == CompositionState::CompositionlessComposing, L"Context state is CompositionlessComposing");
        CHECK_TRUE(tip.GetComposition(ctx) == nullptr, L"m_pComposition is nullptr in CompositionlessComposing");
        CHECK_TRUE(ctx->m_cRef > initialRef, L"TIP holds strong COM reference lease on ITfContext while in CompositionlessComposing");
        CHECK_EQUAL(IsWindowVisible(wnd.GetHwnd()) != FALSE, true, L"Candidate window is visible in Compositionless mode");
        CHECK_TRUE(wnd.GetSnapshot().mode == CandidateWindowMode::Compositionless, L"Snapshot mode is Compositionless");
        CHECK_EQUAL(wnd.IsPreeditHeaderVisible(), true, L"Preedit Header is visible in Compositionless mode");
        CHECK_EQUAL(wnd.GetDisplayedPreeditText(), std::wstring(L"a [工]"), L"Preedit Header displays 'a [工]'");

        int editSessionsAfterFirstKey = ctx->m_editSessionRequestCount;
        CHECK_EQUAL(editSessionsAfterFirstKey, 1, L"First key requested 1 EditSession before degrading");

        // Second coding key 'B' in CompositionlessComposing updates UI directly via UiOnlyHandled WITHOUT RequestEditSession!
        tip.OnKeyDown(ctx, 'B', 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"Second key 'B' eaten in CompositionlessComposing");
        CHECK_EQUAL(ctx->m_editSessionRequestCount, editSessionsAfterFirstKey, L"Subsequent keystroke in CompositionlessComposing does NOT request TSF write lock");
        CHECK_EQUAL(wnd.GetDisplayedPreeditText(), std::wstring(L"ab [节]"), L"Preedit Header updated to 'ab [节]'");

        // Backspace in CompositionlessComposing returns to 'a' ("a [工]") without RequestEditSession
        tip.OnKeyDown(ctx, VK_BACK, 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"VK_BACK eaten in CompositionlessComposing");
        CHECK_EQUAL(ctx->m_editSessionRequestCount, editSessionsAfterFirstKey, L"VK_BACK in CompositionlessComposing does NOT request TSF write lock");
        CHECK_EQUAL(wnd.GetDisplayedPreeditText(), std::wstring(L"a [工]"), L"Preedit Header updated back to 'a [工]'");

        // Paging (VK_NEXT) in CompositionlessComposing flips to Page 1 without RequestEditSession
        tip.OnKeyDown(ctx, VK_NEXT, 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"VK_NEXT eaten in CompositionlessComposing");
        CHECK_EQUAL(ctx->m_editSessionRequestCount, editSessionsAfterFirstKey, L"VK_NEXT in CompositionlessComposing does NOT request TSF write lock");
        CHECK_EQUAL(wnd.GetSnapshot().page_index, 1u, L"Snapshot page_index updated to 1");
        CHECK_EQUAL(wnd.GetDisplayedPreeditText(), std::wstring(L"a [腌]"), L"Preedit Header updated to Page 1 first candidate 'a [腌]'");

        // Mouse click selection in CompositionlessComposing passes IsDisplayedSnapshotValid and commits to host selection!
        int commitsBefore = server.m_commitSuccessCount.load();
        const auto& rects = wnd.GetCandidateRects();
        POINT ptCand1 = { (rects[1].left + rects[1].right) / 2, (rects[1].top + rects[1].bottom) / 2 };
        SendMessageW(wnd.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ptCand1.x, ptCand1.y));
        SendMessageW(wnd.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(ptCand1.x, ptCand1.y));

        CHECK_EQUAL(server.m_commitSuccessCount.load(), commitsBefore + 1, L"Mouse click in CompositionlessComposing committed and confirmed CommitSuccess!");
        CHECK_EQUAL(ctx->m_activeCompRange, L"锕", L"Host selection range written with committed text '锕'");
        CHECK_TRUE(tip.GetCompositionState(ctx) == CompositionState::None, L"Context exited CompositionlessComposing to Idle (None) after commit");
        CHECK_EQUAL(ctx->m_cRef, initialRef, L"Strong COM reference lease on ITfContext released upon exiting CompositionlessComposing");
        CHECK_EQUAL(IsWindowVisible(wnd.GetHwnd()) != FALSE, false, L"Candidate window hidden after commit");

        // 4. Compositionless commit where SetSelection fails after SetText succeeds ->
        //    PartiallyModified (CommitSuccess confirmed once), does NOT enter CommitPendingEnd or fabricate composition!
        tip.OnKeyDown(ctx, 'A', 0, &eaten);
        CHECK_TRUE(tip.GetCompositionState(ctx) == CompositionState::CompositionlessComposing, L"Re-entered CompositionlessComposing");
        g_failSetSelection = true;
        commitsBefore = server.m_commitSuccessCount.load();
        tip.OnKeyDown(ctx, VK_SPACE, 0, &eaten);
        g_failSetSelection = false;
        CHECK_EQUAL(eaten, TRUE, L"VK_SPACE commit eaten");
        CHECK_EQUAL(ctx->m_activeCompRange, L"工", L"Host text written to '工'");
        CHECK_EQUAL(server.m_commitSuccessCount.load(), commitsBefore + 1, L"PartiallyModified in Compositionless commit still confirms CommitSuccess");
        CHECK_TRUE(tip.GetCompositionState(ctx) == CompositionState::None, L"Compositionless SetSelection failure must NOT enter CommitPendingEnd");
        CHECK_TRUE(tip.GetComposition(ctx) == nullptr, L"No composition object fabricated");

        // 5. Esc and focus loss (OnSetFocus(FALSE)) in CompositionlessComposing invalidate snapshot,
        //    advance epoch/generation, release COM lease, and reset server without touching host text!
        ctx->m_activeCompRange = L"untouched_host_text";
        tip.OnKeyDown(ctx, 'A', 0, &eaten);
        CandidateDisplaySnapshot snapBeforeEsc = wnd.GetSnapshot();
        CHECK_TRUE(tip.IsDisplayedSnapshotValid(snapBeforeEsc), L"Snapshot valid while in CompositionlessComposing");

        tip.OnKeyDown(ctx, VK_ESCAPE, 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"VK_ESCAPE eaten in CompositionlessComposing");
        CHECK_EQUAL(ctx->m_activeCompRange, L"untouched_host_text", L"VK_ESCAPE in CompositionlessComposing does not touch host text");
        CHECK_TRUE(tip.GetCompositionState(ctx) == CompositionState::None, L"State returned to None after Esc");
        CHECK_EQUAL(ctx->m_cRef, initialRef, L"COM reference lease released on Esc");
        CHECK_EQUAL(tip.IsDisplayedSnapshotValid(snapBeforeEsc), false, L"Old snapshot invalidated after Esc");

        // Re-enter and test OnSetFocus(FALSE)
        tip.OnKeyDown(ctx, 'A', 0, &eaten);
        CandidateDisplaySnapshot snapBeforeUnfocus = wnd.GetSnapshot();
        int resetsBefore = server.m_resetCount.load();
        tip.OnSetFocus(FALSE);
        CHECK_TRUE(tip.GetCompositionState(ctx) == CompositionState::None, L"State returned to None on OnSetFocus(FALSE)");
        CHECK_EQUAL(ctx->m_cRef, initialRef, L"COM reference lease released on OnSetFocus(FALSE)");
        CHECK_EQUAL(tip.IsDisplayedSnapshotValid(snapBeforeUnfocus), false, L"Old snapshot invalidated on OnSetFocus(FALSE)");
        CHECK_TRUE(server.m_resetCount.load() > resetsBefore, L"Server context reset on OnSetFocus(FALSE)");

        ctx->Release();
    }

    tip.Deactivate();
    server.Stop();
    IpcClient::SetPipeNameForTest(nullptr);
    std::wcout << L"[PASS] Stage 7.1b Compositionless degradation, leases, selection validation & DisplayAttributes verified!" << std::endl;
    return 0;
}

// Test 20: Stage 7.1b Review Fixes:
// 1. Async compositionless commit -> unfocus/caret change -> deferred DoEditSession -> Unmodified + no write + CommitCancel
// 2. Stage 7 capability negotiation & segmented query_code delivery + legacy fallback
// 3. Collapse failure on GetSelection & InsertTextAtSelection -> PartiallyModified with exact HRESULT
int TestStage71bReviewFixes() {
    std::wcout << L"[TEST 20] Testing Stage 7.1b Review Fixes (Async Compositionless Unfocus Guard, Stage 7 QueryCode Negotiation, Collapse Failure)..." << std::endl;

    const std::wstring testPipeName = L"\\\\.\\pipe\\PersonalIME_MockPipe_T20_" + std::to_wstring(GetCurrentProcessId());
    SnapshotMockIpcServer server(testPipeName);
    server.m_advertiseStage7.store(true);
    CHECK_TRUE(server.Start(3), L"Snapshot mock server must start");
    IpcClient::SetPipeNameForTest(testPipeName.c_str());

    // 1. Stage 7 capability negotiation -> KeyDown uses IPC_PROTOCOL_VERSION_STAGE7 and delivers query_code "da'jia"
    {
        CTextInputProcessor tip;
        tip.SetClientIdForTest(1);
        CHECK_TRUE(tip.NegotiateStage7ForTest(), L"NegotiateStage7ForTest must succeed when server advertises Stage 7");
        MockContext ctx;
        ctx.m_failStartComposition = true;
        ctx.m_startCompositionHr = E_FAIL;

        BOOL eaten = FALSE;
        tip.OnKeyDown(&ctx, 'A', 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"Key 'A' eaten");
        CHECK_EQUAL(server.m_lastKeyDownProtocol.load(), IPC_PROTOCOL_VERSION_STAGE7, L"KeyDown negotiated IPC_PROTOCOL_VERSION_STAGE7");
        CHECK_EQUAL(tip.GetCandidateWindowForTest().GetSnapshot().query_code, std::wstring(L"da'jia"), L"Snapshot carries segmented query_code from Stage 7 server");
        CHECK_EQUAL(tip.GetCandidateWindowForTest().GetDisplayedPreeditText(), std::wstring(L"da'jia [工]"), L"Header renders segmented query_code + preview");

        // 2. Async compositionless commit -> OnSetFocus(FALSE) -> deferred DoEditSession executes:
        //    MUST NOT write into old context's GetSelection, MUST return Unmodified & send CommitCancel!
        ctx.m_activeCompRange = L"new_caret_untouched";
        ctx.m_forceSyncBlocked = true;
        ctx.m_deferAsyncSession = true;

        int successBefore = server.m_commitSuccessCount.load();
        int cancelBefore = server.m_commitCancelCount.load();
        int learnedBefore = server.m_learnedCount.load();

        tip.OnKeyDown(&ctx, VK_SPACE, 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"Async compositionless commit queued");
        CHECK_EQUAL(tip.GetPendingEditSessionCount(), 1, L"1 queued EditSession");

        // Focus lost before deferred EditSession runs!
        tip.OnSetFocus(FALSE);
        CHECK_EQUAL(ctx.m_activeCompRange, std::wstring(L"new_caret_untouched"), L"Host untouched on unfocus");

        // Now deferred EditSession executes on old context
        ctx.m_forceSyncBlocked = false;
        ctx.m_deferAsyncSession = false;
        ctx.ExecuteDeferredSession();

        CHECK_EQUAL(ctx.m_activeCompRange, std::wstring(L"new_caret_untouched"), L"Deferred compositionless commit after unfocus MUST NOT write to host GetSelection!");
        CHECK_EQUAL(server.m_commitSuccessCount.load(), successBefore, L"Deferred compositionless commit after unfocus MUST NOT confirm CommitSuccess!");
        CHECK_EQUAL(server.m_learnedCount.load(), learnedBefore, L"Deferred compositionless commit after unfocus MUST NOT learn!");
        CHECK_EQUAL(server.m_commitCancelCount.load(), cancelBefore + 1, L"Deferred compositionless commit after unfocus MUST send CommitCancel!");

        tip.Deactivate();
    }

    // 3. Legacy server fallback: if server rejects IPC_PROTOCOL_VERSION_STAGE7 KeyDown, client falls back to STAGE3
    {
        server.m_rejectStage7KeyDown.store(true);
        CTextInputProcessor tip;
        tip.SetClientIdForTest(1);
        tip.NegotiateStage7ForTest();
        MockContext ctx;
        ctx.m_failStartComposition = true;

        BOOL eaten = FALSE;
        tip.OnKeyDown(&ctx, 'A', 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"Legacy fallback KeyDown succeeded and ate key");
        CHECK_EQUAL(server.m_lastKeyDownProtocol.load(), IPC_PROTOCOL_VERSION_STAGE3, L"Client fell back to IPC_PROTOCOL_VERSION_STAGE3");
        CHECK_EQUAL(tip.GetCandidateWindowForTest().GetSnapshot().query_code, std::wstring(L""), L"Legacy server returns no query_code");
        CHECK_EQUAL(tip.GetCandidateWindowForTest().GetDisplayedPreeditText(), std::wstring(L"a [工]"), L"Header falls back to raw_input 'a [工]'");

        tip.Deactivate();
        server.m_rejectStage7KeyDown.store(false);
    }

    // 4. Collapse failure on GetSelection + SetText and ITfInsertAtSelection paths -> PartiallyModified with exact HRESULT
    {
        CTextInputProcessor tip;
        tip.SetClientIdForTest(1);
        MockContext ctx;
        ctx.m_failStartComposition = true;

        g_failCollapse = true;
        g_collapseFailHr = E_ABORT;

        // 4a. GetSelection + SetText + Collapse failure
        {
            CEditSession session(&tip, &ctx, nullptr, false, L"", true, L"大家", {});
            HRESULT hr = session.DoEditSession(1);
            CHECK_EQUAL(hr, E_ABORT, L"GetSelection Collapse failure preserves exact HRESULT E_ABORT");
            CHECK_TRUE(session.GetResult() == EditSessionResult::PartiallyModified, L"GetSelection Collapse failure must return PartiallyModified");
        }

        // 4b. ITfInsertAtSelection + Collapse failure
        {
            ctx.m_failGetSelection = true;
            CEditSession session(&tip, &ctx, nullptr, false, L"", true, L"测试", {});
            HRESULT hr = session.DoEditSession(1);
            ctx.m_failGetSelection = false;
            CHECK_EQUAL(hr, E_ABORT, L"InsertTextAtSelection Collapse failure preserves exact HRESULT E_ABORT");
            CHECK_TRUE(session.GetResult() == EditSessionResult::PartiallyModified, L"InsertTextAtSelection Collapse failure must return PartiallyModified");
        }

        g_failCollapse = false;
        tip.Deactivate();
    }

    // 5. Same-context caret move while keeping focus (ITfTextEditSink::OnEndEdit):
    //    Async compositionless commit queued -> external selection move in same context (focus retained) ->
    //    deferred DoEditSession MUST NOT write to new selection and MUST send CommitCancel!
    {
        CTextInputProcessor tip;
        tip.SetClientIdForTest(1);
        tip.NegotiateStage7ForTest();
        MockContext ctx;
        ctx.m_failStartComposition = true;

        BOOL eaten = FALSE;
        tip.OnKeyDown(&ctx, 'A', 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"Key 'A' eaten in CompositionlessComposing");
        CHECK_TRUE(ctx.m_pTextEditSink != nullptr, L"ITfTextEditSink advised on CompositionlessComposing");

        ctx.m_activeCompRange = L"moved_caret_untouched";
        ctx.m_forceSyncBlocked = true;
        ctx.m_deferAsyncSession = true;

        int successBefore = server.m_commitSuccessCount.load();
        int cancelBefore = server.m_commitCancelCount.load();
        int learnedBefore = server.m_learnedCount.load();

        tip.OnKeyDown(&ctx, VK_SPACE, 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"Async compositionless commit queued");

        // Keep focus, stay in same context, only move caret / selection!
        ctx.SimulateExternalSelectionMove();
        CHECK_TRUE(tip.GetCompositionState(&ctx) == CompositionState::None, L"Selection move exited CompositionlessComposing");

        ctx.m_forceSyncBlocked = false;
        ctx.m_deferAsyncSession = false;
        ctx.ExecuteDeferredSession();

        CHECK_EQUAL(ctx.m_activeCompRange, std::wstring(L"moved_caret_untouched"), L"Same-context caret move must prevent deferred compositionless write!");
        CHECK_EQUAL(server.m_commitSuccessCount.load(), successBefore, L"Same-context caret move must NOT confirm CommitSuccess!");
        CHECK_EQUAL(server.m_learnedCount.load(), learnedBefore, L"Same-context caret move must NOT learn!");
        CHECK_EQUAL(server.m_commitCancelCount.load(), cancelBefore + 1, L"Same-context caret move must send CommitCancel!");

        tip.Deactivate();
        CHECK_TRUE(ctx.m_pTextEditSink == nullptr, L"ITfTextEditSink cleanly unadvised on Deactivate");
    }

    // 6. Server-supplied candidate_core_texts (IPC_STATUS_HAS_CAND_CORES) preserves "版本 [beta]" AND long 64-char + " [beta]"
    {
        CTextInputProcessor tip;
        tip.SetClientIdForTest(1);
        CHECK_TRUE(tip.NegotiateStage7ForTest(), L"Negotiate Stage 7 for core text test");
        MockContext ctx;
        ctx.m_failStartComposition = true;

        BOOL eaten = FALSE;
        tip.OnKeyDown(&ctx, 'V', 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"Key 'V' eaten");
        const auto& snap = tip.GetCandidateWindowForTest().GetSnapshot();
        CHECK_EQUAL(snap.candidate_core_texts.size(), size_t(2), L"2 explicit candidate_core_texts received from Stage 7 server");
        CHECK_EQUAL(snap.candidate_core_texts[0], std::wstring(L"版本 [beta]"), L"Core text 0 preserves '版本 [beta]'");
        CHECK_EQUAL(tip.GetCandidateWindowForTest().GetDisplayedPreeditText(), std::wstring(L"da'jia [版本 [beta]]"), L"Preedit Header preserves '版本 [beta]' intact without stripping [beta]!");

        tip.OnKeyDown(&ctx, 'L', 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"Key 'L' eaten for mixed 123-unit + '版本 [beta]' page");
        std::wstring expectedLongCore(116, L'汉');
        expectedLongCore += L" [beta]"; // 123 UTF-16 units
        const auto& snapLong = tip.GetCandidateWindowForTest().GetSnapshot();
        CHECK_EQUAL(snapLong.candidate_core_texts.size(), size_t(2), L"Mixed page core_texts received");
        CHECK_EQUAL(snapLong.candidate_core_texts[0], expectedLongCore, L"123-unit candidate preserves [beta] without guessing");
        CHECK_EQUAL(snapLong.candidate_core_texts[1], std::wstring(L"版本 [beta]"), L"Same-page '版本 [beta]' still keeps exact boundary!");
        CHECK_EQUAL(tip.GetCandidateWindowForTest().GetDisplayedPreeditText(), L"da'jia [" + expectedLongCore + L"]", L"Preedit Header preserves 123-unit candidate");
        tip.GetCandidateWindowForTest().SetSelectedIndex(1);
        CHECK_EQUAL(tip.GetCandidateWindowForTest().GetDisplayedPreeditText(), std::wstring(L"da'jia [版本 [beta]]"), L"Selecting '版本 [beta]' on same page preserves [beta] intact!");

        tip.Deactivate();
    }

    // 7. AdviseSink failure blocks queuing an unguarded compositionless async commit
    {
        CTextInputProcessor tip;
        tip.SetClientIdForTest(1);
        MockContext ctx;
        ctx.m_failStartComposition = true;
        ctx.m_failAdviseSink = true;

        BOOL eaten = FALSE;
        tip.OnKeyDown(&ctx, 'A', 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"Entered CompositionlessComposing even when AdviseSink fails");
        CHECK_TRUE(ctx.m_pTextEditSink == nullptr, L"AdviseSink failed as injected");

        ctx.m_forceSyncBlocked = true;
        ctx.m_deferAsyncSession = true;
        int cancelBefore = server.m_commitCancelCount.load();
        tip.OnKeyDown(&ctx, VK_SPACE, 0, &eaten);
        CHECK_EQUAL(eaten, FALSE, L"Unguarded compositionless async commit MUST be rejected when AdviseSink failed!");
        CHECK_EQUAL(tip.GetPendingEditSessionCount(), 0, L"No async EditSession queued when AdviseSink failed");
        CHECK_EQUAL(server.m_commitCancelCount.load(), cancelBefore + 1, L"Rejected unguarded async commit sent CommitCancel");

        tip.Deactivate();
    }

    // 8. Real TSF OnEndEdit timing (fired after DoEditSession returns while InWriteSession == TRUE)
    //    must NOT falsely cancel our own edit/composition!
    {
        CTextInputProcessor tip;
        tip.SetClientIdForTest(1);
        MockContext ctx;
        ctx.m_failStartComposition = true;
        ctx.m_fireOnEndEditAfterDoEditSession = true;

        BOOL eaten = FALSE;
        tip.OnKeyDown(&ctx, 'A', 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"Key 'A' eaten");
        CHECK_TRUE(tip.GetCompositionState(&ctx) == CompositionState::CompositionlessComposing, L"Remains in CompositionlessComposing");

        // Trigger a synchronous commit; OnEndEdit fires after DoEditSession returns while InWriteSession == TRUE
        int successBefore = server.m_commitSuccessCount.load();
        tip.OnKeyDown(&ctx, VK_SPACE, 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"Commit succeeded despite OnEndEdit firing after DoEditSession");
        CHECK_EQUAL(server.m_commitSuccessCount.load(), successBefore + 1, L"CommitSuccess confirmed");

        tip.Deactivate();
    }

    server.Stop();
    IpcClient::SetPipeNameForTest(nullptr);
    std::wcout << L"[PASS] Stage 7.1b Review Fixes verified!" << std::endl;
    return 0;
}

// Test 21: Stage 7.1c - Request Control Block Fence, Observation Timeout -> UnknownPending -> Quarantined,
// Late Callback Arbitration, v2 WAL Conflict/Corrupt Tail Resilience, and Startup/Crash Recovery
int TestStage71cRequestFenceAndWalRecovery() {
    std::wcout << L"[TEST 21] Testing Stage 7.1c Request Fence, UnknownPending/Quarantine & WAL v2 Recovery..." << std::endl;

    auto cleanupWals = []() {
        std::error_code ec;
        std::filesystem::remove(receipt_journal_v2::Path(), ec);
        std::filesystem::remove(receipt_journal::Directory() / L"PersonalIME_receipts.wal", ec);
    };
    cleanupWals();

    const std::wstring testPipeName = L"\\\\.\\pipe\\PersonalIME_MockPipe_T21_" + std::to_wstring(GetCurrentProcessId());
    SnapshotMockIpcServer server(testPipeName);
    server.m_advertiseStage7.store(true);
    CHECK_TRUE(server.Start(3), L"Stage 7.1c mock server must start");
    IpcClient::SetPipeNameForTest(testPipeName.c_str());

    // 1. Pre-side-effect revocation on EditRequestControlBlock:
    //    Revoking a queued async commit before DoEditSession executes must abort with Unmodified,
    //    transition controlBlock->state to RevokedBeforeWrite, perform ZERO host writes,
    //    and persist V2_PREP -> V2_RES(CancelDiscard) -> V2_ACK.
    {
        CTextInputProcessor tip;
        tip.SetClientIdForTest(1);
        CHECK_TRUE(tip.NegotiateStage7ForTest(), L"NegotiateStage7 must succeed");

        MockContext ctx;
        BOOL eaten = FALSE;
        tip.OnKeyDown(&ctx, 'A', 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"Key 'A' eaten");

        ctx.m_forceSyncBlocked = true;
        ctx.m_deferAsyncSession = true;
        std::wstring compBefore = ctx.m_activeCompRange;
        tip.OnKeyDown(&ctx, VK_SPACE, 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"Async commit queued");
        CHECK_TRUE(ctx.m_pDeferredSession != nullptr, L"Deferred EditSession captured");

        auto cb = tip.GetRequestControlBlockForTest(2); // req 1 = 'A', req 2 = VK_SPACE commit
        CHECK_TRUE(cb != nullptr, L"EditRequestControlBlock exists for queued commit");
        CHECK_TRUE(cb->wal_prepared, L"V2_PREP persisted before queuing commit");
        CHECK_TRUE(cb->state == RequestLifecycleState::Queued, L"Control block starts in Queued");

        // Revoke before DoEditSession runs (e.g. focus loss / context switch)
        tip.SuppressQueuedEditSessionsForContext(reinterpret_cast<uint64_t>(&ctx), true);
        CHECK_TRUE(cb->revoked, L"Control block marked revoked");
        CHECK_TRUE(!cb->crossed_side_effect, L"Control block has NOT crossed side effect");

        // Now host runs the late deferred EditSession
        HRESULT hrLate = ctx.m_pDeferredSession->DoEditSession(1);
        CHECK_EQUAL(hrLate, E_ABORT, L"Revoked pre-side-effect EditSession must abort with E_ABORT");
        CHECK_TRUE(cb->state == RequestLifecycleState::RevokedBeforeWrite, L"Control block transitioned to RevokedBeforeWrite");
        CHECK_TRUE(!cb->crossed_side_effect, L"crossed_side_effect remains false");
        CHECK_EQUAL(ctx.m_activeCompRange, compBefore, L"ZERO host SetText writes occurred after revocation!");
        CHECK_TRUE(cb->wal_resolved && cb->wal_acked, L"V2_RES and V2_ACK persisted for CancelDiscard");
        CHECK_TRUE(server.m_terminalDispositions[cb->commit_event_id] == IpcCommitDisposition::CancelDiscard,
                   L"Server recorded CancelDiscard");

        ctx.m_pDeferredSession->Release();
        ctx.m_pDeferredSession = nullptr;
        tip.Deactivate();
    }

    cleanupWals();

    // 2. Observation timeout -> UnknownPending (V2_UNK) -> Quarantined -> key passthrough ->
    //    independent second context stays usable -> late Success resolves & retires Quarantined!
    {
        CTextInputProcessor tip;
        tip.SetClientIdForTest(1);
        CHECK_TRUE(tip.NegotiateStage7ForTest(), L"NegotiateStage7 must succeed");

        MockContext ctx1;
        MockContext ctx2;
        BOOL eaten = FALSE;
        tip.OnKeyDown(&ctx1, 'A', 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"ctx1 'A' eaten");

        ctx1.m_forceSyncBlocked = true;
        ctx1.m_deferAsyncSession = true;
        tip.OnKeyDown(&ctx1, VK_SPACE, 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"ctx1 commit queued async");

        auto cb = tip.GetRequestControlBlockForTest(2);
        CHECK_TRUE(cb != nullptr, L"ctx1 commit control block exists");
        uint64_t ctx1Id = reinterpret_cast<uint64_t>(&ctx1);

        CHECK_TRUE(tip.MarkObservationTimeout(cb->edit_request_id), L"MarkObservationTimeout succeeded");
        CHECK_TRUE(tip.GetContextState(ctx1Id) == CompositionState::Quarantined, L"ctx1 transitioned to Quarantined");

        // Verify V2_UNK persisted and NOT ClosedUnknown
        {
            receipt_journal_v2::Snapshot snap;
            CHECK_TRUE(receipt_journal_v2::LoadSnapshot(snap, false), L"LoadSnapshot v2 succeeded");
            auto itEv = snap.events.find(cb->commit_event_id);
            CHECK_TRUE(itEv != snap.events.end(), L"Event found in v2 WAL");
            CHECK_TRUE(itEv->second.has_unknown_pending, L"V2_UNK recorded in v2 WAL");
            CHECK_TRUE(!itEv->second.has_resolved, L"Timeout alone MUST NOT force ClosedUnknown / Resolved!");
        }

        // Quarantined ctx1 passes keys through without eating
        eaten = TRUE;
        tip.OnTestKeyDown(&ctx1, 'B', 0, &eaten);
        CHECK_EQUAL(eaten, FALSE, L"Quarantined ctx1 OnTestKeyDown passes key through");
        eaten = TRUE;
        tip.OnKeyDown(&ctx1, 'B', 0, &eaten);
        CHECK_EQUAL(eaten, FALSE, L"Quarantined ctx1 OnKeyDown passes key through");

        // After timeout + key passthrough, the unstarted queued EditSession MUST be revoked and
        // aborted with ZERO host writes (cannot overwrite text after keys were passed through!).
        std::wstring compBeforeLate = ctx1.m_activeCompRange;
        HRESULT hrLate = ctx1.m_pDeferredSession->DoEditSession(1);
        CHECK_EQUAL(hrLate, E_ABORT, L"Timed-out unstarted EditSession must abort with E_ABORT after keys passed through");
        CHECK_TRUE(cb->state == RequestLifecycleState::RevokedBeforeWrite, L"Control block is RevokedBeforeWrite");
        CHECK_TRUE(!cb->crossed_side_effect, L"Timed-out request never crossed side effect");
        CHECK_EQUAL(ctx1.m_activeCompRange, compBeforeLate, L"Host text untouched by timed-out request");
        CHECK_TRUE(cb->wal_resolved && cb->wal_acked, L"CancelDiscard persisted V2_RES and V2_ACK");
        CHECK_TRUE(server.m_terminalDispositions[cb->commit_event_id] == IpcCommitDisposition::CancelDiscard,
                   L"Server recorded CancelDiscard for timed-out revoked request");
        CHECK_TRUE(tip.GetContextState(ctx1Id) == CompositionState::CommitPendingEnd,
                   L"ctx1 retired from Quarantined -> CommitPendingEnd after timed-out request aborted (inline composition still open)");

        ctx1.m_pDeferredSession->Release();
        ctx1.m_pDeferredSession = nullptr;
        tip.Deactivate();
    }

    cleanupWals();

    // 2b. Live cross-instance recovery MUST NOT falsely close another live TIP's pending event as ClosedUnknown;
    //     and runtime v2 receipt retry must recover from transient CommitTerminal / _commit failure without rewriting text.
    {
        CTextInputProcessor liveTip1;
        liveTip1.SetClientIdForTest(1);
        CHECK_TRUE(liveTip1.NegotiateStage7ForTest(), L"NegotiateStage7 for liveTip1");

        MockContext ctxLive;
        BOOL eaten = FALSE;
        liveTip1.OnKeyDown(&ctxLive, 'A', 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"liveTip1 'A' eaten");

        ctxLive.m_forceSyncBlocked = true;
        ctxLive.m_deferAsyncSession = true;
        liveTip1.OnKeyDown(&ctxLive, VK_SPACE, 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"liveTip1 async commit queued");
        auto cbLive = liveTip1.GetRequestControlBlockForTest(2);
        CHECK_TRUE(cbLive != nullptr && cbLive->wal_prepared, L"liveTip1 V2_PREP persisted");

        // Another live TIP instance runs RecoverReceiptsFromWal while liveTip1 is still alive!
        CTextInputProcessor tipOther;
        tipOther.SetClientIdForTest(2);
        CHECK_TRUE(tipOther.NegotiateStage7ForTest(), L"NegotiateStage7 for tipOther");
        tipOther.RecoverReceiptsFromWal();
        CHECK_TRUE(server.m_terminalDispositions.find(cbLive->commit_event_id) == server.m_terminalDispositions.end(),
                   L"RecoverReceiptsFromWal MUST NOT mark another live TIP instance's event as ClosedUnknown!");

        // Inject transient CommitTerminal failure + _commit failure for V2_ACK, then run liveTip1's deferred EditSession
        server.m_dropNextCommitTerminal.store(1);
        HRESULT hrOk = ctxLive.m_pDeferredSession->DoEditSession(1);
        CHECK_EQUAL(hrOk, S_OK, L"liveTip1 commit succeeded on host");
        CHECK_TRUE(cbLive->wal_resolved && !cbLive->wal_acked, L"V2_RES persisted, V2_ACK pending due to CommitTerminal failure");
        CHECK_EQUAL(liveTip1.GetPendingV2ReceiptCountForTest(), size_t(1), L"Failed V2 receipt queued for runtime retry");

        // Inject 1 _commit failure on V2_ACK retry, then succeed on second DrainUnconfirmedCommits
        receipt_journal_v2::FailCommitInjectCountForTest() = 1;
        liveTip1.DrainUnconfirmedCommits(true);
        CHECK_EQUAL(liveTip1.GetPendingV2ReceiptCountForTest(), size_t(1), L"V2_ACK _commit failure retains item in retry queue");
        liveTip1.DrainUnconfirmedCommits(true);
        CHECK_EQUAL(liveTip1.GetPendingV2ReceiptCountForTest(), size_t(0), L"Runtime retry persisted V2_ACK and drained queue");
        CHECK_TRUE(server.m_terminalDispositions[cbLive->commit_event_id] == IpcCommitDisposition::ConfirmedWritten,
                   L"liveTip1 settled ConfirmedWritten without conflict");

        ctxLive.m_pDeferredSession->Release();
        ctxLive.m_pDeferredSession = nullptr;
        tipOther.Deactivate();
        liveTip1.Deactivate();
    }

    cleanupWals();

    // 2c. Server commit generation divergence (after AdvanceContextInputGeneration/Reset) +
    //     initial V2_RES persistence failure recovery + automatic timer-driven timeout without follow-up keys.
    {
        CTextInputProcessor tipGen;
        tipGen.SetClientIdForTest(1);
        CHECK_TRUE(tipGen.NegotiateStage7ForTest(), L"NegotiateStage7 for tipGen");

        MockContext ctxG;
        uint64_t ctxGId = reinterpret_cast<uint64_t>(&ctxG);
        BOOL eaten = FALSE;
        tipGen.OnKeyDown(&ctxG, 'A', 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"tipGen 'A' eaten");

        // Advance local input_generation far past server candidate_version (simulating Rollback/Reset divergence)
        for (int i = 0; i < 10; ++i) {
            tipGen.AdvanceContextInputGeneration(ctxGId);
        }

        // Inject V2_RES _commit failure on commit completion, then recover via DrainUnconfirmedCommits
        ctxG.m_forceSyncBlocked = true;
        ctxG.m_deferAsyncSession = true;
        tipGen.OnKeyDown(&ctxG, VK_SPACE, 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"tipGen async commit queued");
        auto cbG = tipGen.GetRequestControlBlockForTest(2);
        CHECK_TRUE(cbG != nullptr && cbG->wal_prepared, L"tipGen V2_PREP persisted");
        CHECK_TRUE(cbG->server_commit_generation != cbG->input_generation,
                   L"Server commit generation tracked separately from local input_generation");

        receipt_journal_v2::FailCommitInjectCountForTest() = 1;
        HRESULT hrResFail = ctxG.m_pDeferredSession->DoEditSession(1);
        CHECK_EQUAL(hrResFail, S_OK, L"Host write succeeded before V2_RES failure");
        CHECK_TRUE(!cbG->wal_resolved, L"V2_RES failed on first _commit");
        CHECK_TRUE(tipGen.IsWalBarrierFaulted(), L"WAL barrier faulted on V2_RES failure");
        CHECK_TRUE(tipGen.GetContextState(ctxGId) == CompositionState::Quarantined, L"Context quarantined while V2_RES pending");
        CHECK_EQUAL(tipGen.GetPendingV2ReceiptCountForTest(), size_t(1), L"Failed V2_RES queued for runtime retry");

        tipGen.DrainUnconfirmedCommits(true);
        CHECK_EQUAL(tipGen.GetPendingV2ReceiptCountForTest(), size_t(0), L"Runtime retry persisted V2_RES, sent CommitTerminal with server_commit_generation, and persisted V2_ACK");
        CHECK_TRUE(!tipGen.IsWalBarrierFaulted(), L"WAL barrier fault cleared after recovery");
        CHECK_TRUE(tipGen.GetContextState(ctxGId) == CompositionState::None, L"Context retired from Quarantined after V2_RES recovery");
        CHECK_TRUE(server.m_terminalDispositions[cbG->commit_event_id] == IpcCommitDisposition::ConfirmedWritten,
                   L"CommitTerminal succeeded with server_commit_generation despite local input_generation divergence");

        ctxG.m_pDeferredSession->Release();
        ctxG.m_pDeferredSession = nullptr;

        // Automatic timer-driven observation timeout with ZERO subsequent keystrokes
        ctxG.m_forceSyncBlocked = false;
        ctxG.m_deferAsyncSession = false;
        tipGen.OnKeyDown(&ctxG, 'A', 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"tipGen second 'A' eaten");
        ctxG.m_forceSyncBlocked = true;
        ctxG.m_deferAsyncSession = true;
        tipGen.OnKeyDown(&ctxG, VK_SPACE, 0, &eaten);
        CHECK_EQUAL(eaten, TRUE, L"tipGen second async commit queued");
        auto cbTimeout = tipGen.GetRequestControlBlockForTest(4);
        CHECK_TRUE(cbTimeout != nullptr, L"Control block exists for timer timeout test");
        CHECK_TRUE(tipGen.HasPendingObservationRequests(), L"HasPendingObservationRequests is true while queued");

        // Pump Win32 WM_TIMER messages without any keystrokes until 1500ms timeout fires
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2200);
        while (!cbTimeout->observation_timed_out && std::chrono::steady_clock::now() < deadline) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(50);
        }
        CHECK_TRUE(cbTimeout->observation_timed_out, L"Drain timer automatically triggered observation timeout without follow-up keys");
        CHECK_TRUE(cbTimeout->revoked, L"Timed-out queued request automatically revoked");
        CHECK_TRUE(tipGen.GetContextState(ctxGId) == CompositionState::Quarantined, L"Context automatically quarantined by timer");

        HRESULT hrTimedOut = ctxG.m_pDeferredSession->DoEditSession(1);
        CHECK_EQUAL(hrTimedOut, E_ABORT, L"Timer-revoked session aborted cleanly");
        ctxG.m_pDeferredSession->Release();
        ctxG.m_pDeferredSession = nullptr;
        tipGen.Deactivate();
    }

    cleanupWals();

    // 3. v2 WAL contradictory terminal frame & corrupt tail resilience
    {
        receipt_journal_v2::Entry e{};
        e.owner_id = 0x1111;
        e.session_id = 0x2222;
        e.context_id = 0x3333;
        e.context_epoch = 1;
        e.input_generation = 1;
        e.edit_request_id = 1;
        e.commit_event_id = 7777;
        e.text_len = 1;
        e.text_digest = receipt_journal_v2::TextDigest(L"工");
        CHECK_TRUE(receipt_journal_v2::AppendPrepared(e), L"AppendPrepared succeeded");
        e.disposition = static_cast<uint32_t>(IpcCommitDisposition::ConfirmedWritten);
        CHECK_TRUE(receipt_journal_v2::AppendResolved(e), L"First AppendResolved(ConfirmedWritten) succeeded");
        e.disposition = static_cast<uint32_t>(IpcCommitDisposition::CancelDiscard);
        CHECK_TRUE(!receipt_journal_v2::AppendResolved(e), L"AppendResolved rejects contradictory terminal disposition");

        // Append corrupt tail bytes
        {
            std::ofstream ofs(receipt_journal_v2::Path(), std::ios::binary | std::ios::app);
            ofs << "V2_RES\t999\tCORRUPT_TAIL_NO_CRC\n";
        }

        receipt_journal_v2::Snapshot snap;
        CHECK_TRUE(receipt_journal_v2::LoadSnapshot(snap, false), L"LoadSnapshot survives corrupt tail");
        CHECK_TRUE(snap.v2_corrupt_tail_ignored, L"Corrupt tail flagged");
        auto it = snap.events.find(7777);
        CHECK_TRUE(it != snap.events.end(), L"Event 7777 loaded");
        CHECK_EQUAL(it->second.resolved_disposition, static_cast<uint32_t>(IpcCommitDisposition::ConfirmedWritten),
                    L"First terminal disposition preserved immutably");

        // _commit failure on AppendResolved followed by retry must execute SyncWalFileLocked and succeed
        receipt_journal_v2::Entry eRetry{};
        eRetry.owner_id = 0x1111;
        eRetry.session_id = 0x2222;
        eRetry.context_id = 0x3333;
        eRetry.commit_event_id = 7778;
        eRetry.disposition = static_cast<uint32_t>(IpcCommitDisposition::ConfirmedWritten);
        CHECK_TRUE(receipt_journal_v2::AppendPrepared(eRetry), L"AppendPrepared 7778");
        receipt_journal_v2::FailCommitInjectCountForTest() = 1;
        CHECK_TRUE(!receipt_journal_v2::AppendResolved(eRetry), L"AppendResolved 7778 fails when _commit fails");
        receipt_journal_v2::FailCommitInjectCountForTest() = 1;
        CHECK_TRUE(!receipt_journal_v2::AppendResolved(eRetry), L"Idempotent retry still executes _commit and fails if _commit fails");
        CHECK_TRUE(receipt_journal_v2::AppendResolved(eRetry), L"Retry succeeds once _commit succeeds");

        // Legitimate CancelRestore -> CancelDiscard downgrade must NOT be marked conflicted on WAL reload
        receipt_journal_v2::Entry eDown{};
        eDown.owner_id = 0x1111;
        eDown.session_id = 0x2222;
        eDown.context_id = 0x3333;
        eDown.commit_event_id = 7779;
        CHECK_TRUE(receipt_journal_v2::AppendPrepared(eDown), L"AppendPrepared 7779");
        eDown.disposition = static_cast<uint32_t>(IpcCommitDisposition::CancelRestore);
        CHECK_TRUE(receipt_journal_v2::AppendResolved(eDown), L"AppendResolved(CancelRestore) 7779");
        eDown.disposition = static_cast<uint32_t>(IpcCommitDisposition::CancelDiscard);
        CHECK_TRUE(receipt_journal_v2::AppendServiceAck(eDown), L"AppendServiceAck(CancelDiscard) 7779");

        receipt_journal_v2::Snapshot snapDown;
        CHECK_TRUE(receipt_journal_v2::LoadSnapshot(snapDown, false), L"Reload WAL after CancelRestore->CancelDiscard");
        auto itDown = snapDown.events.find(7779);
        CHECK_TRUE(itDown != snapDown.events.end(), L"Event 7779 loaded");
        CHECK_TRUE(!itDown->second.conflicted, L"Legitimate CancelRestore->CancelDiscard downgrade is NOT conflicted");
        CHECK_EQUAL(itDown->second.resolved_disposition, static_cast<uint32_t>(IpcCommitDisposition::CancelRestore),
                    L"Resolved keeps CancelRestore");
        CHECK_EQUAL(itDown->second.acked_disposition, static_cast<uint32_t>(IpcCommitDisposition::CancelDiscard),
                    L"Acked keeps CancelDiscard");
    }

    cleanupWals();

    // 4. Startup / Crash Recovery (RecoverReceiptsFromWal):
    //    - Dead-owner Prepared without Resolved -> QueryCommitStatus(None) -> settles ClosedUnknown (V2_RES + CommitTerminal + V2_ACK)
    //    - Resolved without ServiceAck -> resends CommitTerminal + records V2_ACK
    //    - Legacy v1 P1 ignored, R1 replayed & ACKed
    //    - Zero SetText / SendInput / UI resurrection
    {
        uint64_t evDeadPrep = 8001;
        uint64_t evUnackedRes = 8002;
        uint64_t evLegacyP1 = 8003;
        uint64_t evLegacyR1 = 8004;

        uint64_t deadSession = (static_cast<uint64_t>(GetCurrentProcessId()) << 32) | 0xBEEFull;
        EnterCriticalSection(&server.m_cs);
        server.m_pendingCommits[evDeadPrep] = { deadSession, 0xCAFE1, L"工", L"a", 0, {L"工"}, 0 };
        server.m_pendingCommits[evUnackedRes] = { deadSession, 0xCAFE2, L"戈", L"b", 0, {L"戈"}, 0 };
        server.m_pendingCommits[evLegacyP1] = { deadSession, 0xCAFE3, L"阿", L"c", 0, {L"阿"}, 0 };
        server.m_pendingCommits[evLegacyR1] = { deadSession, 0xCAFE4, L"啊", L"d", 0, {L"啊"}, 0 };
        LeaveCriticalSection(&server.m_cs);

        receipt_journal_v2::Entry e1{};
        e1.owner_id = 0xDEAD1234;
        e1.session_id = deadSession;
        e1.context_id = 0xCAFE1;
        e1.context_epoch = 1;
        e1.input_generation = 1;
        e1.edit_request_id = 10;
        e1.commit_event_id = evDeadPrep;
        CHECK_TRUE(receipt_journal_v2::AppendPrepared(e1), L"Write dead-owner V2_PREP");

        receipt_journal_v2::Entry e2{};
        e2.owner_id = 0xDEAD1234;
        e2.session_id = deadSession;
        e2.context_id = 0xCAFE2;
        e2.context_epoch = 1;
        e2.input_generation = 2;
        e2.edit_request_id = 11;
        e2.commit_event_id = evUnackedRes;
        e2.disposition = static_cast<uint32_t>(IpcCommitDisposition::ConfirmedWritten);
        CHECK_TRUE(receipt_journal_v2::AppendPrepared(e2), L"Write V2_PREP for unacked resolved");
        CHECK_TRUE(receipt_journal_v2::AppendResolved(e2), L"Write V2_RES(ConfirmedWritten) without V2_ACK");

        // Legacy v1 P1 + R1
        CHECK_TRUE(receipt_journal::Prepare({ deadSession, 0xCAFE3, evLegacyP1, 0, 0 }), L"Write v1 P1");
        CHECK_TRUE(receipt_journal::Append({ { deadSession, 0xCAFE4, evLegacyR1, 0, 0 } }), L"Write v1 R1");

        CTextInputProcessor tip2;
        tip2.SetClientIdForTest(1);
        CHECK_TRUE(tip2.NegotiateStage7ForTest(), L"NegotiateStage7 for recovery TIP");
        int learnedBefore = server.m_learnedCount.load();
        tip2.RecoverReceiptsFromWal();

        CHECK_TRUE(server.m_terminalDispositions[evDeadPrep] == IpcCommitDisposition::ClosedUnknown,
                   L"Dead-owner Prepared settled as ClosedUnknown");
        CHECK_TRUE(server.m_terminalDispositions[evUnackedRes] == IpcCommitDisposition::ConfirmedWritten,
                   L"Unacked Resolved resent CommitTerminal(ConfirmedWritten)");
        CHECK_EQUAL(server.m_pendingCommits[evLegacyP1].state, 0,
                    L"Legacy v1 P1 intent-only record was NEVER inferred as success or cancel!");
        CHECK_EQUAL(server.m_pendingCommits[evLegacyR1].state, 1,
                    L"Legacy v1 R1 record was replayed and confirmed");
        CHECK_TRUE(server.m_learnedCount.load() >= learnedBefore + 1,
                   L"ConfirmedWritten increased learned count while ClosedUnknown did not");
        CHECK_EQUAL(IsWindowVisible(tip2.GetCandidateWindowForTest().GetHwnd()) != FALSE, false,
                    L"Startup recovery NEVER resurrects candidate UI");

        receipt_journal_v2::Snapshot postSnap;
        CHECK_TRUE(receipt_journal_v2::LoadSnapshot(postSnap, true), L"Post-recovery LoadSnapshot");
        CHECK_TRUE(postSnap.events[evDeadPrep].has_service_ack, L"ClosedUnknown got V2_ACK");
        CHECK_TRUE(postSnap.events[evUnackedRes].has_service_ack, L"Resent ConfirmedWritten got V2_ACK");
    }

    cleanupWals();
    server.Stop();
    IpcClient::SetPipeNameForTest(nullptr);
    std::wcout << L"[PASS] Stage 7.1c Request Fence, UnknownPending & WAL v2 Recovery verified!" << std::endl;
    return 0;
}

int main() {
    std::wcout << L"=== Running TSF EditSession & Composition Lifecycle Test Suite ===" << std::endl;

    if (TestStartCompositionRefcountPairing() != 0) return 1;
    if (TestSetCompositionSelfAssignment() != 0) return 1;
    if (TestCancelSetTextFailure() != 0) return 1;
    if (TestCommitEndCompositionFailure() != 0) return 1;
    if (TestSetSelectionFailure() != 0) return 1;
    if (TestContextSwitchCompositionTermination() != 0) return 1;
    if (TestPersistentRangeCommitFailureRecovery() != 0) return 1;
    if (TestPersistentRangeCommitFailureStillFailing() != 0) return 1;
    if (TestContextSwitchCommitPendingEndPreservesText() != 0) return 1;
    if (TestRound6PersistentRecoverySequence() != 0) return 1;
    if (TestMultiContextPendingTerminationTracking() != 0) return 1;
    if (TestWorkerThreadCreationFailureFallback() != 0) return 1;
    if (TestPendingRefCountAndDestructionSafety() != 0) return 1;
    if (TestQueuedTerminationSingleFlight() != 0) return 1;
    if (TestDeactivateFailureRemainsRecoverable() != 0) return 1;
    if (TestAsyncQueuedEditSessionCommitAndFailureMatrix() != 0) return 1;
    if (TestSnapshotBoundMouseSelectionAndLatePaging() != 0) return 1;
    if (TestPluginSyncNonBlockingAndDisabledClickRejection() != 0) return 1;
    if (TestStage71bCompositionlessAndDisplayAttributes() != 0) return 1;
    if (TestStage71bReviewFixes() != 0) return 1;
    if (TestStage71cRequestFenceAndWalRecovery() != 0) return 1;

    std::wcout << L"=== ALL Composition Lifecycle Tests PASSED! ===" << std::endl;
    return 0;
}
