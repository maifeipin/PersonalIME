#pragma once
#include <cstdint>
#include <cwchar>
#include "../core/types.h"

constexpr uint32_t IPC_PROTOCOL_VERSION_STAGE3 = 0x0300;
constexpr uint32_t IPC_PROTOCOL_VERSION_STAGE4 = 0x0400;

enum class IpcMsgType : uint32_t {
    Ping           = 1,
    Pong           = 2,
    TestKeyDown    = 3,
    KeyDown        = 4,
    Reset          = 5,
    Rollback       = 6,
    ContextLost    = 7,
    CommitSuccess  = 8,  // Host document successfully inserted text; record learning
    CommitCancel   = 9,  // Host document edit failed; cancel learning
    SetMode        = 10, // Explicitly set mode flags
    GetMode        = 11, // Query mode flags
    NextDictionary = 12, // Step to next dictionary in priority order
    SetScheme      = 13, // Explicitly set input scheme (Stage 4)
    GetScheme      = 14  // Query input scheme (Stage 4)
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
    IPC_MODE_CHINESE         = 1 << 0, // 1 = Chinese input mode, 0 = English direct passthrough
    IPC_MODE_PINYIN          = 1 << 1, // Legacy Stage 3: 1 = Pinyin scheme, 0 = Wubi86 scheme
    IPC_MODE_FULL_WIDTH      = 1 << 2, // 1 = Full-width (全角), 0 = Half-width (半角)
    IPC_MODE_CHINESE_PUNCT   = 1 << 3, // 1 = Chinese punctuation, 0 = English punctuation
    IPC_STATUS_HAS_NEXT_DICT = 1 << 4, // 1 = Next dictionary fallback available
    IPC_STATUS_TRUNCATED     = 1 << 5, // 1 = Ambiguity exploration / candidate budget truncated
    IPC_STATUS_ERROR         = 1 << 6  // 1 = Query encountered storage / internal error
};

// Scheme encoding in status_flags (bits 16..23)
constexpr uint32_t IPC_SCHEME_SHIFT = 16;
constexpr uint32_t IPC_SCHEME_MASK  = 0x00FF0000;

// Update mask for SetMode / SetScheme
enum IpcUpdateMask : uint32_t {
    IPC_UPDATE_CHINESE       = 1 << 0,
    IPC_UPDATE_FULL_WIDTH    = 1 << 1,
    IPC_UPDATE_CHINESE_PUNCT = 1 << 2,
    IPC_UPDATE_SCHEME        = 1 << 3
};

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
    
    // Stage 4 protocol extensions (replaces reserved[12], total struct size remains 64 bytes)
    uint32_t scheme_id;        // Explicit IpcSchemeId / InputScheme (offset 52)
    uint32_t update_mask;      // IpcUpdateMask bitmask (offset 56)
    uint8_t  reserved[4];      // Reserved for alignment & future expansion (offset 60)
};                             // Total size: 64 bytes

struct IpcKeyResponse {
    uint32_t msg_type;         // IpcMsgType (offset 0)
    uint64_t session_id;       // Session ID (offset 4)
    uint64_t context_id;       // Context ID (offset 12)
    uint32_t handled;          // 1 = eaten by IME, 0 = pass-through (offset 20)
    
    // Stage 3 status & flags
    uint32_t protocol_version; // IPC_PROTOCOL_VERSION_STAGE3 (offset 24)
    uint32_t status_flags;     // IpcModeFlags bitmask (offset 28)

    // Composition string
    uint32_t has_composition;   // 1 = composition string updated (offset 32)
    wchar_t  composition[128];  // Current composition string (null-terminated) (offset 36, 256 bytes)

    // Commit payload
    uint32_t has_commit;        // 1 = commit text present (offset 292)
    uint64_t commit_event_id;   // Unique commit event token for host confirmation (offset 296)
    uint32_t commit_length;     // Character length of commit text (offset 304)
    wchar_t  commit_text[1024]; // Committed text (null-terminated, supports long phrases/Emoji) (offset 308, 2048 bytes)

    // Paginated candidate list
    uint32_t candidate_version; // Candidate snapshot version / epoch (offset 2356)
    uint32_t total_candidates;  // Total matching candidates for current code (offset 2360)
    uint32_t page_index;        // Current page index (0-based) (offset 2364)
    uint32_t page_count;        // Total candidate pages (offset 2368)
    uint32_t candidate_count;   // Number of candidates on this page (0~9) (offset 2372)
    wchar_t  candidates[9][128];// Up to 9 candidates for current page (offset 2376, 2304 bytes)
};                             // Total size: 4680 bytes

#pragma pack(pop)
