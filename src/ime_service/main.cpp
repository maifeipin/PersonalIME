#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include "../common/ime_guids.h"
#include "../common/ipc_protocol.h"
#include "../common/plugin_ipc_codec.h"
#include "../core/engine/ime_engine.h"
#include "../core/strategy/input_coordinator.h"
#include "../core/types.h"
#include "../core/encoding/shuangpin_decoder.h"
#include "admin_server.h"

static ime::ImeEngine g_engine;

// High-precision monotonically unique 64-bit event counter across restarts
static std::atomic<uint64_t> g_nextEventId{0};

static uint64_t InitBaseEventId() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return t;
}

static uint64_t NextEventId() {
    return g_nextEventId.fetch_add(1);
}

// Retrieve the exact Current User SID string at runtime
static std::wstring GetCurrentUserSidString() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return L"";
    }
    DWORD len = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
    if (len == 0) {
        CloseHandle(hToken);
        return L"";
    }
    std::vector<BYTE> buf(len);
    if (!GetTokenInformation(hToken, TokenUser, buf.data(), len, &len)) {
        CloseHandle(hToken);
        return L"";
    }
    CloseHandle(hToken);

    auto* pUser = reinterpret_cast<TOKEN_USER*>(buf.data());
    LPWSTR pSidStr = nullptr;
    if (ConvertSidToStringSidW(pUser->User.Sid, &pSidStr)) {
        std::wstring sid(pSidStr);
        LocalFree(pSidStr);
        return sid;
    }
    return L"";
}

enum class CommitState {
    Pending,
    Committing,
    Recorded,
    Cancelled
};

struct ServerPendingCommit {
    uint64_t event_id = 0;
    uint64_t session_id = 0;
    uint64_t context_id = 0;
    uint64_t connection_epoch = 0;
    uint64_t generation = 0;
    ime::Candidate candidate;
    ime::InputScheme scheme = ime::InputScheme::Wubi86;
    std::string query_code; // The actual query code typed by user (e.g. "a", "xi'an")
    std::string entry_id;
    std::wstring text;
    std::shared_ptr<const ime::InputPolicy> policy_snapshot;
    std::wstring composition_before_commit;
    uint32_t page_before_commit = 0;
    CommitState state = CommitState::Pending;
    int64_t created_at = 0;
};

struct CompletedCommitInfo {
    uint64_t session_id = 0;
    uint64_t context_id = 0;
    CommitState state = CommitState::Recorded;
};

static std::atomic<uint64_t> g_nextConnectionEpoch{1};
static std::mutex g_pendingMutex;
static std::map<uint64_t, ServerPendingCommit> g_pendingCommits;

// Completed commits cache for idempotency with owner tracking
static constexpr size_t MAX_COMPLETED_EVENTS = 4096;
static std::map<uint64_t, CompletedCommitInfo> g_completedEvents;

struct ContextState {
    ime::InputScheme scheme = ime::InputScheme::Wubi86; // Default to 86 Wubi
    bool is_chinese = true;
    bool full_width = false;
    bool chinese_punctuation = true;

    std::wstring composition;
    std::wstring prev_composition; // For rollback
    std::string current_query_code;

    // Stage 4: Input Coordinator integration
    std::shared_ptr<const ime::InputPolicy> policy_snapshot;
    int current_dict_index = 0;
    ime::CoordinatorQueryResult cached_coord_query;

    uint32_t current_page = 0;
    static constexpr uint32_t DEFAULT_PAGE_SIZE = 5;
    uint32_t candidate_version = 0;
    bool has_paged = false; // Tracks if candidate paging occurred

    // Revision and connection epoch to protect against stale/cross-connection cancellations
    uint64_t connection_epoch = 0;
    uint64_t input_generation = 0;
    void AdvanceGeneration() { input_generation++; }
};

static uint32_t ComputeModeFlags(const ContextState& st) {
    uint32_t flags = 0;
    if (st.is_chinese) flags |= IPC_MODE_CHINESE;
    if (st.scheme == ime::InputScheme::Pinyin) flags |= IPC_MODE_PINYIN;
    if (st.full_width) flags |= IPC_MODE_FULL_WIDTH;
    if (st.chinese_punctuation) flags |= IPC_MODE_CHINESE_PUNCT;
    if (st.cached_coord_query.has_next_dictionary) {
        flags |= IPC_STATUS_HAS_NEXT_DICT;
    }
    if (st.cached_coord_query.is_truncated) {
        flags |= IPC_STATUS_TRUNCATED;
    }
    if (st.cached_coord_query.status != ime::CoordinatorStatus::Success &&
        st.cached_coord_query.status != ime::CoordinatorStatus::NoCandidates &&
        st.cached_coord_query.status != ime::CoordinatorStatus::BudgetTruncated) {
        flags |= IPC_STATUS_ERROR;
    }
    // Encode independent scheme_id in bits 16..23 using wire protocol value
    IpcSchemeId wireScheme = InputSchemeToIpcSchemeId(st.scheme);
    flags |= (static_cast<uint32_t>(wireScheme) & 0xFF) << IPC_SCHEME_SHIFT;
    if (g_engine.GetPluginRepo()) {
        uint32_t pluginRev = g_engine.GetPluginRepo()->GetRevision();
        flags |= IPC_STATUS_HAS_PLUGINS_REV;
        flags |= (pluginRev & 0xFFu) << IPC_PLUGINS_REV_SHIFT;
    }
    return flags;
}

static void SafeCopyCommitText(IpcKeyResponse& resp, const std::wstring& text) {
    resp.has_commit = 1;
    size_t len = text.length();
    if (len >= 1024) {
        len = 1023;
        // Never split UTF-16 surrogate pairs
        if (text[len - 1] >= 0xD800 && text[len - 1] <= 0xDBFF) {
            len = 1022;
        }
    }
    wcsncpy_s(resp.commit_text, text.c_str(), len);
    resp.commit_text[len] = L'\0';
    resp.commit_length = static_cast<uint32_t>(len);
}

static void SafeCopyCandidateText(wchar_t* dest, size_t destCap, const std::wstring& src) {
    if (destCap == 0) return;
    size_t len = src.length();
    if (len >= destCap) {
        len = destCap - 1;
        // Never leave a dangling high surrogate at truncated boundary
        if (len > 0 && src[len - 1] >= 0xD800 && src[len - 1] <= 0xDBFF) {
            len--;
        }
    }
    wcsncpy_s(dest, destCap, src.c_str(), len);
    dest[len] = L'\0';
}

static void PopulateCandidatesPage(IpcKeyResponse& resp, ContextState& st) {
    uint32_t total = static_cast<uint32_t>(st.cached_coord_query.candidates.size());
    resp.total_candidates = total;

    uint32_t pageSize = st.policy_snapshot ? st.policy_snapshot->page_size : 5;
    if (pageSize < 5) pageSize = 5;
    if (pageSize > 9) pageSize = 9;

    uint32_t pageCount = (total + pageSize - 1) / pageSize;
    if (pageCount == 0) pageCount = 1;
    resp.page_count = pageCount;

    if (st.current_page >= pageCount) {
        st.current_page = (pageCount > 0) ? (pageCount - 1) : 0;
    }
    resp.page_index = st.current_page;

    uint32_t startIdx = st.current_page * pageSize;
    uint32_t count = 0;
    if (startIdx < total) {
        count = (std::min)(pageSize, total - startIdx);
    }
    resp.candidate_count = count;

    for (uint32_t i = 0; i < count; ++i) {
        const auto& cand = st.cached_coord_query.candidates[startIdx + i];
        std::wstring label = std::to_wstring(i + 1) + L". " + cand.text;
        if (!cand.reverse_lookup_code.empty()) {
            label += L" [" + ime::Utf8ToWide(cand.reverse_lookup_code) + L"]";
        }
        SafeCopyCandidateText(resp.candidates[i], sizeof(resp.candidates[i])/sizeof(wchar_t), label);
    }
}

