#include "nanobook/protocol.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace nanobook {
namespace {
constexpr std::size_t header_size = 4;
constexpr std::size_t enter_payload = 29;
constexpr std::size_t cancel_payload = 16;
constexpr std::size_t replace_payload = 28;
constexpr std::size_t event_payload = 49;

void put_u16(std::byte* out, std::uint16_t value) {
    out[0] = std::byte(value >> 8);
    out[1] = std::byte(value);
}

void put_u32(std::byte* out, std::uint32_t value) {
    for (int i = 3; i >= 0; --i) {
        out[3 - i] = std::byte(value >> (i * 8));
    }
}

void put_u64(std::byte* out, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out[7 - i] = std::byte(value >> (i * 8));
    }
}

std::uint16_t get_u16(const std::byte* in) {
    return (std::to_integer<std::uint16_t>(in[0]) << 8) |
           std::to_integer<std::uint16_t>(in[1]);
}

std::uint32_t get_u32(const std::byte* in) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value = (value << 8) | std::to_integer<std::uint8_t>(in[i]);
    return value;
}

std::uint64_t get_u64(const std::byte* in) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | std::to_integer<std::uint8_t>(in[i]);
    return value;
}

WireFrame start_frame(WireType type, std::size_t payload_size) {
    WireFrame frame;
    frame.size = header_size + payload_size;
    frame.bytes[0] = std::byte(protocol_version);
    frame.bytes[1] = std::byte(static_cast<std::uint8_t>(type));
    put_u16(frame.bytes.data() + 2, static_cast<std::uint16_t>(payload_size));
    return frame;
}

void put_symbol(std::byte* out, Symbol symbol) {
    std::memcpy(out, symbol.bytes.data(), symbol.bytes.size());
}

Symbol get_symbol(const std::byte* in) {
    Symbol symbol;
    std::memcpy(symbol.bytes.data(), in, symbol.bytes.size());
    return symbol;
}

WireType event_wire_type(EventType type) {
    switch (type) {
    case EventType::accepted: return WireType::accepted;
    case EventType::canceled: return WireType::canceled;
    case EventType::replaced: return WireType::replaced;
    case EventType::executed: return WireType::executed;
    case EventType::rejected: return WireType::rejected;
    }
    return WireType::rejected;
}

bool event_type(std::uint8_t type, EventType& result) {
    switch (static_cast<WireType>(type)) {
    case WireType::accepted: result = EventType::accepted; return true;
    case WireType::canceled: result = EventType::canceled; return true;
    case WireType::replaced: result = EventType::replaced; return true;
    case WireType::executed: result = EventType::executed; return true;
    case WireType::rejected: result = EventType::rejected; return true;
    default: return false;
    }
}
} // namespace

Symbol Symbol::from_string(std::string_view text) {
    if (text.empty() || text.size() > 8) throw std::invalid_argument("symbol must contain 1-8 bytes");
    Symbol result;
    std::copy(text.begin(), text.end(), result.bytes.begin());
    return result;
}

std::string_view Symbol::view() const {
    std::size_t length = bytes.size();
    while (length != 0 && bytes[length - 1] == ' ') --length;
    return {bytes.data(), length};
}

bool Symbol::empty() const { return view().empty(); }

void FrameDecoder::reset() {
    buffered_ = 0;
    expected_ = 0;
}

void FrameDecoder::consume(std::span<const std::byte> input,
                           CommandCallback command_callback,
                           ProtocolErrorCallback error_callback, void* context) {
    while (!input.empty()) {
        if (expected_ == 0 && buffered_ < header_size) {
            const auto count = std::min(header_size - buffered_, input.size());
            std::memcpy(buffer_.data() + buffered_, input.data(), count);
            buffered_ += count;
            input = input.subspan(count);
            if (buffered_ < header_size) return;
            expected_ = header_size + get_u16(buffer_.data() + 2);
            if (expected_ > buffer_.size() || expected_ < header_size) {
                if (error_callback) error_callback(ProtocolError::malformed_frame, context);
                reset();
                continue;
            }
        }
        const auto count = std::min(expected_ - buffered_, input.size());
        std::memcpy(buffer_.data() + buffered_, input.data(), count);
        buffered_ += count;
        input = input.subspan(count);
        if (buffered_ == expected_) {
            parse_frame(command_callback, error_callback, context);
            reset();
        }
    }
}

