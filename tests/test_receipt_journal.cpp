#include "tsf_client/receipt_journal.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <atomic>
using namespace receipt_journal;
#define REQUIRE(x) do{if(!(x)){std::cerr<<"FAIL line "<<__LINE__<<": "<<#x<<'\n';return 1;}}while(false)
int main(int argc,char**argv){
    if(argc>1){std::vector<Record> r;for(int i=0;i<40;++i)r.push_back({12,34,static_cast<uint64_t>(std::stoull(argv[1])+i),0,0});for(int i=0;i<60;++i){if(Append(r))return 0;Sleep(50);}return 1;}
    auto dir=std::filesystem::absolute("receipt-unit-"+std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);SetEnvironmentVariableW(L"TEMP",dir.c_str());SetEnvironmentVariableW(L"TMP",dir.c_str());
    std::vector<Record> r;
    REQUIRE(Read(r)&&r.empty());
    REQUIRE(Prepare({1,2,3,0,0}));REQUIRE(Read(r)&&r.empty());
    REQUIRE(Append({{1,2,3,0,0}}));REQUIRE(Read(r)&&r.size()==1&&r[0].sid==1);
    REQUIRE(!Append({{9,2,3,0,0}}));REQUIRE(!Append({{1,2,3,1,0}}));
    REQUIRE(Ack(3));REQUIRE(Append({{1,2,3,0,0}}));REQUIRE(Read(r)&&r.empty());
    REQUIRE(AppendFrame("R1 1 2 3 0 0"));REQUIRE(Read(r)&&r.empty()); // delayed REC cannot undo a terminal ACK
    {std::ofstream f(Path(),std::ios::app|std::ios::binary);f<<"R1 999 2 444";}
    REQUIRE(Append({{1,2,4,1,0}}));REQUIRE(Read(r)&&r.size()==1&&r[0].eid==4);
    // Multiple real processes use the same WAL lock and preserve all frames.
    wchar_t exe[MAX_PATH];GetModuleFileNameW(nullptr,exe,MAX_PATH);
    PROCESS_INFORMATION pi[3]{};
    for(int i=0;i<3;++i){std::wstring cmd=L"\""+std::wstring(exe)+L"\" "+std::to_wstring(100+i*100);STARTUPINFOW si{};si.cb=sizeof(si);REQUIRE(CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi[i]));}
    for(auto&p:pi){REQUIRE(WaitForSingleObject(p.hProcess,10000)==WAIT_OBJECT_0);DWORD exit=1;GetExitCodeProcess(p.hProcess,&exit);CloseHandle(p.hProcess);CloseHandle(p.hThread);REQUIRE(exit==0);}
    REQUIRE(Read(r)&&r.size()==121);
    // Backup-only, swap-only, tmp-only and emergency-only are immutable migration sources.
    const wchar_t* files[]={L"PersonalIME_spillover_receipts.dat.bak",L"PersonalIME_spillover_receipts.dat.swap",L"PersonalIME_spillover_receipts.dat.tmp",L"PersonalIME_emergency_receipts.dat.bak"};
    for(int i=0;i<4;++i){std::ofstream f(dir/files[i]);f<<"1 2 "<<(700+i)<<" 0 0\n";}
    REQUIRE(Read(r)&&r.size()==125);for(int i=0;i<4;++i)REQUIRE(Ack(700+i));REQUIRE(Read(r)&&r.size()==121);
    REQUIRE(std::filesystem::exists(dir/files[0])); // No migration source is deleted.
    // Lock contention is failure/unknown, never an empty successful read.
    std::atomic<bool> ready=false,release=false;
    std::thread holder([&]{Lock lock;ready=true;while(!release)Sleep(1);});while(!ready)Sleep(1);
    bool appended=Append({{1,2,9999,0,0}}),read=Read(r);release=true;holder.join();REQUIRE(!appended&&!read);
    // Storage unavailable returns failure; no false persisted status.
    auto other=dir/L"blocked";std::filesystem::create_directory(other);std::filesystem::create_directory(other/L"PersonalIME_receipts.wal");
    SetEnvironmentVariableW(L"TEMP",other.c_str());SetEnvironmentVariableW(L"TMP",other.c_str());
    REQUIRE(!Append({{1,2,8888,0,0}}));REQUIRE(!Prepare({1,2,8888,0,0}));
    std::cout<<"PASS: intent/outcome, ownership, terminal ACK, torn tail, multiprocess append, legacy recovery, lock and IO failure\n";
    return 0;
}
