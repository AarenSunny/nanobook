#pragma once

#include "nanobook/order_book.hpp"

#include <cstdint>
#include <istream>
#include <vector>

namespace nanobook {

struct FeedEvent {
    std::uint8_t type = 0;
    std::uint64_t id = 0;
    std::uint32_t quantity = 0;
    std::int64_t price = 0;
    Side side = Side::buy;
};

struct ReplayCounters {
    std::uint64_t submissions = 0;
    std::uint64_t cancellations = 0;
    std::uint64_t deletions = 0;
    std::uint64_t visible_executions = 0;
    std::uint64_t ignored_hidden_cross_halt = 0;
    std::uint64_t rejected = 0;
    std::uint64_t missing_id = 0;
};

// Parses LOBSTER's six-column message CSV. Throws on malformed records.
std::vector<FeedEvent> read_lobster(std::istream& input);
Status apply_feed_event(OrderBook& book, const FeedEvent& event,
                        ReplayCounters& counters);

} // namespace nanobook
