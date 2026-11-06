#pragma once

#include <cstdint>
#include <string>

namespace ime {

// Admin Named Pipe Name
#define IME_ADMIN_PIPE_NAME L"\\\\.\\pipe\\PersonalIME_Admin_Pipe"

// Wire byte sequence for Admin Protocol Magic: ASCII "PIMA"
// Byte 0: 0x50 ('P')
// Byte 1: 0x49 ('I')
// Byte 2: 0x4D ('M')
// Byte 3: 0x41 ('A')
constexpr uint8_t ADMIN_MAGIC_BYTES[4] = { 'P', 'I', 'M', 'A' };

// On Little-Endian platforms (x86/x64), reading {'P','I','M','A'} as uint32_t yields 0x414D4950
constexpr uint32_t ADMIN_MAGIC_LITTLE_ENDIAN = 0x414D4950;

// Protocol Version: 1.0 (0x0100)
constexpr uint32_t ADMIN_PROTOCOL_VERSION_1 = 0x0100;

// Maximum payload size for JSON request/response (16 MB)
constexpr uint32_t MAX_ADMIN_PAYLOAD_SIZE = 16 * 1024 * 1024;

// Default pipe operation timeout in milliseconds
constexpr uint32_t ADMIN_DEFAULT_TIMEOUT_MS = 5000;

// Structured Error Codes
enum class AdminErrorCode : int {
    Success             = 0,
    InvalidMagic        = 1001,
    UnsupportedVersion  = 1002,
    PayloadTooLarge     = 1003,
    InvalidJson         = 1004,
    UnknownAction       = 1005,
    MissingParameter    = 1006,
    ValidationFailed    = 1007,
    EntryNotFound       = 2001,
    StorageError        = 3001,
    Busy                = 3002,
    RevisionConflict    = 4001
};

#pragma pack(push, 1)

// Exactly 16-byte fixed-size binary frame header
struct AdminHeader {
    uint8_t  magic[4];       // {'P', 'I', 'M', 'A'}
    uint32_t version;        // ADMIN_PROTOCOL_VERSION_1 (little-endian)
    uint32_t payload_length; // UTF-8 payload length in bytes (little-endian)
    uint32_t request_id;     // Correlation ID between client and server (little-endian)
};

#pragma pack(pop)

static_assert(sizeof(AdminHeader) == 16, "AdminHeader must be exactly 16 bytes");

} // namespace ime