static void PopulateCandidates(ContextState& st, IpcKeyResponse& resp) {
    resp.has_composition = 1;
    wcsncpy_s(resp.composition, st.composition.c_str(), _TRUNCATE);

    if (st.composition.empty()) {
        st.current_query_code.clear();
        st.cached_coord_query = {};
        st.has_paged = false;
        st.current_dict_index = 0;
        st.policy_snapshot = nullptr;
        resp.candidate_count = 0;
        resp.total_candidates = 0;
        resp.page_index = 0;
        resp.page_count = 0;
        resp.candidate_version = st.candidate_version;
        return;
    }

    std::string code = ime::WideToUtf8(st.composition);
    st.current_query_code = code;

    // Ensure policy snapshot is locked for this composition
    if (!st.policy_snapshot && g_engine.GetCoordinator()) {
        auto base = g_engine.GetCoordinator()->GetPolicySnapshot();
        if (base && !base->IsMethodEnabled(st.scheme)) {
            st.scheme = base->primary_method;
        }
        if (base && base->primary_method != st.scheme) {
            auto customSnapshot = std::make_shared<ime::InputPolicy>(*base);
            customSnapshot->primary_method = st.scheme;
            st.policy_snapshot = customSnapshot;
        } else {
            st.policy_snapshot = base;
        }
    }

    if (g_engine.GetCoordinator()) {
        st.cached_coord_query = g_engine.GetCoordinator()->Query(
            code,
            st.policy_snapshot,
            -1 // Automatic cascading fallback
        );
        st.current_dict_index = st.cached_coord_query.current_dict_index;
    } else {
        st.cached_coord_query = {};
    }

    st.candidate_version++;
    resp.candidate_version = st.candidate_version;

    PopulateCandidatesPage(resp, st);
}

static std::wstring MapChinesePunctuation(uint32_t vk, uint32_t modifiers) {
    bool isShift = (modifiers & 1) != 0;
    if (!isShift) {
        switch (vk) {
            case VK_OEM_COMMA:   return L"，";
            case VK_OEM_PERIOD:  return L"。";
            case VK_OEM_2:       return L"/";
            case VK_OEM_5:       return L"、";
            case VK_OEM_4:       return L"【";
            case VK_OEM_6:       return L"】";
            default: return L"";
        }
    } else {
        switch (vk) {
            case '1':            return L"！";
            case '4':            return L"￥";
            case '6':            return L"……";
            case '9':            return L"（";
            case '0':            return L"）";
            case VK_OEM_1:       return L"：";
            case VK_OEM_2:       return L"？";
            case VK_OEM_COMMA:   return L"《";
            case VK_OEM_PERIOD:  return L"》";
            case VK_OEM_3:       return L"～";
            default: return L"";
        }
    }
}

static std::wstring MapFullWidthChar(uint32_t vk, uint32_t modifiers) {
    bool isShift = (modifiers & 1) != 0;
    if (vk >= '0' && vk <= '9' && !isShift) {
        return std::wstring(1, static_cast<wchar_t>(L'\uFF10' + (vk - '0')));
    }
    if (vk >= 'A' && vk <= 'Z') {
        wchar_t base = isShift ? L'\uFF21' : L'\uFF41';
        return std::wstring(1, static_cast<wchar_t>(base + (vk - 'A')));
    }
    if (vk == VK_SPACE) {
        return L"\u3000";
    }
    return L"";
}

static bool IsVNumberComposition(const std::wstring& comp) {
    if (comp.empty() || (comp[0] != L'v' && comp[0] != L'V')) {
        return false;
    }
    int dotCount = 0;
    for (size_t i = 1; i < comp.size(); ++i) {
        wchar_t c = comp[i];
        if (c >= L'0' && c <= L'9') {
            continue;
        } else if (c == L'.') {
            dotCount++;
            if (dotCount > 1) return false;
        } else {
            return false;
        }
    }
    return true;
}

static void ResetContextCompositionAndState(ContextState& state) {
    state.composition.clear();
    state.prev_composition.clear();
    state.current_query_code.clear();
    state.cached_coord_query = {};
    state.current_page = 0;
    state.current_dict_index = 0;
    state.policy_snapshot = nullptr;
    state.AdvanceGeneration();
    // Do NOT clear g_pendingCommits! Pending commits belong to host document insertion lifecycle.
}

