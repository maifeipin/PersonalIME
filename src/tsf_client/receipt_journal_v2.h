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
#include "../common/ipc_protocol.h"
#include "receipt_journal.h"

// Stage 7.1c: Versioned v2 Write-Ahead Log (PersonalIME_receipts_v2.wal)
// - Separate from v1 PersonalIME_receipts.wal (legacy logs remain read-only migration sources).
// - Checksum-framed, append-only with fflush + _commit durability barrier.
// - Never writes raw input text; records only text length and FNV-1a digest.
// - Enforces terminal disposition immutability and flags conflicting terminal frames.
namespace receipt_journal_v2 {

enum class RecordKind {
    Prepared          = 1, // V2_PREP: Server persisted pending_commit; client intends to write
    InjectionAttempt  = 2, // V2_INJ:  Persisted before calling SendInput
    UnknownPending    = 3, // V2_UNK:  Outcome unknown with live potential writer / observation timeout
    Resolved          = 4, // V2_RES:  Terminal disposition persisted before CommitTerminal
    ServiceAck        = 5  // V2_ACK:  Server confirmed atomic persistence of terminal disposition
};

struct Entry {
    uint32_t version = 2;
    uint64_t seq = 0;
    uint64_t owner_id = 0;
    uint64_t session_id = 0;
    uint64_t context_id = 0;
    uint64_t context_epoch = 0;
    uint64_t input_generation = 0;
    uint64_t edit_request_id = 0;
    uint64_t commit_event_id = 0;
    uint32_t channel = 0;          // 0 = TSF, 1 = SendInput
    uint32_t text_len = 0;
    uint64_t text_digest = 0;
    uint32_t disposition = 0;      // IpcCommitDisposition
    uint32_t reason_or_stage = 0;
    int32_t  hr = 0;
};

struct EventState {
    uint64_t commit_event_id = 0;
    uint64_t owner_id = 0;
    uint64_t session_id = 0;
    uint64_t context_id = 0;
    uint64_t context_epoch = 0;
    uint64_t input_generation = 0;
    uint64_t edit_request_id = 0;
    uint32_t channel = 0;
    uint32_t text_len = 0;
    uint64_t text_digest = 0;
    bool has_prepared = false;
    bool has_injection_attempt = false;
    bool has_unknown_pending = false;
    bool has_resolved = false;
    bool has_service_ack = false;
    bool conflicted = false;
    uint32_t resolved_disposition = 0;
    uint32_t acked_disposition = 0;
    int32_t last_hr = 0;
};

struct Snapshot {
    uint64_t max_seq = 0;
    std::map<uint64_t, EventState> events;
    std::vector<receipt_journal::Record> legacy_records;
    bool v2_corrupt_tail_ignored = false;
};

class Lock {
    HANDLE h_ = nullptr;
    bool owned_ = false;
public:
    explicit Lock(const wchar_t* name = L"Local\\PersonalIME_Receipt_Wal_V2_Mutex") {
        h_ = CreateMutexW(nullptr, FALSE, name);
        if (h_) {
            DWORD r = WaitForSingleObject(h_, 100);
            owned_ = (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED);
        }
    }
    ~Lock() {
        if (h_) {
            if (owned_) ReleaseMutex(h_);
            CloseHandle(h_);
        }
    }
    explicit operator bool() const { return owned_; }
};

inline std::filesystem::path Path() {
    return receipt_journal::Directory() / L"PersonalIME_receipts_v2.wal";
}

inline uint64_t TextDigest(const std::wstring& text) {
    uint64_t h = 14695981039346656037ull;
    for (wchar_t ch : text) {
        uint16_t u = static_cast<uint16_t>(ch);
        h ^= (u & 0xFFu);
        h *= 1099511628211ull;
        h ^= ((u >> 8) & 0xFFu);
        h *= 1099511628211ull;
    }
    return h;
}

inline const char* KindTag(RecordKind k) {
    switch (k) {
        case RecordKind::Prepared:         return "V2_PREP";
        case RecordKind::InjectionAttempt: return "V2_INJ";
        case RecordKind::UnknownPending:   return "V2_UNK";
        case RecordKind::Resolved:         return "V2_RES";
        case RecordKind::ServiceAck:       return "V2_ACK";
        default:                           return "V2_BAD";
    }
}

inline bool ParseKindTag(const std::string& tag, RecordKind& out) {
    if (tag == "V2_PREP") { out = RecordKind::Prepared; return true; }
    if (tag == "V2_INJ")  { out = RecordKind::InjectionAttempt; return true; }
    if (tag == "V2_UNK")  { out = RecordKind::UnknownPending; return true; }
    if (tag == "V2_RES")  { out = RecordKind::Resolved; return true; }
    if (tag == "V2_ACK")  { out = RecordKind::ServiceAck; return true; }
    return false;
}

inline std::string FormatBody(RecordKind kind, const Entry& e) {
    std::ostringstream os;
    os << KindTag(kind) << " "
       << e.version << " "
       << e.seq << " "
       << e.owner_id << " "
       << e.session_id << " "
       << e.context_id << " "
       << e.context_epoch << " "
       << e.input_generation << " "
       << e.edit_request_id << " "
       << e.commit_event_id << " "
       << e.channel << " "
       << e.text_len << " "
       << e.text_digest << " "
       << e.disposition << " "
       << e.reason_or_stage << " "
       << e.hr;
    return os.str();
}

inline bool ReadV2FileLocked(Snapshot& snap) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, Path().c_str(), L"rb") != 0 || !f) {
        return errno == ENOENT;
    }

    char buf[2048];
    bool ok = true;
    while (fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        if (line.empty() || line.back() != '\n') {
            if (!feof(f)) {
                int ch;
                while ((ch = fgetc(f)) != EOF && ch != '\n') {}
            }
            snap.v2_corrupt_tail_ignored = true;
            OutputDebugStringW(L"PersonalIME: v2 WAL ignored incomplete frame\n");
            continue;
        }
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        if (line.empty()) continue;

        size_t pos = line.rfind(' ');
        if (pos == std::string::npos) {
            snap.v2_corrupt_tail_ignored = true;
            OutputDebugStringW(L"PersonalIME: v2 WAL invalid frame without checksum\n");
            continue;
        }
        uint64_t hash = 0;
        std::string extra;
        std::istringstream hs(line.substr(pos + 1));
        if (!(hs >> hash) || (hs >> extra) || hash != receipt_journal::Checksum(line.substr(0, pos))) {
            snap.v2_corrupt_tail_ignored = true;
            OutputDebugStringW(L"PersonalIME: v2 WAL checksum failure\n");
            continue;
        }
        line.resize(pos);

        std::istringstream in(line);
        std::string tag;
        Entry e{};
        if (!(in >> tag)) continue;
        RecordKind kind{};
        if (!ParseKindTag(tag, kind)) {
            snap.v2_corrupt_tail_ignored = true;
            continue;
        }
        if (!(in >> e.version >> e.seq >> e.owner_id >> e.session_id >> e.context_id
                 >> e.context_epoch >> e.input_generation >> e.edit_request_id
                 >> e.commit_event_id >> e.channel >> e.text_len >> e.text_digest
                 >> e.disposition >> e.reason_or_stage >> e.hr) || (in >> extra)) {
            snap.v2_corrupt_tail_ignored = true;
            continue;
        }
        if (e.version != 2 || e.commit_event_id == 0) {
            snap.v2_corrupt_tail_ignored = true;
            continue;
        }
        if (e.seq > snap.max_seq) {
            snap.max_seq = e.seq;
        }

        EventState& st = snap.events[e.commit_event_id];
        st.commit_event_id = e.commit_event_id;
        if (st.owner_id == 0) st.owner_id = e.owner_id;
        if (st.session_id == 0) st.session_id = e.session_id;
        if (st.context_id == 0) st.context_id = e.context_id;
        if (st.context_epoch == 0) st.context_epoch = e.context_epoch;
        if (st.input_generation == 0) st.input_generation = e.input_generation;
        if (st.edit_request_id == 0) st.edit_request_id = e.edit_request_id;
        if (e.text_len != 0 && st.text_len == 0) {
            st.text_len = e.text_len;
            st.text_digest = e.text_digest;
        }
        st.last_hr = e.hr;

        if (kind == RecordKind::Prepared) {
            st.has_prepared = true;
            st.channel = e.channel;
        } else if (kind == RecordKind::InjectionAttempt) {
            st.has_injection_attempt = true;
            st.channel = 1;
        } else if (kind == RecordKind::UnknownPending) {
            st.has_unknown_pending = true;
        } else if (kind == RecordKind::Resolved) {
            if (st.has_resolved && st.resolved_disposition != e.disposition) {
                st.conflicted = true;
            } else {
                st.has_resolved = true;
                st.resolved_disposition = e.disposition;
            }
        } else if (kind == RecordKind::ServiceAck) {
            auto isCompatibleAck = [](uint32_t resolvedDisp, uint32_t ackedDisp) -> bool {
                if (resolvedDisp == ackedDisp) return true;
                // Server may legitimately downgrade CancelRestore -> CancelDiscard.
                return resolvedDisp == static_cast<uint32_t>(IpcCommitDisposition::CancelRestore) &&
                       ackedDisp == static_cast<uint32_t>(IpcCommitDisposition::CancelDiscard);
            };
            if (st.has_service_ack && st.acked_disposition != e.disposition) {
                st.conflicted = true;
            } else {
                st.has_service_ack = true;
                st.acked_disposition = e.disposition;
                if (!st.has_resolved) {
                    st.has_resolved = true;
                    st.resolved_disposition = e.disposition;
                } else if (!isCompatibleAck(st.resolved_disposition, e.disposition)) {
                    st.conflicted = true;
                }
            }
        }
    }

    if (ferror(f)) ok = false;
    if (fclose(f) != 0) ok = false;
    return ok;
}

