#include <windows.h>
#include <msctf.h>
#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include "../src/tsf_client/globals.h"
#include "../src/tsf_client/edit_session.h"
#include "../src/tsf_client/text_input_processor.h"

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
static std::wstring g_lastCommittedText = L"";

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

    STDMETHODIMP SetText(TfEditCookie ec, DWORD dwFlags, const WCHAR* pchText, LONG cch) override {
        if (g_failSetText) return E_FAIL;
        m_text = (pchText && cch > 0) ? std::wstring(pchText, cch) : L"";
        g_lastCommittedText = m_text;
        return S_OK;
    }

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
    STDMETHODIMP Collapse(TfEditCookie ec, TfAnchor aPos) override { return S_OK; }
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

// 3. Mock ITfContext and ITfContextComposition
class MockContext : public ITfContext, public ITfContextComposition {
public:
    MockContext() : m_cRef(1) {}
    virtual ~MockContext() {
        if (m_pDeferredSession) {
            m_pDeferredSession->Release();
            m_pDeferredSession = nullptr;
        }
    }

    bool m_forceSyncBlocked = false;
    bool m_failAsyncRequest = false;
    bool m_deferAsyncSession = false;
    int m_asyncRequestCount = 0;
    ITfEditSession* m_pDeferredSession = nullptr;

    void ExecuteDeferredSession(TfEditCookie ec = 1001) {
        if (m_pDeferredSession) {
            ITfEditSession* pes = m_pDeferredSession;
            m_pDeferredSession = nullptr;
            pes->DoEditSession(ec);
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
        *phrSession = pes->DoEditSession(1001); // Invoke directly with mock cookie
        return S_OK;
    }

    STDMETHODIMP InWriteSession(TfClientId tid, BOOL *pfWriteSession) override {
        if (pfWriteSession) *pfWriteSession = TRUE;
        return S_OK;
    }

    STDMETHODIMP GetSelection(TfEditCookie ec, ULONG ulIndex, ULONG ulCount, TF_SELECTION* pSelection, ULONG* pcFetched) override {
        if (!pSelection || !pcFetched) return E_POINTER;
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
    STDMETHODIMP GetStatus(TF_STATUS* pdcs) override { return E_NOTIMPL; }
    STDMETHODIMP GetProperty(REFGUID guidProp, ITfProperty** ppProp) override { return E_NOTIMPL; }
    STDMETHODIMP GetAppProperty(REFGUID guidProp, ITfReadOnlyProperty** ppProp) override { return E_NOTIMPL; }
    STDMETHODIMP TrackProperties(const GUID **prgProp, ULONG cProp, const GUID **prgAppProp, ULONG cAppProp, ITfReadOnlyProperty** ppProperty) override { return E_NOTIMPL; }
    STDMETHODIMP EnumProperties(IEnumTfProperties **ppEnum) override { return E_NOTIMPL; }
    STDMETHODIMP GetDocumentMgr(ITfDocumentMgr **ppDm) override { return E_NOTIMPL; }
    STDMETHODIMP CreateRangeBackup(TfEditCookie ec, ITfRange *pRange, ITfRangeBackup **ppBackup) override { return E_NOTIMPL; }

    // ITfContextComposition methods
    STDMETHODIMP StartComposition(TfEditCookie ecWrite, ITfRange* pCompositionRange, ITfCompositionSink* pSink, ITfComposition** ppComposition) override {
        if (!ppComposition) return E_POINTER;
        auto* comp = new MockComposition();
        m_lastCreatedComposition = comp;
        *ppComposition = comp; // COM contract: returns AddRef'd pointer (ref count = 1)
        return S_OK;
    }

    STDMETHODIMP EnumCompositions(IEnumITfCompositionView** ppEnum) override { return E_NOTIMPL; }
    STDMETHODIMP FindComposition(TfEditCookie ecRead, ITfRange *pTestRange, IEnumITfCompositionView **ppEnum) override { return E_NOTIMPL; }
    STDMETHODIMP TakeOwnership(TfEditCookie ecWrite, ITfCompositionView *pComposition, ITfCompositionSink *pSink, ITfComposition** ppComposition) override { return E_NOTIMPL; }

    MockComposition* m_lastCreatedComposition = nullptr;
    LONG m_cRef = 1;
};

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

    std::wcout << L"=== ALL Composition Lifecycle Tests PASSED! ===" << std::endl;
    return 0;
}