// Handles a single client pipe connection with multi-context isolation
void HandleClient(HANDLE hPipe) {
    uint64_t currentConnEpoch = g_nextConnectionEpoch.fetch_add(1);
    std::map<std::pair<uint64_t, uint64_t>, ContextState> contextStates;
    DWORD bytesRead = 0;
    DWORD bytesWritten = 0;

    std::wcout << L"[ime-service] Client connected to pipe." << std::endl;

    while (true) {
        IpcKeyRequest req{};
        BOOL success = ReadFile(hPipe, &req, sizeof(req), &bytesRead, nullptr);
        if (!success || bytesRead != sizeof(req)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || bytesRead == 0) {
                std::wcout << L"[ime-service] Client disconnected." << std::endl;
            } else {
                std::wcout << L"[ime-service] Pipe read error or invalid length (" << bytesRead << L" bytes): " << err << std::endl;
            }
            break;
        }

        auto contextKey = std::make_pair(req.session_id, req.context_id);
        auto& state = contextStates[contextKey];
        if (state.connection_epoch == 0) {
            state.connection_epoch = currentConnEpoch;
            if (g_engine.GetCoordinator()) {
                auto pol = g_engine.GetCoordinator()->GetPolicySnapshot();
                state.scheme = pol->primary_method;
                state.is_chinese = pol->initial_mode.chinese;
                state.full_width = pol->initial_mode.full_width;
                state.chinese_punctuation = pol->initial_mode.chinese_punctuation;
            }
        }

        IpcKeyResponse resp{};
        resp.msg_type = req.msg_type;
        resp.session_id = req.session_id;
        resp.context_id = req.context_id;
        resp.protocol_version = req.protocol_version;
        resp.status_flags = ComputeModeFlags(state);

        // Strict protocol version validation (accept Stage 3 legacy and Stage 4 modern)
        if (req.protocol_version != IPC_PROTOCOL_VERSION_STAGE3 &&
            req.protocol_version != IPC_PROTOCOL_VERSION_STAGE4) {
            resp.handled = 0;
            WriteFile(hPipe, &resp, sizeof(resp), &bytesWritten, nullptr);
            FlushFileBuffers(hPipe);
            continue;
        }

        if (req.msg_type == static_cast<uint32_t>(IpcMsgType::Ping)) {
            resp.handled = 1;
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::Reset) ||
                 req.msg_type == static_cast<uint32_t>(IpcMsgType::ContextLost)) {
            ResetContextCompositionAndState(state);
            resp.handled = 1;
            resp.has_composition = 1;
            resp.composition[0] = L'\0';
            resp.candidate_count = 0;
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::Rollback)) {
            state.composition = state.prev_composition;
            state.current_page = 0;
            state.has_paged = false;
            state.AdvanceGeneration();
            PopulateCandidates(state, resp);
            resp.handled = 1;
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::CommitSuccess)) {
            if (req.event_id != 0) {
                std::unique_lock<std::mutex> lock(g_pendingMutex);
                auto it = g_pendingCommits.find(req.event_id);
                if (it != g_pendingCommits.end()) {
                    if (it->second.session_id != req.session_id || it->second.context_id != req.context_id) {
                        // Strict ownership check: wrong context cannot confirm
                        resp.handled = 0;
                    } else if (it->second.state == CommitState::Cancelled) {
                        resp.handled = 0;
                    } else if (it->second.state == CommitState::Recorded) {
                        resp.handled = 1;
                    } else {
                        it->second.state = CommitState::Committing;
                        auto pc = it->second;
                        lock.unlock();

                        ime::Candidate c = pc.candidate;
                        if (c.entry_id.empty()) {
                            c.entry_id = pc.entry_id;
                            c.scheme = pc.scheme;
                            c.matched_code = pc.query_code;
                            c.text = pc.text;
                        }

                        bool ok = false;
                        if (g_engine.GetCoordinator()) {
                            ok = g_engine.GetCoordinator()->CommitCandidate(
                                c,
                                std::to_string(req.event_id),
                                pc.session_id,
                                pc.context_id,
                                pc.policy_snapshot
                            );
                        } else {
                            ok = g_engine.CommitCandidate(
                                std::to_string(req.event_id),
                                pc.session_id,
                                pc.context_id,
                                pc.scheme,
                                pc.query_code,
                                pc.entry_id
                            );
                        }

                        lock.lock();
                        it = g_pendingCommits.find(req.event_id);
                        if (ok) {
                            if (it != g_pendingCommits.end()) {
                                g_pendingCommits.erase(it);
                            }
                            g_completedEvents[req.event_id] = { req.session_id, req.context_id, CommitState::Recorded };
                            if (g_completedEvents.size() > MAX_COMPLETED_EVENTS) {
                                g_completedEvents.erase(g_completedEvents.begin());
                            }
                            resp.handled = 1;
                        } else {
                            // SQLite failed: revert back to Pending for retry or cancellation
                            if (it != g_pendingCommits.end()) {
                                it->second.state = CommitState::Pending;
                            }
                            resp.handled = 0;
                        }
                    }
                } else {
                    // Check if already completed (replay idempotency with ownership validation)
                    auto cit = g_completedEvents.find(req.event_id);
                    if (cit != g_completedEvents.end()) {
                        if (cit->second.session_id == req.session_id && cit->second.context_id == req.context_id && cit->second.state == CommitState::Recorded) {
                            resp.handled = 1;
                        } else {
                            resp.handled = 0;
                        }
                    } else {
                        lock.unlock();
                        auto info = g_engine.GetCommitEventInfo(std::to_string(req.event_id));
                        lock.lock();
                        if (info.status == 1 && info.session_id == req.session_id && info.context_id == req.context_id) {
                            g_completedEvents[req.event_id] = { req.session_id, req.context_id, CommitState::Recorded };
                            resp.handled = 1;
                        } else {
                            resp.handled = 0;
                        }
                    }
                }
            } else {
                resp.handled = 0;
            }
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::CommitCancel)) {
            if (req.event_id != 0) {
                std::unique_lock<std::mutex> lock(g_pendingMutex);
                auto it = g_pendingCommits.find(req.event_id);
                if (it != g_pendingCommits.end()) {
                    if (it->second.session_id != req.session_id || it->second.context_id != req.context_id) {
                        // Strict ownership check: wrong context cannot cancel
                        resp.handled = 0;
                        resp.has_composition = 0;
                        resp.composition[0] = L'\0';
                        resp.candidate_count = 0;
                    } else if (it->second.state == CommitState::Committing || it->second.state == CommitState::Recorded) {
                        // In-flight SQLite write or already recorded: cancel loses the race
                        resp.handled = 0;
                        resp.has_composition = 0;
                        resp.composition[0] = L'\0';
                        resp.candidate_count = 0;
                    } else {
                        auto pc = it->second;
                        lock.unlock();

                        // Persist cancellation to SQLite: delete from pending_commits and record Cancelled status
                        bool ok = g_engine.RecordCancel(std::to_string(req.event_id), req.session_id, req.context_id);

                        lock.lock();
                        if (!ok) {
                            // Durable cancellation failed: do NOT acknowledge and keep pending for retry
                            resp.handled = 0;
                            resp.has_composition = 0;
                            resp.composition[0] = L'\0';
                            resp.candidate_count = 0;
                        } else {
                            it = g_pendingCommits.find(req.event_id);
                            if (it != g_pendingCommits.end()) {
                                g_pendingCommits.erase(it);
                            }
                            g_completedEvents[req.event_id] = { req.session_id, req.context_id, CommitState::Cancelled };
                            if (g_completedEvents.size() > MAX_COMPLETED_EVENTS) {
                                g_completedEvents.erase(g_completedEvents.begin());
                            }

                            // Only restore composition if connection epoch, generation and empty buffer match!
                            if (state.connection_epoch == pc.connection_epoch &&
                                state.input_generation == pc.generation &&
                                state.composition.empty()) {
                                state.composition = pc.composition_before_commit;
                                state.current_page = pc.page_before_commit;
                                PopulateCandidates(state, resp);
                                state.AdvanceGeneration();
                            } else {
                                resp.has_composition = 0;
                                resp.composition[0] = L'\0';
                                resp.candidate_count = 0;
                            }
                            resp.handled = 1;
                        }
                    }
                } else {
                    auto cit = g_completedEvents.find(req.event_id);
                    if (cit != g_completedEvents.end()) {
                        if (cit->second.session_id == req.session_id && cit->second.context_id == req.context_id && cit->second.state == CommitState::Cancelled) {
                            resp.handled = 1;
                        } else {
                            resp.handled = 0;
                        }
                    } else {
                        lock.unlock();
                        auto info = g_engine.GetCommitEventInfo(std::to_string(req.event_id));
                        lock.lock();
                        if (info.status == 2 && info.session_id == req.session_id && info.context_id == req.context_id) {
                            g_completedEvents[req.event_id] = { req.session_id, req.context_id, CommitState::Cancelled };
                            resp.handled = 1;
                        } else {
                            resp.handled = 0;
                        }
                    }
                    resp.has_composition = 0;
                    resp.composition[0] = L'\0';
                    resp.candidate_count = 0;
                }
            } else {
                resp.handled = 0;
            }
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::SetMode)) {
            // Validate update_mask: unknown mask bits are rejected under Stage 4
            constexpr uint32_t VALID_UPDATE_MASKS = IPC_UPDATE_CHINESE | IPC_UPDATE_FULL_WIDTH | IPC_UPDATE_CHINESE_PUNCT | IPC_UPDATE_SCHEME;
            if (req.protocol_version >= IPC_PROTOCOL_VERSION_STAGE4 && (req.update_mask & ~VALID_UPDATE_MASKS) != 0) {
                resp.handled = 0;
                resp.status_flags = ComputeModeFlags(state);
                WriteFile(hPipe, &resp, sizeof(resp), &bytesWritten, nullptr);
                FlushFileBuffers(hPipe);
                continue;
            }

            std::shared_ptr<const ime::InputPolicy> pol;
            if (g_engine.GetCoordinator()) {
                pol = g_engine.GetCoordinator()->GetPolicySnapshot();
            }

            bool new_is_chinese = state.is_chinese;
            bool new_full_width = state.full_width;
            bool new_chinese_punct = state.chinese_punctuation;
            ime::InputScheme new_scheme = state.scheme;

            if (req.protocol_version >= IPC_PROTOCOL_VERSION_STAGE4) {
                // Check if scheme update is requested
                if ((req.update_mask & IPC_UPDATE_SCHEME) || req.scheme_id != 0) {
                    uint32_t sId = req.scheme_id != 0 ? req.scheme_id : ((req.mode_arg & IPC_SCHEME_MASK) >> IPC_SCHEME_SHIFT);
                    ime::InputScheme targetScheme;
                    if (!IpcSchemeIdToInputScheme(sId, targetScheme) ||
                        (targetScheme != ime::InputScheme::Wubi86 &&
                         targetScheme != ime::InputScheme::Pinyin &&
                         targetScheme != ime::InputScheme::Shuangpin)) {
                        resp.handled = 0;
                        resp.status_flags = ComputeModeFlags(state);
                        WriteFile(hPipe, &resp, sizeof(resp), &bytesWritten, nullptr);
                        FlushFileBuffers(hPipe);
                        continue;
                    }
                    if (pol && !pol->IsMethodEnabled(targetScheme)) {
                        // Explicit selection of disabled scheme MUST be rejected with zero modification!
                        resp.handled = 0;
                        resp.status_flags = ComputeModeFlags(state);
                        WriteFile(hPipe, &resp, sizeof(resp), &bytesWritten, nullptr);
                        FlushFileBuffers(hPipe);
                        continue;
                    }
                    new_scheme = targetScheme;
                }

                // Mode flags update:
                if (req.update_mask == 0) {
                    // Whole-set mode flags update from mode_arg
                    new_is_chinese = (req.mode_arg & IPC_MODE_CHINESE) != 0;
                    new_full_width = (req.mode_arg & IPC_MODE_FULL_WIDTH) != 0;
                    new_chinese_punct = (req.mode_arg & IPC_MODE_CHINESE_PUNCT) != 0;
                } else {
                    // Selective mode update based on specified mask bits
                    if (req.update_mask & IPC_UPDATE_CHINESE) {
                        new_is_chinese = (req.mode_arg & IPC_MODE_CHINESE) != 0;
                    }
                    if (req.update_mask & IPC_UPDATE_FULL_WIDTH) {
                        new_full_width = (req.mode_arg & IPC_MODE_FULL_WIDTH) != 0;
                    }
                    if (req.update_mask & IPC_UPDATE_CHINESE_PUNCT) {
                        new_chinese_punct = (req.mode_arg & IPC_MODE_CHINESE_PUNCT) != 0;
                    }
                }
            } else {
                // Legacy Stage 3 (0x0300) compatibility branch:
                new_is_chinese = (req.mode_arg & IPC_MODE_CHINESE) != 0;
                new_full_width = (req.mode_arg & IPC_MODE_FULL_WIDTH) != 0;
                new_chinese_punct = (req.mode_arg & IPC_MODE_CHINESE_PUNCT) != 0;

                // Legacy binary choice: if context is currently in Shuangpin, SetMode NEVER changes scheme!
                if (state.scheme != ime::InputScheme::Shuangpin) {
                    if (req.mode_arg & IPC_MODE_PINYIN) {
                        new_scheme = ime::InputScheme::Pinyin;
                    } else if (req.mode_arg & IPC_MODE_CHINESE) {
                        new_scheme = ime::InputScheme::Wubi86;
                    }
                    if (pol && !pol->IsMethodEnabled(new_scheme)) {
                        new_scheme = pol->primary_method;
                    }
                }
            }

            // All validations passed: apply changes atomically
            bool switching_to_english = (state.is_chinese && !new_is_chinese);
            bool mode_or_scheme_changed = (state.is_chinese != new_is_chinese ||
                                           state.scheme != new_scheme ||
                                           state.full_width != new_full_width ||
                                           state.chinese_punctuation != new_chinese_punct);

            if (switching_to_english && !state.composition.empty()) {
                SafeCopyCommitText(resp, state.composition);
                resp.commit_event_id = 0;
            }

            state.is_chinese = new_is_chinese;
            state.full_width = new_full_width;
            state.chinese_punctuation = new_chinese_punct;
            state.scheme = new_scheme;

            if (!state.composition.empty() || mode_or_scheme_changed) {
                ResetContextCompositionAndState(state);
                resp.has_composition = 1;
                resp.composition[0] = L'\0';
                resp.candidate_count = 0;
            }

            resp.status_flags = ComputeModeFlags(state);
            resp.handled = 1;
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::SetScheme)) {
            // Stage 4 command: explicitly reject under legacy protocol versions
            if (req.protocol_version < IPC_PROTOCOL_VERSION_STAGE4) {
                resp.handled = 0;
                resp.status_flags = ComputeModeFlags(state);
                WriteFile(hPipe, &resp, sizeof(resp), &bytesWritten, nullptr);
                FlushFileBuffers(hPipe);
                continue;
            }

            uint32_t sId = req.scheme_id != 0 ? req.scheme_id : req.mode_arg;
            ime::InputScheme targetScheme;
            if (!IpcSchemeIdToInputScheme(sId, targetScheme) ||
                (targetScheme != ime::InputScheme::Wubi86 &&
                 targetScheme != ime::InputScheme::Pinyin &&
                 targetScheme != ime::InputScheme::Shuangpin)) {
                resp.handled = 0;
                resp.status_flags = ComputeModeFlags(state);
                WriteFile(hPipe, &resp, sizeof(resp), &bytesWritten, nullptr);
                FlushFileBuffers(hPipe);
                continue;
            }

            std::shared_ptr<const ime::InputPolicy> pol;
            if (g_engine.GetCoordinator()) {
                pol = g_engine.GetCoordinator()->GetPolicySnapshot();
            }

            if (pol && !pol->IsMethodEnabled(targetScheme)) {
                // Explicit selection of disabled scheme MUST be rejected and must not modify context!
                resp.handled = 0;
                resp.status_flags = ComputeModeFlags(state);
                WriteFile(hPipe, &resp, sizeof(resp), &bytesWritten, nullptr);
                FlushFileBuffers(hPipe);
                continue;
            }

            if (state.scheme != targetScheme || !state.composition.empty()) {
                state.scheme = targetScheme;
                ResetContextCompositionAndState(state);
                resp.has_composition = 1;
                resp.composition[0] = L'\0';
                resp.candidate_count = 0;
            }

            resp.status_flags = ComputeModeFlags(state);
            resp.handled = 1;
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::GetScheme)) {
            if (req.protocol_version < IPC_PROTOCOL_VERSION_STAGE4) {
                resp.handled = 0;
                resp.status_flags = ComputeModeFlags(state);
                WriteFile(hPipe, &resp, sizeof(resp), &bytesWritten, nullptr);
                FlushFileBuffers(hPipe);
                continue;
            }
            resp.status_flags = ComputeModeFlags(state);
            resp.handled = 1;
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::GetMode)) {
            resp.status_flags = ComputeModeFlags(state);
            resp.handled = 1;
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::OpenManager)) {
            HWND hMgr = FindWindowW(nullptr, L"人人 设置");
            if (!hMgr) {
                hMgr = FindWindowW(nullptr, L"PersonalIME 输入法设置");
            }
            if (!hMgr) {
                hMgr = FindWindowW(nullptr, L"PersonalIME 管理器");
            }
            if (hMgr) {
                if (IsIconic(hMgr)) {
                    ShowWindowAsync(hMgr, SW_RESTORE);
                } else {
                    ShowWindowAsync(hMgr, SW_SHOW);
                }
                SetForegroundWindow(hMgr);
                resp.handled = 1;
            } else {
                std::wstring managerPath;
                char envBuf[MAX_PATH];
                DWORD envLen = GetEnvironmentVariableA("PERSONALIME_MANAGER_PATH", envBuf, MAX_PATH);
                if (envLen > 0 && envLen < MAX_PATH) {
                    std::error_code ec;
                    if (std::filesystem::exists(envBuf, ec)) {
                        managerPath = std::filesystem::path(envBuf).wstring();
                    }
                }
                if (managerPath.empty()) {
                    wchar_t exeBuf[MAX_PATH];
                    if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH) > 0) {
                        std::filesystem::path servicePath(exeBuf);
                        std::filesystem::path serviceDir = servicePath.parent_path();
                        std::vector<std::filesystem::path> candPaths = {
                            serviceDir / L"PersonalIME.Manager.exe",
                            serviceDir / L"manager" / L"PersonalIME.Manager.exe",
                            serviceDir / L"../../src/manager/PersonalIME.Manager/bin/Release/net10.0-windows/PersonalIME.Manager.exe",
                            serviceDir / L"../../out/publish/manager/PersonalIME.Manager.exe",
                            L"C:\\Program Files\\PersonalIME\\manager\\PersonalIME.Manager.exe"
                        };
                        for (const auto& p : candPaths) {
                            std::error_code ec;
                            if (std::filesystem::exists(p, ec)) {
                                managerPath = p.wstring();
                                break;
                            }
                        }
                    }
                }
                if (!managerPath.empty()) {
                    std::wstring targetPath = managerPath;
                    std::thread([targetPath]() {
                        ShellExecuteW(nullptr, L"open", targetPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }).detach();
                    resp.handled = 1;
                } else {
                    resp.handled = 0;
                }
            }
            resp.status_flags = ComputeModeFlags(state);
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::ExecutePlugin)) {
            std::string outMsg;
            bool ok = false;
            ime::PluginExecStatus execStatus = ime::PluginExecStatus::LaunchFailed;
            if (g_engine.GetPluginRepo()) {
                ok = g_engine.GetPluginRepo()->ExecutePlugin(req.vk_code, outMsg, &execStatus);
                resp.candidate_version = g_engine.GetPluginRepo()->GetRevision();
            }
            resp.page_index = static_cast<uint32_t>(execStatus);
            SafeCopyCommitText(resp, ime::Utf8ToWide(outMsg));
            resp.handled = ok ? 1 : 0;
            resp.status_flags = ComputeModeFlags(state);
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::GetPlugins)) {
            resp.status_flags = ComputeModeFlags(state);
            if (g_engine.GetPluginRepo()) {
                std::vector<ime::PluginRecord> plugins;
                if (g_engine.GetPluginRepo()->GetAllPlugins(plugins, true)) {
                    ime::EncodePluginsToIpcResponse(plugins, g_engine.GetPluginRepo()->GetRevision(), resp);
                }
            }
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::NextDictionary)) {
            if (state.cached_coord_query.has_next_dictionary && g_engine.GetCoordinator()) {
                auto qNext = g_engine.GetCoordinator()->QueryNextDictionary(
                    state.current_query_code,
                    state.current_dict_index,
                    state.policy_snapshot
                );
                state.current_dict_index = qNext.current_dict_index;
                state.cached_coord_query = qNext;
                state.current_page = 0;
                state.has_paged = false;
                state.AdvanceGeneration();

                resp.total_candidates = static_cast<uint32_t>(qNext.candidates.size());
                PopulateCandidatesPage(resp, state);
                resp.has_composition = 1;
                wcsncpy_s(resp.composition, state.composition.c_str(), _TRUNCATE);
                state.candidate_version++;
                resp.candidate_version = state.candidate_version;
                resp.handled = 1;
            } else {
                resp.handled = 0;
            }
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::TestKeyDown)) {
            uint32_t vk = req.vk_code;
            uint32_t mods = req.modifiers;

            // Shortcut checks
            if ((mods & 2) && (vk == VK_SHIFT || vk == VK_SPACE || vk == VK_OEM_PERIOD)) {
                resp.handled = 1;
            }
            else if ((mods & 1) && vk == VK_SPACE) {
                resp.handled = 1; // Shift + Space (Full/Half width)
            }
            else if (vk == VK_CAPITAL && (mods & ~1u) == 0) {
                resp.handled = 1; // CapsLock (Switch Chinese / English)
            }
            else if (vk == VK_SHIFT) {
                // Standalone Shift down: DO NOT eat in TestKeyDown so host key state machine is intact
                resp.handled = 0;
            }
            else if (state.is_chinese) {
                if (state.composition.empty()) {
                    if ((vk >= 'A' && vk <= 'Z') || vk == VK_OEM_1) {
                        resp.handled = 1;
                    } else if (state.full_width && !MapFullWidthChar(vk, mods).empty()) {
                        resp.handled = 1;
                    } else if (state.chinese_punctuation && !MapChinesePunctuation(vk, mods).empty()) {
                        resp.handled = 1;
                    } else {
                        resp.handled = 0;
                    }
                } else {
                    // Active composition
                    uint32_t pageSize = state.policy_snapshot ? state.policy_snapshot->page_size : 5;
                    if (pageSize < 5) pageSize = 5; if (pageSize > 9) pageSize = 9;
                    bool inVNumber = IsVNumberComposition(state.composition);

                    if ((vk >= 'A' && vk <= 'Z') || (vk >= '1' && vk <= ('0' + pageSize)) ||
                        (inVNumber && (mods & 1) == 0 && ((vk >= '0' && vk <= '9') || (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) || vk == VK_DECIMAL)) ||
                        vk == VK_SPACE || vk == VK_BACK || vk == VK_ESCAPE || vk == VK_RETURN ||
                        vk == VK_OEM_7 || vk == VK_OEM_1 || vk == VK_NEXT || vk == VK_PRIOR ||
                        vk == VK_OEM_PERIOD || vk == VK_OEM_COMMA || vk == VK_OEM_PLUS || vk == VK_OEM_MINUS ||
                        (vk == VK_TAB && state.cached_coord_query.has_next_dictionary)) {
                        resp.handled = 1;
                    } else {
                        // Do NOT eat navigation/arrow keys (VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, etc.)!
                        resp.handled = 0;
                    }
                }
            } else {
                // English mode
                if (state.full_width && !MapFullWidthChar(vk, mods).empty()) {
                    resp.handled = 1;
                } else if (!state.full_width && (mods & (2 | 4 | 8)) == 0 && (vk >= 'A' && vk <= 'Z')) {
                    resp.handled = 1;
                } else {
                    resp.handled = 0;
                }
            }
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::KeyDown)) {
            uint32_t vk = req.vk_code;
            uint32_t mods = req.modifiers;

            // 1. Hotkey Handling
            if ((mods & 2) && vk == VK_SHIFT) {
                // Ctrl + Shift: toggle / cycle through enabled schemes
                std::shared_ptr<const ime::InputPolicy> pol;
                if (g_engine.GetCoordinator()) {
                    pol = g_engine.GetCoordinator()->GetPolicySnapshot();
                }
                std::vector<ime::InputScheme> enabledMethods;
                if (pol) {
                    enabledMethods = pol->GetEnabledInputMethods();
                }
                if (enabledMethods.empty()) {
                    enabledMethods = { pol ? pol->primary_method : ime::InputScheme::Wubi86 };
                }

                auto it = std::find(enabledMethods.begin(), enabledMethods.end(), state.scheme);
                if (it == enabledMethods.end() || enabledMethods.size() <= 1) {
                    state.scheme = enabledMethods.front();
                } else {
                    size_t nextIdx = (std::distance(enabledMethods.begin(), it) + 1) % enabledMethods.size();
                    state.scheme = enabledMethods[nextIdx];
                }

                ResetContextCompositionAndState(state);
                resp.status_flags = ComputeModeFlags(state);
                resp.has_composition = 1;
                resp.composition[0] = L'\0';
                resp.handled = 1;
            }
            else if ((mods & 2) && vk == VK_SPACE) {
                // Ctrl + Space: toggle Chinese / English (commit uncommitted raw composition when switching to English)
                if (state.is_chinese && !state.composition.empty()) {
                    SafeCopyCommitText(resp, state.composition);
                    resp.commit_event_id = 0;
                }
                state.is_chinese = !state.is_chinese;
                ResetContextCompositionAndState(state);
                resp.status_flags = ComputeModeFlags(state);
                resp.has_composition = 1;
                resp.composition[0] = L'\0';
                resp.candidate_count = 0;
                resp.handled = 1;
            }
            else if (vk == VK_CAPITAL && (mods & ~1u) == 0) {
                // CapsLock: switch to English (committing any uncommitted raw composition) or toggle back to Chinese
                if (state.is_chinese && !state.composition.empty()) {
                    SafeCopyCommitText(resp, state.composition);
                    resp.commit_event_id = 0;
                }
                state.is_chinese = !state.is_chinese;
                ResetContextCompositionAndState(state);
                resp.status_flags = ComputeModeFlags(state);
                resp.has_composition = 1;
                resp.composition[0] = L'\0';
                resp.candidate_count = 0;
                resp.handled = 1;
            }
            else if (vk == VK_SHIFT && mods == 1) {
                // Standalone Shift down: DO NOT toggle or clear composition on KeyDown!
                // Passing through ensures typing Shift+character does not destroy existing composition.
                resp.handled = 0;
            }
            else if ((mods & 1) && vk == VK_SPACE) {
                // Shift + Space: toggle Full-width / Half-width
                state.full_width = !state.full_width;
                resp.status_flags = ComputeModeFlags(state);
                resp.handled = 1;
            }
            else if ((mods & 2) && vk == VK_OEM_PERIOD) {
                // Ctrl + .: toggle Chinese / English punctuation
                state.chinese_punctuation = !state.chinese_punctuation;
                resp.status_flags = ComputeModeFlags(state);
                resp.handled = 1;
            }
            // 2. English Mode
            else if (!state.is_chinese) {
                if (state.full_width) {
                    std::wstring fullChar = MapFullWidthChar(vk, mods);
                    if (!fullChar.empty()) {
                        SafeCopyCommitText(resp, fullChar);
                        resp.handled = 1;
                    } else {
                        resp.handled = 0;
                    }
                } else if ((mods & (2 | 4 | 8)) == 0 && (vk >= 'A' && vk <= 'Z')) {
                    // English half-width mode:
                    // Commit LOWERCASE by default; require Shift for UPPERCASE!
                    bool isShift = (mods & 1) != 0;
                    wchar_t ch = isShift ? static_cast<wchar_t>(vk) : static_cast<wchar_t>(std::tolower(static_cast<char>(vk)));
                    SafeCopyCommitText(resp, std::wstring(1, ch));
                    resp.commit_event_id = 0;
                    resp.handled = 1;
                } else {
                    resp.handled = 0;
                }
            }
            // 3. Chinese Mode
            else {
                if (state.composition.empty()) {
                    if (vk >= 'A' && vk <= 'Z') {
                        state.prev_composition = state.composition;
                        wchar_t ch = static_cast<wchar_t>(L'a' + (vk - 'A'));
                        state.composition.push_back(ch);
                        state.current_page = 0;
                        state.AdvanceGeneration();
                        PopulateCandidates(state, resp);
                        resp.handled = 1;
                    }
                    else if (vk == VK_OEM_1) {
                        // Semicolon ';' starts custom phrase (e.g. ;dz)
                        state.prev_composition = state.composition;
                        state.composition.push_back(L';');
                        state.current_page = 0;
                        state.AdvanceGeneration();
                        PopulateCandidates(state, resp);
                        resp.handled = 1;
                    }
                    else if (state.full_width && !MapFullWidthChar(vk, mods).empty()) {
                        SafeCopyCommitText(resp, MapFullWidthChar(vk, mods));
                        resp.handled = 1;
                    }
                    else if (state.chinese_punctuation) {
                        std::wstring punct = MapChinesePunctuation(vk, mods);
                        if (!punct.empty()) {
                            SafeCopyCommitText(resp, punct);
                            resp.handled = 1;
                        } else {
                            resp.handled = 0;
                        }
                    }
                    else {
                        resp.handled = 0;
                    }
                }
                else {
                    // Active composition non-empty
                    state.prev_composition = state.composition;

                    if (vk >= 'A' && vk <= 'Z') {
                        if (state.composition.length() < 127) {
                            wchar_t ch = static_cast<wchar_t>(L'a' + (vk - 'A'));
                            state.composition.push_back(ch);
                            state.current_page = 0;
                            state.has_paged = false;
                            state.AdvanceGeneration();
                            PopulateCandidates(state, resp);
                        }
                        resp.handled = 1;
                    }
                    else if (vk == VK_OEM_7) {
                        // Apostrophe in Pinyin or Shuangpin explicit syllable separation (e.g. xi'an)
                        if ((state.scheme == ime::InputScheme::Pinyin || state.scheme == ime::InputScheme::Shuangpin) && state.composition.length() < 127) {
                            state.composition.push_back(L'\'');
                            state.current_page = 0;
                            state.has_paged = false;
                            state.AdvanceGeneration();
                            PopulateCandidates(state, resp);
                            resp.handled = 1;
                        } else {
                            resp.handled = 1;
                        }
                    }
                    else if (vk == VK_OEM_1) {
                        // Semicolon in Shuangpin (e.g. Microsoft layout uses ';' for 'ing') or if custom phrase started with ';'
                        if ((state.scheme == ime::InputScheme::Shuangpin || (!state.composition.empty() && state.composition.front() == L';')) && state.composition.length() < 127) {
                            state.composition.push_back(L';');
                            state.current_page = 0;
                            state.has_paged = false;
                            state.AdvanceGeneration();
                            PopulateCandidates(state, resp);
                        }
                        resp.handled = 1;
                    }
                    else if (IsVNumberComposition(state.composition) &&
                             (req.mode_arg & IPC_KEYDOWN_EXPLICIT_SELECT) == 0 &&
                             (mods & 1) == 0 &&
                             ((vk >= '0' && vk <= '9') || (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9))) {
                        if (state.composition.length() < 32) {
                            wchar_t digit = (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
                                ? static_cast<wchar_t>(L'0' + (vk - VK_NUMPAD0))
                                : static_cast<wchar_t>(L'0' + (vk - '0'));
                            state.composition.push_back(digit);
                            state.current_page = 0;
                            state.has_paged = false;
                            state.AdvanceGeneration();
                            PopulateCandidates(state, resp);
                        }
                        resp.handled = 1;
                    }
                    else if (IsVNumberComposition(state.composition) &&
                             state.composition.length() >= 2 &&
                             state.composition.find(L'.') == std::wstring::npos &&
                             (mods & 1) == 0 &&
                             (vk == VK_OEM_PERIOD || vk == VK_DECIMAL)) {
                        if (state.composition.length() < 32) {
                            state.composition.push_back(L'.');
                            state.current_page = 0;
                            state.has_paged = false;
                            state.AdvanceGeneration();
                            PopulateCandidates(state, resp);
                        }
                        resp.handled = 1;
                    }
                    else if (vk >= '1' && vk <= static_cast<uint32_t>('0' + (state.policy_snapshot ? (std::min)(9u, (std::max)(5u, state.policy_snapshot->page_size)) : 5u))) {
                        uint32_t pageSize = state.policy_snapshot ? state.policy_snapshot->page_size : 5;
                        if (pageSize < 5) pageSize = 5; if (pageSize > 9) pageSize = 9;

                        bool staleVersion = false;
                        if (!state.cached_coord_query.candidates.empty()) {
                            if (req.candidate_version != state.candidate_version) {
                                staleVersion = true;
                            }
                        }

                        if (staleVersion) {
                            resp.handled = 0; // Reject stale candidate snapshot selection
                        } else {
                            uint32_t candIdx = state.current_page * pageSize + (vk - '1');
                            if (candIdx < state.cached_coord_query.candidates.size()) {
                                const auto& cand = state.cached_coord_query.candidates[candIdx];
                                uint64_t evId = NextEventId();

                                bool learningOn = state.policy_snapshot ? state.policy_snapshot->learning_enabled : true;
                                bool saved = g_engine.SavePendingCommit(
                                    std::to_string(evId),
                                    req.session_id,
                                    req.context_id,
                                    cand.scheme,
                                    state.current_query_code,
                                    cand.entry_id,
                                    cand.text,
                                    learningOn,
                                    cand.layout_id,
                                    cand.matched_code,
                                    cand.resolved_code
                                );

                                if (!saved) {
                                    // Durable persistence failed! Do NOT publish commit response!
                                    resp.handled = 0;
                                    resp.has_commit = 0;
                                    resp.commit_event_id = 0;
                                    resp.commit_text[0] = L'\0';
                                    resp.commit_length = 0;
                                } else {
                                    state.AdvanceGeneration();
                                    ServerPendingCommit pc;
                                    pc.event_id = evId;
                                    pc.session_id = req.session_id;
                                    pc.context_id = req.context_id;
                                    pc.connection_epoch = state.connection_epoch;
                                    pc.generation = state.input_generation;
                                    pc.candidate = cand;
                                    pc.scheme = cand.scheme;
                                    pc.query_code = state.current_query_code;
                                    pc.entry_id = cand.entry_id;
                                    pc.text = cand.text;
                                    pc.policy_snapshot = state.policy_snapshot;
                                    pc.composition_before_commit = state.composition;
                                    pc.page_before_commit = state.current_page;
                                    pc.state = CommitState::Pending;
                                    pc.created_at = g_engine.GetClock() ? g_engine.GetClock()->Now() : 0;
                                    {
                                        std::lock_guard<std::mutex> lock(g_pendingMutex);
                                        g_pendingCommits[evId] = pc;
                                    }

                                    SafeCopyCommitText(resp, cand.text);
                                    resp.commit_event_id = evId;
                                    state.composition.clear();
                                    state.cached_coord_query = {};
                                    state.current_dict_index = 0;
                                    state.policy_snapshot = nullptr;
                                    state.has_paged = false;
                                    resp.has_composition = 1;
                                    resp.composition[0] = L'\0';
                                    resp.candidate_count = 0;
                                    resp.handled = 1;
                                }
                            } else {
                                resp.handled = 1;
                            }
                        }
                    }
                    else if (vk == VK_SPACE) {
                        uint32_t pageSize = state.policy_snapshot ? state.policy_snapshot->page_size : 5;
                        if (pageSize < 5) pageSize = 5; if (pageSize > 9) pageSize = 9;

                        bool staleVersion = false;
                        if (!state.cached_coord_query.candidates.empty()) {
                            if (req.candidate_version != state.candidate_version) {
                                staleVersion = true;
                            }
                        }

                        if (staleVersion) {
                            resp.handled = 0; // Reject stale candidate snapshot selection
                        } else {
                            if (!state.cached_coord_query.candidates.empty()) {
                                uint32_t candIdx = state.current_page * pageSize;
                                if (candIdx < state.cached_coord_query.candidates.size()) {
                                    const auto& cand = state.cached_coord_query.candidates[candIdx];
                                    uint64_t evId = NextEventId();

                                    bool learningOn = state.policy_snapshot ? state.policy_snapshot->learning_enabled : true;
                                    bool saved = g_engine.SavePendingCommit(
                                        std::to_string(evId),
                                        req.session_id,
                                        req.context_id,
                                        cand.scheme,
                                        state.current_query_code,
                                        cand.entry_id,
                                        cand.text,
                                        learningOn,
                                        cand.layout_id,
                                        cand.matched_code,
                                        cand.resolved_code
                                    );

                                    if (!saved) {
                                        resp.handled = 0;
                                        resp.has_commit = 0;
                                        resp.commit_event_id = 0;
                                        resp.commit_text[0] = L'\0';
                                        resp.commit_length = 0;
                                    } else {
                                        state.AdvanceGeneration();
                                        ServerPendingCommit pc;
                                        pc.event_id = evId;
                                        pc.session_id = req.session_id;
                                        pc.context_id = req.context_id;
                                        pc.connection_epoch = state.connection_epoch;
                                        pc.generation = state.input_generation;
                                        pc.candidate = cand;
                                        pc.scheme = cand.scheme;
                                        pc.query_code = state.current_query_code;
                                        pc.entry_id = cand.entry_id;
                                        pc.text = cand.text;
                                        pc.policy_snapshot = state.policy_snapshot;
                                        pc.composition_before_commit = state.composition;
                                        pc.page_before_commit = state.current_page;
                                        pc.state = CommitState::Pending;
                                        pc.created_at = g_engine.GetClock() ? g_engine.GetClock()->Now() : 0;
                                        {
                                            std::lock_guard<std::mutex> lock(g_pendingMutex);
                                            g_pendingCommits[evId] = pc;
                                        }

                                        SafeCopyCommitText(resp, cand.text);
                                        resp.commit_event_id = evId;
                                        state.composition.clear();
                                        state.cached_coord_query = {};
                                        state.current_dict_index = 0;
                                        state.policy_snapshot = nullptr;
                                        state.has_paged = false;
                                        resp.has_composition = 1;
                                        resp.composition[0] = L'\0';
                                        resp.candidate_count = 0;
                                        resp.handled = 1;
                                    }
                                } else {
                                    SafeCopyCommitText(resp, state.composition);
                                    resp.commit_event_id = 0;
                                    state.AdvanceGeneration();
                                    state.composition.clear();
                                    state.cached_coord_query = {};
                                    state.current_dict_index = 0;
                                    state.policy_snapshot = nullptr;
                                    state.has_paged = false;
                                    resp.has_composition = 1;
                                    resp.composition[0] = L'\0';
                                    resp.candidate_count = 0;
                                    resp.handled = 1;
                                }
                            } else {
                                if (state.composition == L";") {
                                    SafeCopyCommitText(resp, L"；");
                                } else {
                                    SafeCopyCommitText(resp, state.composition);
                                }
                                resp.commit_event_id = 0;
                                state.AdvanceGeneration();
                                state.composition.clear();
                                state.cached_coord_query = {};
                                state.current_dict_index = 0;
                                state.policy_snapshot = nullptr;
                                state.has_paged = false;
                                resp.has_composition = 1;
                                resp.composition[0] = L'\0';
                                resp.candidate_count = 0;
                                resp.handled = 1;
                            }
                        }
                    }
                    else if (vk == VK_TAB) {
                        if (state.cached_coord_query.has_next_dictionary && g_engine.GetCoordinator()) {
                            auto qNext = g_engine.GetCoordinator()->QueryNextDictionary(
                                state.current_query_code,
                                state.current_dict_index,
                                state.policy_snapshot
                            );
                            state.current_dict_index = qNext.current_dict_index;
                            state.cached_coord_query = qNext;
                            state.current_page = 0;
                            state.has_paged = false;
                            state.AdvanceGeneration();

                            resp.total_candidates = static_cast<uint32_t>(qNext.candidates.size());
                            PopulateCandidatesPage(resp, state);
                            resp.has_composition = 1;
                            wcsncpy_s(resp.composition, state.composition.c_str(), _TRUNCATE);
                            state.candidate_version++;
                            resp.candidate_version = state.candidate_version;
                            resp.handled = 1;
                        } else {
                            resp.handled = 0;
                        }
                    }
                    else if (vk == VK_RETURN) {
                        // Commit raw composition string unlearned
                        SafeCopyCommitText(resp, state.composition);
                        resp.commit_event_id = 0;
                        state.composition.clear();
                        state.cached_coord_query = {};
                        state.current_dict_index = 0;
                        state.policy_snapshot = nullptr;
                        state.has_paged = false;
                        state.AdvanceGeneration();
                        resp.has_composition = 1;
                        resp.composition[0] = L'\0';
                        resp.candidate_count = 0;
                        resp.handled = 1;
                    }
                    else if (vk == VK_BACK) {
                        state.composition.pop_back();
                        state.current_page = 0;
                        state.has_paged = false;
                        state.AdvanceGeneration();
                        PopulateCandidates(state, resp);
                        resp.handled = 1;
                    }
                    else if (vk == VK_ESCAPE) {
                        state.composition.clear();
                        state.cached_coord_query = {};
                        state.current_dict_index = 0;
                        state.policy_snapshot = nullptr;
                        state.has_paged = false;
                        state.AdvanceGeneration();
                        resp.has_composition = 1;
                        resp.composition[0] = L'\0';
                        resp.candidate_count = 0;
                        resp.handled = 1;
                    }
                    else if (vk == VK_NEXT || vk == VK_OEM_PERIOD || vk == VK_OEM_PLUS) {
                        uint32_t total = static_cast<uint32_t>(state.cached_coord_query.candidates.size());
                        uint32_t pageSize = state.policy_snapshot ? state.policy_snapshot->page_size : 5;
                        if (pageSize < 5) pageSize = 5; if (pageSize > 9) pageSize = 9;
                        if ((state.current_page + 1) * pageSize < total) {
                            state.current_page++;
                            state.has_paged = true;
                            state.AdvanceGeneration();
                            PopulateCandidates(state, resp);
                        }
                        resp.handled = 1;
                    }
                    else if (vk == VK_PRIOR || vk == VK_OEM_COMMA || vk == VK_OEM_MINUS) {
                        if (state.current_page > 0) {
                            state.current_page--;
                            state.has_paged = true;
                            state.AdvanceGeneration();
                            PopulateCandidates(state, resp);
                        }
                        resp.handled = 1;
                    }
                    else {
                        // Do NOT eat navigation/arrow keys (VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_TAB, etc.)!
                        resp.handled = 0;
                    }
                }
            }
        }

        resp.status_flags = ComputeModeFlags(state);
        WriteFile(hPipe, &resp, sizeof(resp), &bytesWritten, nullptr);
        FlushFileBuffers(hPipe);
    }

    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
}

