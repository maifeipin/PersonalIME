#pragma once
#include <cstdint>
#include <cwchar>
#include "../core/types.h"

constexpr uint32_t IPC_PROTOCOL_VERSION_STAGE3 = 0x0300;
constexpr uint32_t IPC_PROTOCOL_VERSION_STAGE4 = 0x0400;
constexpr uint32_t IPC_PROTOCOL_VERSION_STAGE7 = 0x0700;

enum class IpcMsgType : uint32_t {
    Ping              = 1,
    Pong              = 2,
    TestKeyDown       = 3,
    KeyDown           = 4,
    Reset             = 5,
    Rollback          = 6,
    ContextLost       = 7,
    CommitSuccess     = 8,  // Host document successfully inserted text; record learning
    CommitCancel      = 9,  // Host document edit failed; cancel learning
    SetMode           = 10, // Explicitly set mode flags
    GetMode           = 11, // Query mode flags
    NextDictionary    = 12, // Step to next dictionary in priority order
    SetScheme         = 13, // Explicitly set input scheme (Stage 4)
    GetScheme         = 14, // Query input scheme (Stage 4)
    OpenManager       = 15, // Open or activate Manager (Stage 5)
    ExecutePlugin     = 16, // Execute a registered plugin by ID (Stage 5 Extension)
    GetPlugins        = 17, // Fetch active plugins list (Stage 5 Extension)
    CommitTerminal    = 18, // Stage 7: Versioned idempotent commit finalization
    QueryCommitStatus = 19, // Stage 7: Query persisted final disposition for commit_event_id
    GetStage7Settings = 20  // Stage 7.4: Fetch theme_mode, preedit_display_mode, and per-host unicode_injection
};

// Stage 7: Terminal commit disposition for idempotent finalization & WAL v2 recovery
enum class IpcCommitDisposition : uint32_t {
    None                 = 0,
    ConfirmedWritten     = 1, // Host text confirmed written; record learning and finalize
    CancelRestore        = 2, // Unmodified & same epoch/generation; restore candidates if valid
    CancelDiscard        = 3, // Unmodified & stale/unfocused; discard pending commit
    DispatchedUnverified = 4, // Unicode events dispatched via SendInput; consume input without learning
    ClosedUnknown        = 5  // No potential writer remains & outcome unknown; finalize without learning
};

// Explicit candidate source type encoded in candidate metadata (never guessed from text/ID)
enum class IpcCandidateType : uint16_t {
    Normal = 0,
    Emoji  = 1
};

// Independent Scheme Identifier (Wire Protocol Values)
enum class IpcSchemeId : uint32_t {
    Unchanged = 0,
    Wubi86    = 1,
    Pinyin    = 2,
    Shuangpin = 3,
    Custom    = 4
};

// Explicit bi-directional mapping between Wire IpcSchemeId and Core ime::InputScheme
inline bool IpcSchemeIdToInputScheme(uint32_t wireId, ime::InputScheme& outScheme) {
    switch (static_cast<IpcSchemeId>(wireId)) {
        case IpcSchemeId::Wubi86:
            outScheme = ime::InputScheme::Wubi86; // wire 1 -> core 2
            return true;
        case IpcSchemeId::Pinyin:
            outScheme = ime::InputScheme::Pinyin; // wire 2 -> core 1
            return true;
        case IpcSchemeId::Shuangpin:
            outScheme = ime::InputScheme::Shuangpin; // wire 3 -> core 4
            return true;
        case IpcSchemeId::Custom:
            outScheme = ime::InputScheme::Custom; // wire 4 -> core 3
            return true;
        default:
            return false;
    }
}

inline IpcSchemeId InputSchemeToIpcSchemeId(ime::InputScheme scheme) {
    switch (scheme) {
        case ime::InputScheme::Wubi86:
            return IpcSchemeId::Wubi86; // core 2 -> wire 1
        case ime::InputScheme::Pinyin:
            return IpcSchemeId::Pinyin; // core 1 -> wire 2
        case ime::InputScheme::Shuangpin:
            return IpcSchemeId::Shuangpin; // core 4 -> wire 3
        case ime::InputScheme::Custom:
            return IpcSchemeId::Custom; // core 3 -> wire 4
        default:
            return IpcSchemeId::Unchanged; // 0
    }
}

