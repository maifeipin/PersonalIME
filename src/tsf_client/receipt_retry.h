#pragma once
#include "receipt_journal.h"
#include "globals.h"
#include <mutex>

// Own failed handovers independently of a TIP's lifetime. This is deliberately
// NOT called durable: process loss before Append succeeds remains indeterminate.
namespace receipt_retry {
struct State {std::mutex mutex;std::map<uint64_t,receipt_journal::Record> records;bool running=false;bool referenceHeld=false;};
inline State& Shared(){static auto* state=new State;return *state;}
inline DWORD WINAPI Run(void* module){
    auto& s=Shared();
    for(;;){
        std::vector<receipt_journal::Record> batch;
        {std::lock_guard<std::mutex> guard(s.mutex);for(auto&[_,r]:s.records){batch.push_back(r);if(batch.size()==8)break;}
         if(batch.empty()){s.running=false;s.referenceHeld=false;DllRelease();break;}}
        for(auto&r:batch)if(receipt_journal::Append({r})){
            std::lock_guard<std::mutex> guard(s.mutex);s.records.erase(r.eid);
        }
        Sleep(300);
    }
    HMODULE h=static_cast<HMODULE>(module);
    if(h&&h!=GetModuleHandleW(nullptr))FreeLibraryAndExitThread(h,0);
    return 0;
}
inline bool Pending(){auto&s=Shared();std::lock_guard<std::mutex> guard(s.mutex);return !s.records.empty();}
inline void Retain(const receipt_journal::Record&r){
    auto&s=Shared();std::lock_guard<std::mutex> guard(s.mutex);
    auto existing=s.records.find(r.eid);
    if(existing!=s.records.end()&&!receipt_journal::Same(existing->second,r)){
        OutputDebugStringW(L"PersonalIME: conflicting receipt owner/action rejected\n");return;
    }
    s.records[r.eid]=r;
    if(s.running)return;
    if(!s.referenceHeld){DllAddRef();s.referenceHeld=true;}
    HMODULE module=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&Run),&module))return;
    s.running=true;
    HANDLE thread=CreateThread(nullptr,0,Run,module,0,nullptr);
    if(thread)CloseHandle(thread);
    else {s.running=false;if(module!=GetModuleHandleW(nullptr))FreeLibrary(module);}
    OutputDebugStringW(L"PersonalIME: receipt handover pending; new host edits paused\n");
}
} // namespace receipt_retry