static std::string ResolveDictDir(const std::string& overrideDir) {
    if (!overrideDir.empty() && std::filesystem::exists(overrideDir + "/wubi86.dict")) {
        return overrideDir;
    }
    char envBuf[MAX_PATH];
    DWORD envLen = GetEnvironmentVariableA("PERSONAL_IME_DICT_DIR", envBuf, MAX_PATH);
    if (envLen > 0 && envLen < MAX_PATH && std::filesystem::exists(std::string(envBuf) + "/wubi86.dict")) {
        return std::string(envBuf);
    }
    std::vector<std::string> candidates = {
        "data/dictionaries",
        "../../data/dictionaries",
        "../data/dictionaries",
        "C:/Projects/AITools/PersonalIME/data/dictionaries"
    };
    char exeBuf[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exeBuf, MAX_PATH) > 0) {
        std::filesystem::path exePath(exeBuf);
        std::string exeDir = exePath.parent_path().string();
        candidates.push_back(exeDir + "/dictionaries");
        candidates.push_back(exeDir + "/data/dictionaries");
        candidates.push_back(exeDir + "/../data/dictionaries");
    }
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p + "/wubi86.dict") && std::filesystem::exists(p + "/pinyin.dict")) {
            return p;
        }
    }
    return "data/dictionaries";
}

