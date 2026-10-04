#include "nanobook/journal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

using namespace nanobook;

#define CHECK(expr) do { if (!(expr)) { \
    std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " << #expr << '\n'; \
    std::exit(1); \
} } while (0)

namespace {

struct TemporaryFile {
    std::string path;
    TemporaryFile() {
        std::array<char, 64> name{};
        const char pattern[] = "/tmp/nanobook-journal-XXXXXX";
        std::copy(std::begin(pattern), std::end(pattern), name.begin());
        const int descriptor = ::mkstemp(name.data());
        CHECK(descriptor >= 0);
        ::close(descriptor);
        path = name.data();
    }
    ~TemporaryFile() { ::unlink(path.c_str()); }
};

struct Responses { std::vector<Event> events; };

void collect(std::span<const std::byte> wire, void* opaque) {
    Event event;
    CHECK(decode_event(wire, event));
    static_cast<Responses*>(opaque)->events.push_back(event);
}

void send(Gateway& gateway, const WireFrame& frame, Responses& responses) {
    gateway.consume({frame.bytes.data(), frame.size}, collect, &responses);
}

void replay_and_tail_repair_test() {
    TemporaryFile file;
    const auto symbol = Symbol::from_string("AAPL");
    const Config config{100, 1'000, 100, 64};

    {
        OrderBook live(config);
        SingleBookProcessor book_processor(symbol, live);
        JournaledProcessor journal(file.path, book_processor,
                                   {Durability::every_command, 1});
        Gateway gateway(journal);
        Responses responses;
        send(gateway, encode_enter(symbol, 1, Side::buy, 500, 10), responses);
        send(gateway, encode_enter(symbol, 2, Side::buy, 400, 5), responses);
        send(gateway, encode_replace(symbol, 2, 500, 7), responses);
        send(gateway, encode_cancel(symbol, 1), responses);
        CHECK(responses.events.size() == 4);
        CHECK(journal.last_sequence() == 4 && journal.healthy());
        CHECK(live.best_bid().price == 500 && live.best_bid().quantity == 7);
    }

    OrderBook recovered(config);
    SingleBookProcessor recovered_processor(symbol, recovered);
    const auto result = recover_journal(file.path, recovered_processor);
    CHECK(result.status == RecoveryStatus::clean);
    CHECK(result.records == 4 && result.last_sequence == 4);
    CHECK(result.rejected_commands == 0);
    CHECK(recovered.best_bid().price == 500 && recovered.best_bid().quantity == 7);
    CHECK(recovered.check_invariants());

    const int descriptor = ::open(file.path.c_str(), O_WRONLY | O_APPEND);
    CHECK(descriptor >= 0);
    const std::array<std::byte, 7> crash_tail{std::byte{1}, std::byte{2}, std::byte{3}};
    CHECK(::write(descriptor, crash_tail.data(), crash_tail.size()) ==
          static_cast<ssize_t>(crash_tail.size()));
    ::close(descriptor);

    OrderBook tail_recovered(config);
    SingleBookProcessor tail_processor(symbol, tail_recovered);
    const auto tail_result = recover_journal(file.path, tail_processor);
    CHECK(tail_result.status == RecoveryStatus::truncated_tail);
    CHECK(tail_result.records == 4 && tail_result.last_sequence == 4);

    {
        JournaledProcessor repaired(file.path, tail_processor,
                                    {Durability::every_command, 1});
        CHECK(repaired.last_sequence() == 4);
        Gateway gateway(repaired, tail_result.last_sequence + 1);
        Responses responses;
        send(gateway, encode_cancel(symbol, 2), responses);
        CHECK(responses.events.size() == 1);
        CHECK(responses.events[0].type == EventType::canceled);
    }

    OrderBook final_book(config);
    SingleBookProcessor final_processor(symbol, final_book);
    const auto final_result = recover_journal(file.path, final_processor);
    CHECK(final_result.status == RecoveryStatus::clean);
    CHECK(final_result.records == 5 && final_result.last_sequence == 5);
    CHECK(!final_book.best_bid().present && final_book.order_count() == 0);
}

void corruption_test() {
    TemporaryFile file;
    const auto symbol = Symbol::from_string("AMD");
    const Config config{100, 1'000, 100, 16};
    {
        OrderBook book(config);
        SingleBookProcessor processor(symbol, book);
        JournaledProcessor journal(file.path, processor);
        Gateway gateway(journal);
        Responses responses;
        send(gateway, encode_enter(symbol, 9, Side::sell, 600, 3), responses);
    }
    const int descriptor = ::open(file.path.c_str(), O_WRONLY);
    CHECK(descriptor >= 0);
    const std::byte damaged{0x7f};
    CHECK(::pwrite(descriptor, &damaged, 1, 20) == 1);
    ::close(descriptor);

    OrderBook recovered(config);
    SingleBookProcessor processor(symbol, recovered);
    const auto result = recover_journal(file.path, processor);
    CHECK(result.status == RecoveryStatus::corrupt);
    CHECK(result.records == 0 && recovered.order_count() == 0);
}

} // namespace

int main() {
    replay_and_tail_repair_test();
    corruption_test();
    std::cout << "journal tests passed\n";
}
