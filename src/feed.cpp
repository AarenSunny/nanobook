#include "nanobook/feed.hpp"

#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nanobook {
namespace {
template <typename T>
T number(std::string_view field, std::size_t row) {
    while (!field.empty() && field.front() == ' ') field.remove_prefix(1);
    while (!field.empty() && (field.back() == ' ' || field.back() == '\r')) field.remove_suffix(1);
    T result{};
    auto parsed = std::from_chars(field.data(), field.data() + field.size(), result);
    if (field.empty() || parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size()) {
        throw std::runtime_error("invalid LOBSTER number at line " + std::to_string(row));
    }
    return result;
}
} // namespace

std::vector<FeedEvent> read_lobster(std::istream& input) {
    std::vector<FeedEvent> events;
    std::string line;
    std::size_t row = 0;
    while (std::getline(input, line)) {
        ++row;
        if (line.empty()) continue;
        std::string_view fields[6];
        std::string_view remaining(line);
        for (int i = 0; i < 5; ++i) {
            auto comma = remaining.find(',');
            if (comma == std::string_view::npos) {
                throw std::runtime_error("expected six CSV columns at line " + std::to_string(row));
            }
            fields[i] = remaining.substr(0, comma);
            remaining.remove_prefix(comma + 1);
        }
        if (remaining.find(',') != std::string_view::npos) {
            throw std::runtime_error("expected six CSV columns at line " + std::to_string(row));
        }
        fields[5] = remaining;
        // Validate the timestamp even though only event order is needed for replay.
        auto timestamp = fields[0];
        while (!timestamp.empty() && timestamp.front() == ' ') timestamp.remove_prefix(1);
        if (timestamp.empty() || timestamp.find_first_not_of("0123456789.") != std::string_view::npos) {
            throw std::runtime_error("invalid timestamp at line " + std::to_string(row));
        }
        auto type = number<int>(fields[1], row);
        if (type < 1 || type > 7) throw std::runtime_error("invalid event type at line " + std::to_string(row));
        auto id = number<std::uint64_t>(fields[2], row);
        auto quantity = number<std::uint32_t>(fields[3], row);
        auto price = number<std::int64_t>(fields[4], row);
        auto direction = number<int>(fields[5], row);
        if (direction != 1 && direction != -1) {
            throw std::runtime_error("invalid direction at line " + std::to_string(row));
        }
        events.push_back({static_cast<std::uint8_t>(type), id, quantity, price,
                          direction == 1 ? Side::buy : Side::sell});
    }
    return events;
}

Status apply_feed_event(OrderBook& book, const FeedEvent& event,
                        ReplayCounters& counters) {
    Status status = Status::ok;
    switch (event.type) {
    case 1:
        ++counters.submissions;
        status = book.add_passive(event.id, event.side, event.price, event.quantity).status;
        break;
    case 2:
        ++counters.cancellations;
        status = book.reduce(event.id, event.quantity).status;
        break;
    case 3:
        ++counters.deletions;
        status = book.cancel(event.id).status;
        break;
    case 4:
        ++counters.visible_executions;
        status = book.reduce(event.id, event.quantity).status;
        break;
    case 5: case 6: case 7:
        ++counters.ignored_hidden_cross_halt;
        break;
    default:
        status = Status::invalid_order;
    }
    if (status != Status::ok) {
        ++counters.rejected;
        if (status == Status::unknown_id) ++counters.missing_id;
    }
    return status;
}

} // namespace nanobook
