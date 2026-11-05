#pragma once
#include <windows.h>
#include <io.h>
#include <cstdio>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// One append-only journal. Existing text logs are read-only migration sources.
// ACKs are terminal across all sources, including REC records appended later.
namespace receipt_journal {
struct Record { uint64_t sid=0,cid=0,eid=0; int action=0,retry=0; };
class Lock {
    HANDLE h_=nullptr; bool owned_=false;
public:
    explicit Lock(const wchar_t* name=L"Local\\PersonalIME_Receipt_Wal_Mutex") {
        h_=CreateMutexW(nullptr,FALSE,name);
        if(h_) { DWORD r=WaitForSingleObject(h_,50); owned_=r==WAIT_OBJECT_0||r==WAIT_ABANDONED; }
    }
    ~Lock(){if(h_){if(owned_)ReleaseMutex(h_);CloseHandle(h_);}}
    explicit operator bool()const{return owned_;}
};
inline std::filesystem::path Directory(){
    wchar_t p[MAX_PATH];DWORD n=GetTempPathW(MAX_PATH,p);
    return n&&n<MAX_PATH?std::filesystem::path(p):std::filesystem::path();
}
inline std::filesystem::path Path(){return Directory()/L"PersonalIME_receipts.wal";}
inline uint64_t Checksum(const std::string& s){uint64_t h=14695981039346656037ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}return h;}
inline bool Same(const Record&a,const Record&b){return a.sid==b.sid&&a.cid==b.cid&&a.eid==b.eid&&a.action==b.action;}
inline bool Valid(const Record&r){return r.sid&&r.cid&&r.eid&&(r.action==0||r.action==1)&&r.retry>=0&&r.retry<=32;}
struct Snapshot {std::map<uint64_t,Record> active;std::set<uint64_t> acked;};
inline bool ReadFile(const std::filesystem::path& p,bool framed,Snapshot& s){
    FILE*f=nullptr;
    if(_wfopen_s(&f,p.c_str(),L"rb")!=0||!f){return errno==ENOENT;}
    char buf[1024];bool ok=true;
    while(fgets(buf,sizeof(buf),f)){
        std::string line(buf);
        // An interrupted tail is not a complete journal frame. The next append
        // starts with a newline so that it cannot concatenate with that tail.
        if(line.empty()||line.back()!='\n'){
            if(!feof(f)){int ch;while((ch=fgetc(f))!=EOF&&ch!='\n'){} }
            OutputDebugStringW(L"PersonalIME: ignored incomplete receipt frame\n");continue;
        }
        while(!line.empty()&&(line.back()=='\n'||line.back()=='\r'))line.pop_back();
        if(line.empty())continue;
        if(framed){
            auto pos=line.rfind(' ');uint64_t hash=0;std::string extra;
            if(pos==std::string::npos){OutputDebugStringW(L"PersonalIME: invalid receipt frame\n");continue;}
            std::istringstream hs(line.substr(pos+1));
            if(!(hs>>hash)||(hs>>extra)||hash!=Checksum(line.substr(0,pos))){OutputDebugStringW(L"PersonalIME: receipt checksum failure\n");continue;}
            line.resize(pos);
        }
        std::istringstream in(line);std::string kind,extra;in>>kind;
        if(kind=="A1"||kind=="ACK"){
            uint64_t eid=0;if(!(in>>eid)||!eid||(in>>extra)){ok=false;break;}
            s.acked.insert(eid);continue;
        }
        if(kind=="P1")continue; // Durable host intent, NOT evidence of host success.
        Record r;
        if(kind=="R1"||kind=="REC"){
            if(!(in>>r.sid>>r.cid>>r.eid>>r.action>>r.retry)||(in>>extra)){ok=false;break;}
        }else{
            in.clear();in.str(line);
            if(!(in>>r.sid>>r.cid>>r.eid>>r.action>>r.retry)||(in>>extra)){
                // Four-field logs never contained a stable owner. Preserve them,
                // but do not impersonate the new TIP session to replay them.
                OutputDebugStringW(L"PersonalIME: legacy receipt needs owner recovery\n");continue;
            }
        }
        if(!Valid(r)){ok=false;break;}
        auto it=s.active.find(r.eid);
        if(it!=s.active.end()&&!Same(it->second,r)){ok=false;break;}
        s.active[r.eid]=r;
    }
    if(ferror(f))ok=false;
    if(fclose(f)!=0)ok=false;
    return ok;
}
inline bool ReadLocked(Snapshot& s){
    if(!ReadFile(Path(),true,s))return false;
    // Read previous deployments without rewriting or deleting their only copy.
    Lock oldMain(L"Local\\PersonalIME_Receipt_File_Mutex");if(!oldMain)return false;
    Lock oldEmergency(L"Local\\PersonalIME_Emergency_File_Mutex");if(!oldEmergency)return false;
    std::error_code ec;
    for(std::filesystem::directory_iterator i(Directory(),ec),end;!ec&&i!=end;i.increment(ec)){
        auto name=i->path().filename().wstring();
        if(name.rfind(L"PersonalIME_spillover_receipts.dat",0)!=0&&name.rfind(L"PersonalIME_emergency_receipts.dat",0)!=0)continue;
        if(!ReadFile(i->path(),false,s))return false;
    }
    if(ec)return false;
    for(auto eid:s.acked)s.active.erase(eid);
    return true;
}
inline bool AppendFrame(const std::string& body){
    FILE*f=nullptr;if(_wfopen_s(&f,Path().c_str(),L"a+b")!=0||!f)return false;
    std::string frame="\n"+body+" "+std::to_string(Checksum(body))+"\n";
    bool ok=fwrite(frame.data(),1,frame.size(),f)==frame.size();
    if(fflush(f)!=0||ferror(f)||_commit(_fileno(f))!=0)ok=false;
    if(fclose(f)!=0)ok=false;return ok;
}
inline std::string Payload(const Record&r){return std::to_string(r.sid)+" "+std::to_string(r.cid)+" "+std::to_string(r.eid)+" "+std::to_string(r.action)+" "+std::to_string(r.retry);}
inline bool Read(std::vector<Record>& records){
    Lock lock;if(!lock)return false;Snapshot s;if(!ReadLocked(s))return false;
    records.clear();for(auto& [_,r]:s.active)records.push_back(r);return true;
}
inline bool Append(const std::vector<Record>& records){
    Lock lock;if(!lock)return false;Snapshot s;if(!ReadLocked(s))return false;
    for(auto&r:records){
        if(!Valid(r))return false;
        if(s.acked.count(r.eid))continue;
        auto it=s.active.find(r.eid);
        // A complete visible frame may be the residue of a failed _commit.
        // Re-append and sync matching retries before reporting durability.
        if(it!=s.active.end()&&!Same(it->second,r))return false;
        if(!AppendFrame("R1 "+Payload(r)))return false;
        s.active[r.eid]=r;
    }return true;
}
inline bool Ack(uint64_t eid){Lock lock;if(!lock||!eid)return false;return AppendFrame("A1 "+std::to_string(eid));}
inline bool Prepare(const Record&r){Lock lock;if(!lock||!Valid(r))return false;return AppendFrame("P1 "+Payload(r));}
} // namespace receipt_journal
