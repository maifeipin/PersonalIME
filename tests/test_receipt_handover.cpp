#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <memory>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <iostream>
#define private public
#include "tsf_client/text_input_processor.h"
#include "tsf_client/receipt_retry.h"
#undef private
LONG g_cDllRef=0;HINSTANCE g_hInst=nullptr;
#define REQUIRE(x) do{if(!(x)){std::cerr<<"FAIL line "<<__LINE__<<": "<<#x<<'\n';return 1;}}while(false)
int main(){
    auto dir=std::filesystem::absolute("handover-"+std::to_string(GetCurrentProcessId()));std::filesystem::create_directories(dir);
    SetEnvironmentVariableW(L"TEMP",dir.c_str());SetEnvironmentVariableW(L"TMP",dir.c_str());
    auto path=receipt_journal::Path();std::filesystem::create_directory(path);
    auto tip=new CTextInputProcessor();
    // Obtain the true owner through a successful durable legacy-compatible spill later.
    auto sid=tip->m_sessionId;
    bool accepted=tip->EnqueueUnconfirmedCommit(123,700,CTextInputProcessor::UnconfirmedAction::Success);
    REQUIRE(!accepted);REQUIRE(tip->m_unconfirmedCommits.empty());REQUIRE(receipt_retry::Pending());
    delete tip; // Worker owns the unresolved result; no reference to this object.
    REQUIRE(receipt_retry::Pending());REQUIRE(g_cDllRef>0);
    REQUIRE(std::filesystem::remove(path));
    auto deadline=GetTickCount64()+5000;
    while((receipt_retry::Pending()||g_cDllRef!=0)&&GetTickCount64()<deadline)Sleep(20);
    REQUIRE(!receipt_retry::Pending());REQUIRE(g_cDllRef==0);
    std::vector<receipt_journal::Record> rows;REQUIRE(receipt_journal::Read(rows));
    REQUIRE(rows.size()==1&&rows[0].sid==sid&&rows[0].cid==123&&rows[0].eid==700&&rows[0].action==0);
    REQUIRE(receipt_journal::Ack(700));REQUIRE(receipt_journal::Read(rows)&&rows.empty());
    std::cout<<"PASS: failure/backpressure, TIP destruction, independent lifetime, storage recovery, original owner\n";
}
