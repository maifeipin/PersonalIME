#pragma once
#include <cstdint>
#include <cwchar>

constexpr uint32_t IPC_PROTOCOL_VERSION_STAGE3 = 0x0300;

enum class IpcMsgType : uint32_t {
    Ping          = 1,
    Pong          = 2,
    TestKeyDown   = 3,
    KeyDown       = 4,
    Reset         = 5,
    Rollback      = 6,
    ContextLost   = 7,
    CommitSuccess = 8,  // Host document successfully inserted text; record learning
    CommitCancel  = 9,  // Host document edit failed; cancel learning
    SetMode       = 10, // Explicitly set mode flags
    GetMode       = 11  // Query mode flags
};

// Mode flags bitmask
enum IpcModeFlags : uint32_t {
    IPC_MODE_CHINESE         = 1 << 0, // 1 = Chinese input mode, 0 = English direct passthrough
    IPC_MODE_PINYIN          = 1 << 1, // 1 = Pinyin scheme, 0 = Wubi86 scheme
    IPC_MODE_FULL_WIDTH      = 1 << 2, // 1 = Full-width (全角), 0 = Half-width (半角)
    IPC_MODE_CHINESE_PUNCT   = 1 << 3  // 1 = Chinese punctuation, 0 = English punctuation
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
    uint32_t protocol_version; // IPC_PROTOCOL_VERSION_STAGE3 (offset 32)
    uint64_t event_id;         // CommitSuccess / CommitCancel confirmation token (offset 36)
    uint32_t mode_arg;         // Argument for SetMode or extra flags (offset 44)
    uint32_t candidate_version;// Candidate snapshot version selected by client (offset 48)
    uint8_t  reserved[12];     // Reserved for alignment & future expansion (offset 52)
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
