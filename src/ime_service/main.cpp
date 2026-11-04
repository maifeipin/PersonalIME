#include <windows.h>
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
#include "../core/engine/ime_engine.h"
#include "../core/types.h"

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
    ime::InputScheme scheme = ime::InputScheme::Wubi86;
    std::string query_code; // The actual query code typed by user (e.g. "a", "xi'an")
    std::string entry_id;
    std::wstring text;
    std::wstring composition_before_commit;
    uint32_t page_before_commit = 0;
    CommitState state = CommitState::Pending;
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

    ime::QueryResult cached_query;
    uint32_t current_page = 0;
    static constexpr uint32_t PAGE_SIZE = 5;
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

static void PopulateCandidates(ContextState& st, IpcKeyResponse& resp) {
    resp.has_composition = 1;
    wcsncpy_s(resp.composition, st.composition.c_str(), _TRUNCATE);

    if (st.composition.empty()) {
        st.current_query_code.clear();
        st.cached_query.candidates.clear();
        st.has_paged = false;
        resp.candidate_count = 0;
        resp.total_candidates = 0;
        resp.page_index = 0;
        resp.page_count = 0;
        resp.candidate_version = st.candidate_version;
        return;
    }

    std::string code = ime::WideToUtf8(st.composition);
    st.current_query_code = code;

    ime::InputScheme queryScheme = st.scheme;
    if (!code.empty() && code[0] == ';') {
        queryScheme = ime::InputScheme::Custom;
    }

    st.cached_query = g_engine.Query(queryScheme, code);
    st.candidate_version++;
    resp.candidate_version = st.candidate_version;

    uint32_t total = static_cast<uint32_t>(st.cached_query.candidates.size());
    resp.total_candidates = total;
    uint32_t pageSize = ContextState::PAGE_SIZE;
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
        const auto& cand = st.cached_query.candidates[startIdx + i];
        std::wstring label = std::to_wstring(i + 1) + L". " + cand.text;
        SafeCopyCandidateText(resp.candidates[i], sizeof(resp.candidates[i])/sizeof(wchar_t), label);
    }
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