void FrameDecoder::parse_frame(CommandCallback command_callback,
                               ProtocolErrorCallback error_callback, void* context) {
    if (std::to_integer<std::uint8_t>(buffer_[0]) != protocol_version) {
        if (error_callback) error_callback(ProtocolError::unsupported_version, context);
        return;
    }
    const auto type = static_cast<WireType>(std::to_integer<std::uint8_t>(buffer_[1]));
    const auto payload_size = expected_ - header_size;
    const auto* payload = buffer_.data() + header_size;
    Command command;
    switch (type) {
    case WireType::enter:
        if (payload_size != enter_payload) {
            if (error_callback) error_callback(ProtocolError::malformed_frame, context);
            return;
        }
        command.type = CommandType::enter;
        command.symbol = get_symbol(payload);
        command.order_id = get_u64(payload + 8);
        if (std::to_integer<std::uint8_t>(payload[16]) > 1) {
            if (error_callback) error_callback(ProtocolError::invalid_side, context);
            return;
        }
        command.side = std::to_integer<std::uint8_t>(payload[16]) == 0 ? Side::buy : Side::sell;
        command.price = static_cast<std::int64_t>(get_u64(payload + 17));
        command.quantity = get_u32(payload + 25);
        break;
    case WireType::cancel:
        if (payload_size != cancel_payload) {
            if (error_callback) error_callback(ProtocolError::malformed_frame, context);
            return;
        }
        command.type = CommandType::cancel;
        command.symbol = get_symbol(payload);
        command.order_id = get_u64(payload + 8);
        break;
    case WireType::replace:
        if (payload_size != replace_payload) {
            if (error_callback) error_callback(ProtocolError::malformed_frame, context);
            return;
        }
        command.type = CommandType::replace;
        command.symbol = get_symbol(payload);
        command.order_id = get_u64(payload + 8);
        command.price = static_cast<std::int64_t>(get_u64(payload + 16));
        command.quantity = get_u32(payload + 24);
        break;
    default:
        if (error_callback) error_callback(ProtocolError::unsupported_type, context);
        return;
    }
    if (command.symbol.empty()) {
        if (error_callback) error_callback(ProtocolError::symbol_required, context);
        return;
    }
    if (command_callback) command_callback(command, context);
}

WireFrame encode_enter(Symbol symbol, std::uint64_t order_id, Side side,
                       std::int64_t price, std::uint32_t quantity) {
    auto frame = start_frame(WireType::enter, enter_payload);
    auto* payload = frame.bytes.data() + header_size;
    put_symbol(payload, symbol);
    put_u64(payload + 8, order_id);
    payload[16] = std::byte(side == Side::buy ? 0 : 1);
    put_u64(payload + 17, static_cast<std::uint64_t>(price));
    put_u32(payload + 25, quantity);
    return frame;
}

WireFrame encode_cancel(Symbol symbol, std::uint64_t order_id) {
    auto frame = start_frame(WireType::cancel, cancel_payload);
    auto* payload = frame.bytes.data() + header_size;
    put_symbol(payload, symbol);
    put_u64(payload + 8, order_id);
    return frame;
}

WireFrame encode_replace(Symbol symbol, std::uint64_t order_id,
                         std::int64_t price, std::uint32_t quantity) {
    auto frame = start_frame(WireType::replace, replace_payload);
    auto* payload = frame.bytes.data() + header_size;
    put_symbol(payload, symbol);
    put_u64(payload + 8, order_id);
    put_u64(payload + 16, static_cast<std::uint64_t>(price));
    put_u32(payload + 24, quantity);
    return frame;
}

WireFrame encode_event(const Event& event) {
    auto frame = start_frame(event_wire_type(event.type), event_payload);
    auto* payload = frame.bytes.data() + header_size;
    put_u64(payload, event.sequence);
    put_symbol(payload + 8, event.symbol);
    put_u64(payload + 16, event.order_id);
    put_u64(payload + 24, event.resting_id);
    put_u64(payload + 32, static_cast<std::uint64_t>(event.price));
    put_u32(payload + 40, event.quantity);
    put_u32(payload + 44, event.remaining);
    payload[48] = std::byte((static_cast<std::uint8_t>(event.protocol_error) << 4) |
                            (static_cast<std::uint8_t>(event.status) & 0x0f));
    return frame;
}

bool decode_event(std::span<const std::byte> frame, Event& event) {
    if (frame.size() != header_size + event_payload ||
        std::to_integer<std::uint8_t>(frame[0]) != protocol_version ||
        get_u16(frame.data() + 2) != event_payload ||
        !event_type(std::to_integer<std::uint8_t>(frame[1]), event.type)) return false;
    const auto* payload = frame.data() + header_size;
    event.sequence = get_u64(payload);
    event.symbol = get_symbol(payload + 8);
    event.order_id = get_u64(payload + 16);
    event.resting_id = get_u64(payload + 24);
    event.price = static_cast<std::int64_t>(get_u64(payload + 32));
    event.quantity = get_u32(payload + 40);
    event.remaining = get_u32(payload + 44);
    auto codes = std::to_integer<std::uint8_t>(payload[48]);
    event.status = static_cast<Status>(codes & 0x0f);
    event.protocol_error = static_cast<ProtocolError>(codes >> 4);
    return true;
}

} // namespace nanobook
