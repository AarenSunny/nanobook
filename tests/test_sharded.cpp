#include "nanobook/sharded_exchange.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <set>
#include <thread>
#include <vector>

using namespace nanobook;

#define CHECK(expr) do { if (!(expr)) { \
    std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " << #expr << '\n'; \
    std::exit(1); \
} } while (0)

namespace {

void ring_test() {
    SpscRing<int> ring(2);
    CHECK(ring.try_push(10));
    CHECK(ring.try_push(20));
    CHECK(!ring.try_push(30));
    int value = 0;
    CHECK(ring.try_pop(value) && value == 10);
    CHECK(ring.try_push(30));
    CHECK(ring.try_pop(value) && value == 20);
    CHECK(ring.try_pop(value) && value == 30);
    CHECK(!ring.try_pop(value));
}

void collect_event(const Event& event, void* opaque) {
    static_cast<std::vector<Event>*>(opaque)->push_back(event);
}

void recovery_path_test() {
    const std::array symbols{Symbol::from_string("AAPL"), Symbol::from_string("MSFT")};
    ShardedExchange exchange(symbols, {100, 1'000, 100, 32}, 2, 8);
    std::vector<Event> events;
    exchange.process(Command{CommandType::enter, 1, symbols[0], 1, Side::buy, 500, 4},
                     collect_event, &events);
    CHECK(events.size() == 1 && events[0].type == EventType::accepted);
    CHECK(exchange.quote(symbols[0], Side::buy).quantity == 4);
    exchange.process(Command{CommandType::enter, 2, Symbol::from_string("NOPE"),
                             2, Side::buy, 500, 1}, collect_event, &events);
    CHECK(events.back().protocol_error == ProtocolError::unknown_symbol);
    CHECK(exchange.check_invariants());
}

void multicore_routing_test() {
    const std::array symbols{
        Symbol::from_string("AAPL"), Symbol::from_string("MSFT"),
        Symbol::from_string("NVDA"), Symbol::from_string("AMD"),
        Symbol::from_string("META"), Symbol::from_string("GOOG"),
        Symbol::from_string("AMZN"), Symbol::from_string("TSLA")};
    ShardedExchange exchange(symbols, {100, 1'000, 100, 64}, 4, 128);
    std::set<std::size_t> occupied_shards;
    for (const auto symbol : symbols) occupied_shards.insert(exchange.shard_for(symbol));
    CHECK(occupied_shards.size() >= 2);
    CHECK(exchange.shard_for(Symbol::from_string("NOPE")) == exchange.shard_count());

    exchange.start();
    std::uint64_t sequence = 1;
    for (const auto symbol : symbols) {
        CHECK(exchange.try_submit(Command{CommandType::enter, sequence++, symbol,
                                          7, Side::sell, 500, 5}) == SubmitStatus::accepted);
    }
    for (const auto symbol : symbols) {
        CHECK(exchange.try_submit(Command{CommandType::enter, sequence++, symbol,
                                          8, Side::buy, 500, 5}) == SubmitStatus::accepted);
    }
    CHECK(exchange.try_submit(Command{CommandType::enter, sequence,
                                      Symbol::from_string("NOPE"), 9,
                                      Side::buy, 500, 1}) == SubmitStatus::unknown_symbol);

    std::vector<Event> events;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (events.size() < 24 && std::chrono::steady_clock::now() < deadline) {
        Event event;
        if (exchange.try_poll(event)) events.push_back(event);
        else std::this_thread::yield();
    }
    CHECK(events.size() == 24);
    std::array<std::uint32_t, 17> event_counts{};
    std::size_t executions = 0;
    for (const auto& event : events) {
        CHECK(event.sequence >= 1 && event.sequence <= 16);
        ++event_counts[event.sequence];
        if (event.type == EventType::executed) {
            ++executions;
            CHECK(event.order_id == 8 && event.resting_id == 7);
            CHECK(event.quantity == 5 && event.price == 500);
        }
    }
    for (std::size_t seq = 1; seq <= 8; ++seq) CHECK(event_counts[seq] == 1);
    for (std::size_t seq = 9; seq <= 16; ++seq) CHECK(event_counts[seq] == 2);
    CHECK(executions == 8);

    exchange.stop();
    std::uint64_t accepted = 0, processed = 0, output = 0;
    for (std::size_t index = 0; index < exchange.shard_count(); ++index) {
        const auto stats = exchange.stats(index);
        accepted += stats.accepted;
        processed += stats.processed;
        output += stats.output_events;
    }
    CHECK(accepted == 16 && processed == 16 && output == 24);
    for (const auto symbol : symbols) {
        CHECK(!exchange.quote(symbol, Side::buy).present);
        CHECK(!exchange.quote(symbol, Side::sell).present);
    }
    CHECK(exchange.check_invariants());
    CHECK(exchange.try_submit(Command{}) == SubmitStatus::stopped);
}

} // namespace

int main() {
    ring_test();
    recovery_path_test();
    multicore_routing_test();
    std::cout << "sharded exchange tests passed\n";
}