// Mode flags bitmask
enum IpcModeFlags : uint32_t {
    IPC_MODE_CHINESE           = 1 << 0,  // 1 = Chinese input mode, 0 = English direct passthrough
    IPC_MODE_PINYIN            = 1 << 1,  // Legacy Stage 3: 1 = Pinyin scheme, 0 = Wubi86 scheme
    IPC_MODE_FULL_WIDTH        = 1 << 2,  // 1 = Full-width (全角), 0 = Half-width (半角)
    IPC_MODE_CHINESE_PUNCT     = 1 << 3,  // 1 = Chinese punctuation, 0 = English punctuation
    IPC_STATUS_HAS_NEXT_DICT   = 1 << 4,  // 1 = Next dictionary fallback available
    IPC_STATUS_TRUNCATED       = 1 << 5,  // 1 = Ambiguity exploration / candidate / plugin budget truncated
    IPC_STATUS_ERROR           = 1 << 6,  // 1 = Query encountered storage / internal error
    IPC_STATUS_HAS_PLUGINS_REV = 1 << 7,  // 1 = status_flags bits 24..31 carry 8-bit plugins revision tag
    IPC_STATUS_STAGE7_CAPABLE  = 1 << 8,  // 1 = Server supports Stage 7 protocol (CommitTerminal / QueryCommitStatus / QueryCode)
    IPC_STATUS_CONFLICT_ERROR  = 1 << 9,  // 1 = CommitTerminal rejected due to mutually exclusive disposition conflict
    IPC_STATUS_HAS_QUERY_CODE  = 1 << 10, // 1 = When has_commit==0, commit_text carries segmented query_code for Preedit Header
    IPC_STATUS_HAS_CAND_CORES  = 1 << 11, // 1 = Each candidates[i] carries (1 + core_len) wchar_t immediately after the label's NUL
    IPC_STATUS_HAS_POLICY_REV  = 1 << 12  // 1 = status_flags bits 13..15 carry 3-bit policy revision tag
};

// Policy revision tag encoding in status_flags (bits 13..15)
constexpr uint32_t IPC_POLICY_REV_SHIFT = 13;
constexpr uint32_t IPC_POLICY_REV_MASK  = 0x0000E000;

// Scheme encoding in status_flags (bits 16..23)
constexpr uint32_t IPC_SCHEME_SHIFT = 16;
constexpr uint32_t IPC_SCHEME_MASK  = 0x00FF0000;

// Plugins revision tag encoding in status_flags (bits 24..31)
constexpr uint32_t IPC_PLUGINS_REV_SHIFT = 24;
constexpr uint32_t IPC_PLUGINS_REV_MASK  = 0xFF000000;

// Update mask for SetMode / SetScheme
enum IpcUpdateMask : uint32_t {
    IPC_UPDATE_CHINESE       = 1 << 0,
    IPC_UPDATE_FULL_WIDTH    = 1 << 1,
    IPC_UPDATE_CHINESE_PUNCT = 1 << 2,
    IPC_UPDATE_SCHEME        = 1 << 3
};

// KeyDown mode_arg flags
constexpr uint32_t IPC_KEYDOWN_EXPLICIT_SELECT = 1u << 0; // Explicit candidate selection from UI click or Enter/Space on highlighted item
constexpr uint32_t IPC_KEYDOWN_SHIFT_TAP       = 1u << 1; // Standalone Shift tap-release toggle Chinese/English

// Modifiers bitmask additions (IpcKeyRequest::modifiers)
constexpr uint32_t IPC_MOD_CAPSLOCK = 1u << 4; // CapsLock toggle state active

#pragma pack(push, 1)

struct IpcKeyRequest {
    uint32_t msg_type;         // IpcMsgType (offset 0)
    uint64_t session_id;       // Per-TIP instance ID (offset 4)
    uint64_t context_id;       // Per-ITfContext pointer/ID (offset 12)
    uint32_t vk_code;          // Virtual key code (offset 20)
    uint32_t scan_code;        // Hardware scan code (offset 24)
    uint32_t modifiers;        // Bitmask: 1=Shift, 2=Ctrl, 4=Alt, 8=Win (offset 28)
    