inline int& FailCommitInjectCountForTest() {
    static int s_failCount = 0;
    return s_failCount;
}

inline bool SyncWalFileLocked() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, Path().c_str(), L"a+b") != 0 || !f) {
        return false;
    }
    bool ok = true;
    int& injectFail = FailCommitInjectCountForTest();
    if (injectFail > 0) {
        --injectFail;
        ok = false;
    } else if (fflush(f) != 0 || ferror(f) || _commit(_fileno(f)) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    return ok;
}

inline bool AppendFrameLocked(const std::string& body) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, Path().c_str(), L"a+b") != 0 || !f) {
        return false;
    }
    std::string frame = "\n" + body + " " + std::to_string(receipt_journal::Checksum(body)) + "\n";
    bool ok = (fwrite(frame.data(), 1, frame.size(), f) == frame.size());
    int& injectFail = FailCommitInjectCountForTest();
    if (injectFail > 0) {
        --injectFail;
        ok = false;
    } else if (fflush(f) != 0 || ferror(f) || _commit(_fileno(f)) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    return ok;
}

inline bool LoadSnapshot(Snapshot& out, bool includeLegacy = true) {
    Lock lock;
    if (!lock) return false;
    out = {};
    if (!ReadV2FileLocked(out)) return false;
    if (includeLegacy) {
        receipt_journal::Lock v1Lock;
        if (v1Lock) {
            receipt_journal::Snapshot v1Snap;
            if (receipt_journal::ReadLocked(v1Snap)) {
                for (const auto& [_, rec] : v1Snap.active) {
                    if (out.events.find(rec.eid) == out.events.end()) {
                        out.legacy_records.push_back(rec);
                    }
                }
            }
        }
    }
    return true;
}

