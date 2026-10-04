#include "nanobook/gateway.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

using namespace nanobook;

#define CHECK(expr) do { if (!(expr)) { \
    std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " << #expr << '\n'; \
    std::exit(1); \
} } while (0)

namespace {

struct Responses {
    std::vector<Event> events;
};

void collect_wire(std::span<const std::byte> wire, void* opaque) {
    Event event;
    CHECK(decode_event(wire, event));
    static_cast<Responses*>(opaque)->events.push_back(event);
}

void feed_one_byte_at_a_time(Gateway& gateway, const WireFrame& frame, Responses& responses) {
    for (std::size_t i = 0; i < frame.size; ++i) {
        gateway.consume({frame.bytes.data() + i, 1}, collect_wire, &responses);
    }
}

void framing_and_matching_test() {
    const auto symbol = Symbol::from_string("AAPL");
    CHECK(symbol.view() == "AAPL");
    OrderBook book({1, 1'000, 1, 32});
    SingleBookProcessor processor(symbol, book);
    Gateway gateway(processor);
    Responses responses;

    const auto sell = encode_enter(symbol, 100, Side::sell, 101, 7);
    feed_one_byte_at_a_time(gateway, sell, responses);
    CHECK(responses.events.size() == 1);
    CHECK(responses.events[0].type == EventType::accepted);
    CHECK(responses.events[0].sequence == 1);
    CHECK(responses.events[0].remaining == 7);

    const auto buy = encode_enter(symbol, 200, Side::buy, 101, 4);
    gateway.consume({buy.bytes.data(), buy.size}, collect_wire, &responses);
    CHECK(responses.events.size() == 3);
    const auto& execution = responses.events[1];
    CHECK(execution.type == EventType::executed);
    CHECK(execution.sequence == 2 && execution.order_id == 200);
    CHECK(execution.resting_id == 100 && execution.price == 101 && execution.quantity == 4);
    CHECK(responses.events[2].type == EventType::accepted);
    CHECK(responses.events[2].quantity == 4 && responses.events[2].remaining == 0);
    CHECK(book.best_ask().quantity == 3);
    CHECK(gateway.next_sequence() == 3);
}

void cancel_replace_and_batch_test() {
    const auto symbol = Symbol::from_string("MSFT");
    OrderBook book({1, 1'000, 1, 32});
    SingleBookProcessor processor(symbol, book);
    Gateway gateway(processor, 40);
    Responses responses;

    auto enter = encode_enter(symbol, 1, Side::buy, 500, 10);
    auto replace = encode_replace(symbol, 1, 499, 6);
    std::array<std::byte, WireFrame::capacity * 2> batch{};
    std::copy_n(enter.bytes.begin(), enter.size, batch.begin());
    std::copy_n(replace.bytes.begin(), replace.size, batch.begin() + enter.size);
    gateway.consume({batch.data(), enter.size + replace.size}, collect_wire, &responses);

    CHECK(responses.events.size() == 2);
    CHECK(responses.events[0].type == EventType::accepted && responses.events[0].sequence == 40);
    CHECK(responses.events[1].type == EventType::replaced && responses.events[1].sequence == 41);
    CHECK(book.best_bid().price == 499 && book.best_bid().quantity == 6);

    auto cancel = encode_cancel(symbol, 1);
    gateway.consume({cancel.bytes.data(), cancel.size}, collect_wire, &responses);
    CHECK(responses.events.back().type == EventType::canceled);
    CHECK(responses.events.back().sequence == 42);
    CHECK(!book.best_bid().present);

    gateway.consume({cancel.bytes.data(), cancel.size}, collect_wire, &responses);
    CHECK(responses.events.back().type == EventType::rejected);
    CHECK(responses.events.back().status == Status::unknown_id);
}

void rejection_test() {
    const auto symbol = Symbol::from_string("NVDA");
    OrderBook book({1, 1'000, 1, 16});
    SingleBookProcessor processor(symbol, book);
    Gateway gateway(processor);
    Responses responses;

    auto wrong_symbol = encode_enter(Symbol::from_string("AMD"), 1, Side::buy, 100, 1);
    gateway.consume({wrong_symbol.bytes.data(), wrong_symbol.size}, collect_wire, &responses);
    CHECK(responses.events.back().protocol_error == ProtocolError::unknown_symbol);

    auto invalid_side = encode_enter(symbol, 2, Side::buy, 100, 1);
    invalid_side.bytes[4 + 16] = std::byte{9};
    gateway.consume({invalid_side.bytes.data(), invalid_side.size}, collect_wire, &responses);
    CHECK(responses.events.back().protocol_error == ProtocolError::invalid_side);

    auto wrong_version = encode_cancel(symbol, 1);
    wrong_version.bytes[0] = std::byte{protocol_version + 1};
    gateway.consume({wrong_version.bytes.data(), wrong_version.size}, collect_wire, &responses);
    CHECK(responses.events.back().protocol_error == ProtocolError::unsupported_version);

    auto unknown_type = encode_cancel(symbol, 1);
    unknown_type.bytes[1] = std::byte{'?'};
    gateway.consume({unknown_type.bytes.data(), unknown_type.size}, collect_wire, &responses);
    CHECK(responses.events.back().protocol_error == ProtocolError::unsupported_type);

    std::array<std::byte, 4> oversized{std::byte{protocol_version}, std::byte{'O'},
                                        std::byte{0xff}, std::byte{0xff}};
    gateway.consume(oversized, collect_wire, &responses);
    CHECK(responses.events.back().protocol_error == ProtocolError::malformed_frame);

    CHECK(responses.events.size() == 5);
    for (std::size_t i = 0; i < responses.events.size(); ++i) {
        CHECK(responses.events[i].type == EventType::rejected);
        CHECK(responses.events[i].sequence == i + 1);
    }
}

void session_reset_test() {
    const auto symbol = Symbol::from_string("META");
    OrderBook book({1, 1'000, 1, 16});
    SingleBookProcessor processor(symbol, book);
    Gateway gateway(processor);
    Responses responses;
    auto first = encode_enter(symbol, 1, Side::buy, 100, 1);
    gateway.consume({first.bytes.data(), 2}, collect_wire, &responses);
    gateway.reset_session();
    auto second = encode_enter(symbol, 2, Side::buy, 100, 1);
    gateway.consume({second.bytes.data(), second.size}, collect_wire, &responses);
    CHECK(responses.events.size() == 1);
    CHECK(responses.events[0].sequence == 1 && responses.events[0].order_id == 2);
}

} // namespace

int main() {
    framing_and_matching_test();
    cancel_replace_and_batch_test();
    rejection_test();
    session_reset_test();
    std::cout << "gateway tests passed\n";
}