static void ResetContextCompositionAndState(ContextState& state) {
    state.composition.clear();
    state.prev_composition.clear();
    state.current_query_code.clear();
    state.cached_query.candidates.clear();
    state.current_page = 0;
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
        }

        IpcKeyResponse resp{};
        resp.msg_type = req.msg_type;
        resp.session_id = req.session_id;
        resp.context_id = req.context_id;
        resp.protocol_version = IPC_PROTOCOL_VERSION_STAGE3;
        resp.status_flags = ComputeModeFlags(state);

        // Strict protocol version validation
        if (req.protocol_version != IPC_PROTOCOL_VERSION_STAGE3) {
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

                        bool ok = g_engine.CommitCandidate(
                            std::to_string(req.event_id),
                            pc.session_id,
                            pc.context_id,
                            pc.scheme,
                            pc.query_code, // Use actual query_code typed by user!
                            pc.entry_id
                        );

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
            bool prevChinese = state.is_chinese;
            ime::InputScheme prevScheme = state.scheme;

            if (req.mode_arg & IPC_MODE_CHINESE) state.is_chinese = true;
            else state.is_chinese = false;

            if (req.mode_arg & IPC_MODE_PINYIN) state.scheme = ime::InputScheme::Pinyin;
            else state.scheme = ime::InputScheme::Wubi86;

            state.full_width = (req.mode_arg & IPC_MODE_FULL_WIDTH) != 0;
            state.chinese_punctuation = (req.mode_arg & IPC_MODE_CHINESE_PUNCT) != 0;

            // Consistent mode transition: if scheme or Chinese mode changed, cancel composition!
            if (state.is_chinese != prevChinese || state.scheme != prevScheme) {
                ResetContextCompositionAndState(state);
                resp.has_composition = 1;
                resp.composition[0] = L'\0';
                resp.candidate_count = 0;
            }

            resp.status_flags = ComputeModeFlags(state);
            resp.handled = 1;
        }
        else if (req.msg_type == static_cast<uint32_t>(IpcMsgType::GetMode)) {
            resp.status_flags = ComputeModeFlags(state);
            resp.handled = 1;
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
                    if ((vk >= 'A' && vk <= 'Z') || (vk >= '1' && vk <= '5') ||
                        vk == VK_SPACE || vk == VK_BACK || vk == VK_ESCAPE || vk == VK_RETURN ||
                        vk == VK_OEM_7 || vk == VK_OEM_1 || vk == VK_NEXT || vk == VK_PRIOR ||
                        vk == VK_OEM_PERIOD || vk == VK_OEM_COMMA || vk == VK_OEM_PLUS || vk == VK_OEM_MINUS) {
                        resp.handled = 1;
                    } else {
                        // Do NOT eat navigation/arrow keys (VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_TAB, etc.)!
                        resp.handled = 0;
                    }
                }
            } else {
                // English mode
                if (state.full_width && !MapFullWidthChar(vk, mods).empty()) {
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
                // Ctrl + Shift: toggle scheme (Wubi86 <-> Pinyin)
                state.scheme = (state.scheme == ime::InputScheme::Wubi86) ? ime::InputScheme::Pinyin : ime::InputScheme::Wubi86;
                ResetContextCompositionAndState(state);
                resp.status_flags = ComputeModeFlags(state);
                resp.has_composition = 1;
                resp.composition[0] = L'\0';
                resp.handled = 1;
            }
            else if ((mods & 2) && vk == VK_SPACE) {
                // Ctrl + Space: toggle Chinese / English
                state.is_chinese = !state.is_chinese;
                ResetContextCompositionAndState(state);
                resp.status_flags = ComputeModeFlags(state);
                resp.has_composition = 1;
                resp.composition[0] = L'\0';
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
                        // Apostrophe in Pinyin (e.g. xi'an)
                        if (state.scheme == ime::InputScheme::Pinyin && state.composition.length() < 127) {
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
                    else if (vk == VK_OEM_1 && state.composition.front() == L';') {
                        if (state.composition.length() < 127) {
                            state.composition.push_back(L';');
                            state.current_page = 0;
                            state.has_paged = false;
                            state.AdvanceGeneration();
                            PopulateCandidates(state, resp);
                        }
                        resp.handled = 1;
                    }
                    else if (vk >= '1' && vk <= '5') {
                        bool staleVersion = false;
                        if (!state.cached_query.candidates.empty()) {
                            if (req.candidate_version != state.candidate_version) {
                                staleVersion = true;
                            }
                        }

                        if (staleVersion) {
                            resp.handled = 0; // Reject stale candidate snapshot selection
                        } else {
                            uint32_t candIdx = state.current_page * ContextState::PAGE_SIZE + (vk - '1');
                            if (candIdx < state.cached_query.candidates.size()) {
                                const auto& cand = state.cached_query.candidates[candIdx];
                                uint64_t evId = NextEventId();

                                bool saved = g_engine.SavePendingCommit(
                                    std::to_string(evId),
                                    req.session_id,
                                    req.context_id,
                                    cand.scheme,
                                    state.current_query_code,
                                    cand.entry_id,
                                    cand.text
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
                                    pc.scheme = cand.scheme;
                                    pc.query_code = state.current_query_code; // Crucial: use query_code!
                                    pc.entry_id = cand.entry_id;
                                    pc.text = cand.text;
                                    pc.composition_before_commit = state.composition;
                                    pc.page_before_commit = state.current_page;
                                    pc.state = CommitState::Pending;
                                    {
                                        std::lock_guard<std::mutex> lock(g_pendingMutex);
                                        g_pendingCommits[evId] = pc;
                                    }

                                    SafeCopyCommitText(resp, cand.text);
                                    resp.commit_event_id = evId;
                                    state.composition.clear();
                                    state.cached_query.candidates.clear();
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
                        bool staleVersion = false;
                        if (!state.cached_query.candidates.empty()) {
                            if (req.candidate_version != state.candidate_version) {
                                staleVersion = true;
                            }
                        }

                        if (staleVersion) {
                            resp.handled = 0; // Reject stale candidate snapshot selection
                        } else {
                            if (!state.cached_query.candidates.empty()) {
                                uint32_t candIdx = state.current_page * ContextState::PAGE_SIZE;
                                if (candIdx < state.cached_query.candidates.size()) {
                                    const auto& cand = state.cached_query.candidates[candIdx];
                                    uint64_t evId = NextEventId();

                                    bool saved = g_engine.SavePendingCommit(
                                        std::to_string(evId),
                                        req.session_id,
                                        req.context_id,
                                        cand.scheme,
                                        state.current_query_code,
                                        cand.entry_id,
                                        cand.text
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
                                        pc.scheme = cand.scheme;
                                        pc.query_code = state.current_query_code; // Crucial: use query_code!
                                        pc.entry_id = cand.entry_id;
                                        pc.text = cand.text;
                                        pc.composition_before_commit = state.composition;
                                        pc.page_before_commit = state.current_page;
                                        pc.state = CommitState::Pending;
                                        {
                                            std::lock_guard<std::mutex> lock(g_pendingMutex);
                                            g_pendingCommits[evId] = pc;
                                        }

                                        SafeCopyCommitText(resp, cand.text);
                                        resp.commit_event_id = evId;
                                        state.composition.clear();
                                        state.cached_query.candidates.clear();
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
                                    state.cached_query.candidates.clear();
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
                                state.cached_query.candidates.clear();
                                state.has_paged = false;
                                resp.has_composition = 1;
                                resp.composition[0] = L'\0';
                                resp.candidate_count = 0;
                                resp.handled = 1;
                            }
                        }
                    }
                    else if (vk == VK_RETURN) {
                        // Commit raw composition string unlearned
                        SafeCopyCommitText(resp, state.composition);
                        resp.commit_event_id = 0;
                        state.composition.clear();
                        state.cached_query.candidates.clear();
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
                        state.cached_query.candidates.clear();
                        state.has_paged = false;
                        state.AdvanceGeneration();
                        resp.has_composition = 1;
                        resp.composition[0] = L'\0';
                        resp.candidate_count = 0;
                        resp.handled = 1;
                    }
                    else if (vk == VK_NEXT || vk == VK_OEM_PERIOD || vk == VK_OEM_PLUS) {
                        uint32_t total = static_cast<uint32_t>(state.cached_query.candidates.size());
                        uint32_t pageSize = ContextState::PAGE_SIZE;
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
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            overrideDb = argv[++i];
        } else if (strcmp(argv[i], "--dict") == 0 && i + 1 < argc) {
            overrideDict = argv[++i];
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
            pc.state = CommitState::Pending;
            g_pendingCommits[evId] = pc;
        }
    }
    std::wcout << L"[ime-service] Restored " << loadedPending.size() << L" uncompleted pending commits from database.\n";
    std::wcout << L"[ime-service] Pipe Name: " << IME_PIPE_NAME << L"\n";

    // Strictly scoped security descriptor using actual Current User SID at runtime:
    std::wstring userSid = GetCurrentUserSidString();
    if (userSid.empty()) {
        std::wcout << L"[ime-service] Warning: Failed to query user SID, defaulting to Interactive Users (IU)\n";
        userSid = L"IU";
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
