#pragma once

#include "nanobook/order_book.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace nanobook {

// A compact, OUCH-inspired protocol used by Nanobook's order-entry gateway.
// Every frame is: one-byte version, one-byte type, two-byte big-endian payload
// length, payload. Versioning makes incompatible changes fail closed.
inline constexpr std::uint8_t protocol_version = 1;
// Fixed-width payloads make decoding bounded and allocation-free.
enum class WireType : std::uint8_t {
    enter = 'O',
    cancel = 'X',
    replace = 'U',
    accepted = 'A',
    canceled = 'C',
    replaced = 'R',
    executed = 'E',
    rejected = 'J'
};

enum class CommandType : std::uint8_t { enter, cancel, replace };
enum class EventType : std::uint8_t { accepted, canceled, replaced, executed, rejected };
enum class ProtocolError : std::uint8_t {
    none,
    malformed_frame,
    unsupported_type,
    unsupported_version,
    invalid_side,
    symbol_required,
    unknown_symbol,
    journal_failure
};

// Symbols are stored as eight ASCII bytes padded with spaces, matching the
// fixed-width convention used by exchange protocols.
struct Symbol {
    std::array<char, 8> bytes{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};

    static Symbol from_string(std::string_view text);
    std::string_view view() const;
    bool empty() const;
    friend bool operator==(const Symbol&, const Symbol&) = default;
};

struct Command {
    CommandType type = CommandType::enter;
    std::uint64_t sequence = 0;
    Symbol symbol;
    std::uint64_t order_id = 0;
    Side side = Side::buy;
    std::int64_t price = 0;
    std::uint32_t quantity = 0;
};

struct Event {
    EventType type = EventType::accepted;
    std::uint64_t sequence = 0;
    Symbol symbol;
    std::uint64_t order_id = 0;
    std::uint64_t resting_id = 0;
    std::int64_t price = 0;
    std::uint32_t quantity = 0;
    std::uint32_t remaining = 0;
    Status status = Status::ok;
    ProtocolError protocol_error = ProtocolError::none;
};

struct WireFrame {
    static constexpr std::size_t capacity = 64;
    std::array<std::byte, capacity> bytes{};
    std::size_t size = 0;
};

using CommandCallback = void (*)(const Command&, void*);
using ProtocolErrorCallback = void (*)(ProtocolError, void*);

class FrameDecoder {
public:
    static constexpr std::size_t max_frame_size = WireFrame::capacity;

    void consume(std::span<const std::byte> input, CommandCallback command_callback,
                 ProtocolErrorCallback error_callback, void* context);
    void reset();

private:
    std::array<std::byte, max_frame_size> buffer_{};
    std::size_t buffered_ = 0;
    std::size_t expected_ = 0;

    void parse_frame(CommandCallback command_callback,
                     ProtocolErrorCallback error_callback, void* context);
};

WireFrame encode_enter(Symbol symbol, std::uint64_t order_id, Side side,
                       std::int64_t price, std::uint32_t quantity);
WireFrame encode_cancel(Symbol symbol, std::uint64_t order_id);
WireFrame encode_replace(Symbol symbol, std::uint64_t order_id,
                         std::int64_t price, std::uint32_t quantity);
WireFrame encode_event(const Event& event);

// Decodes one complete response frame. Intended for clients and tests.
bool decode_event(std::span<const std::byte> frame, Event& event);

} // namespace nanobook