inline bool AppendEntry(RecordKind kind, Entry entry, uint64_t* outSeq = nullptr) {
    if (entry.commit_event_id == 0) return false;
    Lock lock;
    if (!lock) return false;

    Snapshot snap;
    if (!ReadV2FileLocked(snap)) return false;

    auto it = snap.events.find(entry.commit_event_id);
    if (it != snap.events.end()) {
        if (it->second.conflicted) {
            return false;
        }
        if (kind == RecordKind::Resolved && it->second.has_resolved) {
            if (it->second.resolved_disposition != entry.disposition) {
                return false; // Immutable terminal disposition conflict
            }
            // Previous _commit may have failed even though the frame is readable:
            // re-sync to disk before reporting durable persistence.
            if (!SyncWalFileLocked()) {
                return false;
            }
            if (outSeq) *outSeq = snap.max_seq;
            return true; // Idempotent & durably synced
        }
        if (kind == RecordKind::ServiceAck && it->second.has_service_ack) {
            if (it->second.acked_disposition != entry.disposition) {
                return false;
            }
            if (!SyncWalFileLocked()) {
                return false;
            }
            if (outSeq) *outSeq = snap.max_seq;
            return true;
        }
    }

    entry.version = 2;
    entry.seq = snap.max_seq + 1;
    if (!AppendFrameLocked(FormatBody(kind, entry))) {
        return false;
    }
    if (outSeq) *outSeq = entry.seq;
    return true;
}

inline bool AppendPrepared(const Entry& e) {
    return AppendEntry(RecordKind::Prepared, e);
}

inline bool AppendInjectionAttempt(const Entry& e) {
    return AppendEntry(RecordKind::InjectionAttempt, e);
}

inline bool AppendUnknownPending(const Entry& e) {
    return AppendEntry(RecordKind::UnknownPending, e);
}

inline bool AppendResolved(const Entry& e) {
    return AppendEntry(RecordKind::Resolved, e);
}

inline bool AppendServiceAck(const Entry& e) {
    return AppendEntry(RecordKind::ServiceAck, e);
}

} // namespace receipt_journal_v2
