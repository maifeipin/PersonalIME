#include <windows.h>
#include <msctf.h>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <memory>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <iostream>
// Keep IPC's private inline test-pipe storage identical in every translation unit.
#include "tsf_client/ipc_client.h"
#define private public
#include "tsf_client/text_input_processor.h"
#undef private
LONG g_cDllRef = 0;
HINSTANCE g_hInst = nullptr;
class ReadWriteContext final : public ITfContext {
    LONG refs = 1;
public:
    STDMETHODIMP QueryInterface(REFIID id, void** out) override { if (!out) return E_POINTER; *out = nullptr; if (id != IID_IUnknown && id != IID_ITfContext) return E_NOINTERFACE; *out = static_cast<ITfContext*>(this); AddRef(); return S_OK; }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() override { auto n = InterlockedDecrement(&refs); if (!n) delete this; return n; }
    STDMETHODIMP RequestEditSession(TfClientId, ITfEditSession*, DWORD, HRESULT*) override { return E_NOTIMPL; }
    STDMETHODIMP InWriteSession(TfClientId, BOOL*) override { return E_NOTIMPL; }
    STDMETHODIMP GetSelection(TfEditCookie, ULONG, ULONG, TF_SELECTION*, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP SetSelection(TfEditCookie, ULONG, const TF_SELECTION*) override { return E_NOTIMPL; }
    STDMETHODIMP GetStart(TfEditCookie, ITfRange**) override { return E_NOTIMPL; }
    STDMETHODIMP GetEnd(TfEditCookie, ITfRange**) override { return E_NOTIMPL; }
    STDMETHODIMP GetActiveView(ITfContextView**) override { return E_NOTIMPL; }
    STDMETHODIMP EnumViews(IEnumTfContextViews**) override { return E_NOTIMPL; }
    STDMETHODIMP GetStatus(TF_STATUS* out) override { if (!out) return E_POINTER; *out = {}; return S_OK; }
    STDMETHODIMP GetProperty(REFGUID, ITfProperty**) override { return E_NOTIMPL; }
    STDMETHODIMP GetAppProperty(REFGUID, ITfReadOnlyProperty**) override { return E_NOTIMPL; }
    STDMETHODIMP TrackProperties(const GUID**, ULONG, const GUID**, ULONG, ITfReadOnlyProperty**) override { return E_NOTIMPL; }
    STDMETHODIMP EnumProperties(IEnumTfProperties**) override { return E_NOTIMPL; }
    STDMETHODIMP GetDocumentMgr(ITfDocumentMgr**) override { return E_NOTIMPL; }
    STDMETHODIMP CreateRangeBackup(TfEditCookie, ITfRange*, ITfRangeBackup**) override { return E_NOTIMPL; }
};
int main() {
    wchar_t oldTemp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, oldTemp);
    auto dir = std::filesystem::temp_directory_path() / ("PersonalIME_snapshot_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    SetEnvironmentVariableW(L"TEMP", dir.c_str()); SetEnvironmentVariableW(L"TMP", dir.c_str());
    struct Cleanup {
        std::filesystem::path p;
        std::wstring prev;
        ~Cleanup() {
            SetEnvironmentVariableW(L"TEMP", prev.c_str());
            SetEnvironmentVariableW(L"TMP", prev.c_str());
            std::error_code ec;
            std::filesystem::remove_all(p, ec);
        }
    } cleanup{dir, oldTemp};
    std::wstring name = L"\\\\.\\pipe\\PersonalIME_Snapshot_" + std::to_wstring(GetCurrentProcessId());
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 16384, 16384, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 1;
    HANDLE clientDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!clientDone) { CloseHandle(pipe); return 1; }
    std::thread server([pipe, clientDone] {
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) { CloseHandle(pipe); return; }
        OVERLAPPED ov{}; ov.hEvent = event;
        auto completed = [&](BOOL immediate) {
            if (immediate) return true;
            DWORD error = GetLastError(), n = 0;
            if (error == ERROR_PIPE_CONNECTED) return true;
            if (error != ERROR_IO_PENDING) return false;
            if (WaitForSingleObject(event, 1500) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &ov); GetOverlappedResult(pipe, &ov, &n, TRUE); return false;
            }
            return GetOverlappedResult(pipe, &ov, &n, FALSE) != FALSE;
        };
        if (completed(ConnectNamedPipe(pipe, &ov))) {
            IpcKeyRequest request{}; DWORD n = 0;
            ov = {}; ov.hEvent = event; ResetEvent(event);
            if (completed(ReadFile(pipe, &request, sizeof(request), &n, &ov))) {
                IpcKeyResponse response{};
                response.msg_type = request.msg_type; response.session_id = request.session_id; response.context_id = request.context_id;
                response.protocol_version = IPC_PROTOCOL_VERSION_STAGE3; response.handled = 1;
                // PageUp at the first page leaves both composition and candidates unchanged.
                ov = {}; ov.hEvent = event; ResetEvent(event);
                if (completed(WriteFile(pipe, &response, sizeof(response), &n, &ov))) WaitForSingleObject(clientDone, 1500);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(event);
        CloseHandle(pipe);
    });
    IpcClient::SetPipeNameForTest(name.c_str());
    auto context = new ReadWriteContext(); auto tip = new CTextInputProcessor();
    uint64_t id = reinterpret_cast<uint64_t>(static_cast<ITfContext*>(context));
    tip->m_contextCandidateVersions[id] = 7;
    tip->m_contextCandidateVersions[id + 1] = 9;
    BOOL eaten = FALSE;
    tip->OnKeyDown(context, VK_PRIOR, 0, &eaten);
    bool ok = eaten && tip->m_contextCandidateVersions[id] == 7 && tip->m_contextCandidateVersions[id + 1] == 9;
    delete tip; context->Release(); IpcClient::SetPipeNameForTest(nullptr);
    SetEvent(clientDone);
    server.join();
    CloseHandle(clientDone);
    if (!ok) { std::cerr << "FAIL: unchanged candidate response cleared a valid context snapshot\n"; return 1; }
    std::cout << "PASS: real TIP preserves valid snapshots on handled responses without state updates\n";
}
