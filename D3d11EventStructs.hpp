#pragma once
namespace Microsoft_Windows_D3D11 {

    struct __declspec(uuid("{db6f6ddb-ac77-4e88-8253-819df9bbf140}")) GUID_STRUCT;
    static const auto GUID = __uuidof(GUID_STRUCT);

    enum class Keyword : uint64_t {
        Objects = 0x1,
        Events = 0x2,
        JournalEntries = 0x4,
        Microsoft_Windows_DXGI_Analytic = 0x8000000000000000,
        Microsoft_Windows_DXGI_Logging = 0x4000000000000000,
    };

    enum class Level : uint8_t {
        win_LogAlways = 0x0,
    };

    enum class Channel : uint8_t {
        Microsoft_Windows_DXGI_Analytic = 0x10,
        Microsoft_Windows_DXGI_Logging = 0x11,
    };

    // Event descriptors:
#define EVENT_DESCRIPTOR_DECL(name_, id_, version_, channel_, level_, opcode_, task_, keyword_) struct name_ { \
    static uint16_t const Id      = id_; \
    static uint8_t  const Version = version_; \
    static uint8_t  const Channel = channel_; \
    static uint8_t  const Level   = level_; \
    static uint8_t  const Opcode  = opcode_; \
    static uint16_t const Task    = task_; \
    static Keyword  const Keyword = (Keyword) keyword_; \
};

    EVENT_DESCRIPTOR_DECL(Marker, 38, 0x00, 0x10, 0x0, 0x0, 20, 0x8000000000000100)
}