    // Stage 3 protocol extensions
    uint32_t protocol_version; // IPC_PROTOCOL_VERSION_STAGE3 or IPC_PROTOCOL_VERSION_STAGE4 (offset 32)
    uint64_t event_id;         // CommitSuccess / CommitCancel confirmation token (offset 36)
    uint32_t mode_arg;         // Argument for SetMode or extra flags (offset 44)
    uint32_t candidate_version;// Candidate snapshot version selected by client (offset 48)
    
    // Stage 4 protocol extensions (total struct size remains 64 bytes)
    uint32_t scheme_id;        // Explicit IpcSchemeId / InputScheme (offset 52)
    uint32_t update_mask;      // IpcUpdateMask bitmask (offset 56)
    // Stage 7 protocol extension (replaces reserved[4], total struct size remains 64 bytes)
    uint32_t context_epoch;    // Context lifecycle epoch for ownership verification (offset 60)
};                             // Total size: 64 bytes

struct IpcKeyResponse {
    uint32_t msg_type;         // IpcMsgType (offset 0)
    uint64_t session_id;       // Session ID (offset 4)
    uint64_t context_id;       // Context ID (offset 12)
    uint32_t handled;          // 1 = eaten by IME, 0 = pass-through (offset 20)
    
    // Stage 3 status & flags
    uint32_t protocol_version; // IPC_PROTOCOL_VERSION_STAGE3/4/7 (offset 24)
    uint32_t status_flags;     // IpcModeFlags bitmask (offset 28)

    // Composition string
    uint32_t has_composition;   // 1 = composition string updated (offset 32)
    wchar_t  composition[128];  // Current composition string (null-terminated) (offset 36, 256 bytes)

    // Commit payload
    uint32_t has_commit;        // 1 = commit text present (offset 292)
    uint64_t commit_event_id;   // Unique commit event token for host confirmation (offset 296)
    uint32_t commit_length;     // Character length of commit text (offset 304)
    wchar_t  commit_text[1024]; // Committed text (or query_code when has_commit==0 && IPC_STATUS_HAS_QUERY_CODE) (offset 308, 2048 bytes)

    // Paginated candidate list
    uint32_t candidate_version; // Candidate snapshot version / epoch, or commit input_generation when has_commit==1 (offset 2356)
    uint32_t total_candidates;  // Total matching candidates for current code (offset 2360)
    uint32_t page_index;        // Current page index (0-based) (offset 2364)
    uint32_t page_count;        // Total candidate pages (offset 2368)
    uint32_t candidate_count;   // Number of candidates on this page (0~9) (offset 2372)
    wchar_t  candidates[9][128];// Up to 9 candidates for current page (offset 2376, 2304 bytes)
};                             // Total size: 4680 bytes

#pragma pack(pop)

// Stage 7 helper: encode/decode 64-bit owner_instance_id in (scheme_id, update_mask) for
// non-SetMode/SetScheme requests (TestKeyDown, KeyDown, CommitTerminal, QueryCommitStatus)
inline void SetIpcRequestOwnerInstanceId(IpcKeyRequest& req, uint64_t ownerInstanceId) {
    req.scheme_id = static_cast<uint32_t>(ownerInstanceId & 0xFFFFFFFFULL);
    req.update_mask = static_cast<uint32_t>((ownerInstanceId >> 32) & 0xFFFFFFFFULL);
}

inline uint64_t GetIpcRequestOwnerInstanceId(const IpcKeyRequest& req) {
    return (static_cast<uint64_t>(req.update_mask) << 32) | static_cast<uint64_t>(req.scheme_id);
}

static_assert(sizeof(IpcKeyRequest) == 64, "IpcKeyRequest wire size must remain 64 bytes");
static_assert(sizeof(IpcKeyResponse) == 4680, "IpcKeyResponse wire size must remain 4680 bytes");
