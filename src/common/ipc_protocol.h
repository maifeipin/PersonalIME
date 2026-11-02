#pragma once
#include <cstdint>
#include <cwchar>

enum class IpcMsgType : uint32_t {
    Ping = 1,
    Pong = 2,
    TestKeyDown = 3,
    KeyDown = 4,
    Reset = 5,
    Rollback = 6,
    ContextLost = 7
};

#pragma pack(push, 1)

struct IpcKeyRequest {
    uint32_t msg_type;       // IpcMsgType
    uint64_t session_id;     // Per-TIP instance ID
    uint64_t context_id;     // Per-ITfContext pointer/ID
    uint32_t vk_code;        // Virtual key code
    uint32_t scan_code;
    uint32_t modifiers;      // Bitmask: 1=Shift, 2=Ctrl, 4=Alt, 8=Win
};

struct IpcKeyResponse {
    uint32_t msg_type;       // IpcMsgType
    uint64_t session_id;
    uint64_t context_id;
    uint32_t handled;        // 1 = eaten by IME, 0 = pass-through
    uint32_t has_composition; // 1 = composition string updated
    wchar_t  composition[64]; // Current composition string (null-terminated)
    uint32_t has_commit;      // 1 = commit text present
    wchar_t  commit_text[64]; // Committed text (null-terminated)
    uint32_t candidate_count; // Number of valid candidates (max 5)
    wchar_t  candidates[5][32];// Up to 5 candidates (null-terminated)
};

#pragma pack(pop)