static std::string ResolveDbPath(const std::string& overrideDb) {
    if (!overrideDb.empty()) return overrideDb;
    char envBuf[MAX_PATH];
    DWORD envLen = GetEnvironmentVariableA("PERSONAL_IME_DB_PATH", envBuf, MAX_PATH);
    if (envLen > 0 && envLen < MAX_PATH) {
        return std::string(envBuf);
    }
    char appData[MAX_PATH];
    if (GetEnvironmentVariableA("APPDATA", appData, MAX_PATH) > 0) {
        std::string dir = std::string(appData) + "\\PersonalIME";
        std::filesystem::create_directories(dir);
        return dir + "\\user_dict.db";
    }
    return "user_dict.db";
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    std::wcout << L"========================================\n";
    std::wcout << L"  Personal IME Background Service (Stage 3)\n";
    std::wcout << L"========================================\n";

    // Initialize monotonically increasing base event ID using precise system time
    g_nextEventId.store(InitBaseEventId());

    std::string overrideDb;
    std::string overrideDict;
    std::string overrideAdminPipe;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            overrideDb = argv[++i];
        } else if (strcmp(argv[i], "--dict") == 0 && i + 1 < argc) {
            overrideDict = argv[++i];
        } else if (strcmp(argv[i], "--admin-pipe") == 0 && i + 1 < argc) {
            overrideAdminPipe = argv[++i];
        }
    }

    std::string dbPath = ResolveDbPath(overrideDb);
    std::string dictDir = ResolveDictDir(overrideDict);

    std::wcout << L"[ime-service] Initializing Engine...\n";
    std::wcout << L"[ime-service] Database Path: " << ime::Utf8ToWide(dbPath) << L"\n";
    std::wcout << L"[ime-service] Dictionary Dir: " << ime::Utf8ToWide(dictDir) << L"\n";

    if (!g_engine.Initialize(dbPath, dictDir)) {
        std::wcerr << L"[ime-service] [FATAL] Failed to initialize IME Engine!\n";
        return 1;
    }

    std::wcout << L"[ime-service] Engine initialized successfully: "
               << g_engine.GetBuiltinDict().WubiEntryCount() << L" wubi, "
               << g_engine.GetBuiltinDict().PinyinEntryCount() << L" pinyin entries.\n";

    // Restore uncompleted pending commits across service restart
    std::vector<ime::EntryRepository::PendingCommitRecord> loadedPending;
    if (!g_engine.LoadAllPendingCommits(loadedPending)) {
        std::wcerr << L"[ime-service] [FATAL] Failed to read pending commit recovery log from database! Aborting startup to prevent unjournaled loss.\n";
        return 1;
    }
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        for (const auto& rec : loadedPending) {
            uint64_t evId = 0;
            try {
                evId = std::stoull(rec.event_id);
            } catch (...) { continue; }
            ServerPendingCommit pc;
            pc.event_id = evId;
            pc.session_id = rec.session_id;
            pc.context_id = rec.context_id;
            pc.connection_epoch = 0; // Restoration connection epoch 0 ensures old cancel doesn't restore stale composition
            pc.generation = 0;
            pc.scheme = rec.scheme;
            pc.query_code = rec.query_code;
            pc.entry_id = rec.entry_id;
            pc.text = rec.text;
            pc.created_at = rec.created_at;
            pc.state = CommitState::Pending;

            pc.candidate.entry_id = rec.entry_id;
            pc.candidate.scheme = rec.scheme;
            pc.candidate.text = rec.text;
            pc.candidate.raw_code = rec.query_code;
            pc.candidate.matched_code = rec.matched_code;
            pc.candidate.resolved_code = rec.resolved_code;
            pc.candidate.layout_id = rec.layout_id;

            auto pol = std::make_shared<ime::InputPolicy>();
            pol->learning_enabled = rec.learning_enabled;
            pol->primary_method = rec.scheme;
            if (rec.scheme == ime::InputScheme::Shuangpin && !rec.layout_id.empty()) {
                pol->shuangpin_layout = ime::ShuangpinLayoutFromString(rec.layout_id);
            }
            pc.policy_snapshot = pol;

            g_pendingCommits[evId] = pc;
        }
    }
    std::wcout << L"[ime-service] Restored " << loadedPending.size() << L" uncompleted pending commits from database.\n";
    std::wcout << L"[ime-service] Pipe Name: " << IME_PIPE_NAME << L"\n";

    // Start Stage 4B Admin Named Pipe Server
    std::wstring adminPipeName = overrideAdminPipe.empty() ? IME_ADMIN_PIPE_NAME : ime::Utf8ToWide(overrideAdminPipe);
    if (!adminPipeName.starts_with(L"\\\\.\\pipe\\")) {
        adminPipeName = L"\\\\.\\pipe\\" + adminPipeName;
    }
    ime::AdminServer adminServer(g_engine, adminPipeName);
    if (!adminServer.Start()) {
        std::wcerr << L"[ime-service] [WARNING] Failed to start Admin Named Pipe server!\n";
    } else {
        std::wcout << L"[ime-service] Admin Pipe started: " << adminServer.GetPipeName() << L"\n";
    }

    // Strictly scoped security descriptor using actual Current User SID at runtime:
    std::wstring userSid = GetCurrentUserSidString();
    if (userSid.empty()) {
        std::wcerr << L"[ime-service] [FATAL] Failed to query user SID. Aborting to fail closed.\n";
        return 1;
    }

    std::wstring sddl = L"D:(A;;GA;;;" + userSid + L")(A;;GA;;;AC)(A;;GA;;;BA)(A;;GA;;;SY)";

    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(),
            SDDL_REVISION_1,
            &pSD,
            nullptr)) {
        std::wcout << L"[ime-service] Failed to create security descriptor! Error: "
                   << GetLastError() << L"\n";
        return 1;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = pSD;
    sa.bInheritHandle = FALSE;

    std::wcout << L"[ime-service] Listening for TSF Client connections (PIPE_REJECT_REMOTE_CLIENTS enabled)...\n";

    while (true) {
        HANDLE hPipe = CreateNamedPipeW(
            IME_PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            sizeof(IpcKeyResponse) * 2,
            sizeof(IpcKeyRequest) * 2,
            50,
            &sa
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            std::wcout << L"[ime-service] CreateNamedPipe failed: " << GetLastError() << L"\n";
            break;
        }

        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            std::thread(HandleClient, hPipe).detach();
        } else {
            CloseHandle(hPipe);
        }
    }

    if (pSD) {
        LocalFree(pSD);
    }

    return 0;
}
