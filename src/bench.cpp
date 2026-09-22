#include "nanobook/feed.hpp"
#include "nanobook/order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace nanobook;

enum class Kind : std::uint8_t { add, cancel, reduce, modify, feed, count };
struct Command {
    Kind kind;
    std::uint64_t id;
    Side side;
    std::int64_t price;
    std::uint32_t quantity;
};
struct Stats {
    std::uint64_t p50 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t p999 = 0;
    std::size_t samples = 0;
};
struct Options {
    std::size_t events = 100000;
    std::size_t rounds = 3;
    std::size_t sample_every = 1;
    std::size_t capacity = 100000;
    std::size_t seed_orders = 2048;
    std::string feed_file;
    bool json = false;
};

std::size_t positive(const char* value) {
    std::size_t parsed = 0;
    try {
        std::string text(value);
        if (text.empty() || text[0] == '-') throw std::invalid_argument("negative");
        std::size_t consumed = 0;
        parsed = std::stoull(text, &consumed);
        if (consumed != text.size() || parsed == 0) throw std::invalid_argument("invalid");
    } catch (...) {
        throw std::invalid_argument(std::string("expected positive integer: ") + value);
    }
    return parsed;
}

Options parse(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        std::string flag(argv[i]);
        if (flag == "--json") result.json = true;
        else if (flag == "--help") {
            std::cout << "Usage: nanobook-bench [--events N] [--rounds N] [--sample-every N] "
                         "[--capacity N] [--seed-orders N] [--lobster message.csv] [--json]\n";
            std::exit(0);
        } else {
            if (++i == argc) throw std::invalid_argument("missing value for " + flag);
            if (flag == "--events") result.events = positive(argv[i]);
            else if (flag == "--rounds") result.rounds = positive(argv[i]);
            else if (flag == "--sample-every") result.sample_every = positive(argv[i]);
            else if (flag == "--capacity") result.capacity = positive(argv[i]);
            else if (flag == "--seed-orders") result.seed_orders = positive(argv[i]);
            else if (flag == "--lobster") result.feed_file = argv[i];
            else throw std::invalid_argument("unknown option: " + flag);
        }
    }
    return result;
}

void seed_book(OrderBook& book, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        auto side = i % 2 == 0 ? Side::buy : Side::sell;
        auto price = side == Side::buy ? 950000 + static_cast<std::int64_t>(i % 64) * 100
                                       : 1050000 + static_cast<std::int64_t>(i % 64) * 100;
        auto result = book.add_passive(1000000000000ULL + i, side, price, 100);
        if (result.status != Status::ok) throw std::runtime_error("could not seed book");
    }
}

std::vector<Command> synthetic(std::size_t count) {
    std::vector<Command> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto cycle = static_cast<std::uint64_t>(i / 8) * 4;
        switch (i % 8) {
        case 0: result.push_back({Kind::add, cycle + 1, Side::buy, 990000, 100}); break;
        case 1: result.push_back({Kind::add, cycle + 2, Side::sell, 1000000, 100}); break;
        case 2: result.push_back({Kind::add, cycle + 3, Side::buy, 1000000, 100}); break;
        case 3: result.push_back({Kind::reduce, cycle + 1, Side::buy, 0, 20}); break;
        case 4: result.push_back({Kind::modify, cycle + 1, Side::buy, 990100, 90}); break;
        case 5: result.push_back({Kind::cancel, cycle + 1, Side::buy, 0, 0}); break;
        case 6: result.push_back({Kind::add, cycle + 4, Side::sell, 1000100, 100}); break;
        case 7: result.push_back({Kind::cancel, cycle + 4, Side::sell, 0, 0}); break;
        }
    }
    return result;
}

Status apply(OrderBook& book, const Command& command) {
    switch (command.kind) {
    case Kind::add:
        return book.add(command.id, command.side, command.price, command.quantity).status;
    case Kind::cancel: return book.cancel(command.id).status;
    case Kind::reduce: return book.reduce(command.id, command.quantity).status;
    case Kind::modify:
        return book.modify(command.id, command.price, command.quantity).status;
    default: return Status::invalid_order;
    }
}

std::uint64_t nanos(Clock::duration duration) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

Stats summarize(std::vector<std::uint64_t>& samples) {
    Stats result;
    result.samples = samples.size();
    if (samples.empty()) return result;
    std::sort(samples.begin(), samples.end());
    auto percentile = [&](std::size_t numerator, std::size_t denominator) {
        auto rank = (samples.size() * numerator + denominator - 1) / denominator;
        return samples[rank == 0 ? 0 : rank - 1];
    };
    result.p50 = percentile(50, 100);
    result.p99 = percentile(99, 100);
    result.p999 = percentile(999, 1000);
    return result;
}

const char* name(Kind kind) {
    switch (kind) {
    case Kind::add: return "add";
    case Kind::cancel: return "cancel";
    case Kind::reduce: return "reduce";
    case Kind::modify: return "modify";
    case Kind::feed: return "feed_event";
    default: return "unknown";
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        auto config = Config{};
        config.max_orders = options.capacity;
        const bool replay = !options.feed_file.empty();
        if (!replay && options.seed_orders + 3 > options.capacity) {
            throw std::invalid_argument("capacity must exceed seed-orders by at least three");
        }
        std::vector<FeedEvent> feed;
        std::vector<Command> commands;
        if (replay) {
            std::ifstream input(options.feed_file);
            if (!input) throw std::runtime_error("cannot open " + options.feed_file);
            feed = read_lobster(input);
            if (feed.empty()) throw std::runtime_error("LOBSTER file is empty");
            if (feed.size() > options.events) feed.resize(options.events);
            std::int64_t min_price = std::numeric_limits<std::int64_t>::max();
            std::int64_t max_price = 0;
            for (const auto& event : feed) {
                if (event.type == 1 && event.price > 0) {
                    min_price = std::min(min_price, event.price);
                    max_price = std::max(max_price, event.price);
                }
            }
            if (max_price == 0) throw std::runtime_error("feed contains no submissions");
            config.min_price = min_price;
            config.max_price = max_price;
            config.tick_size = 1; // Exact LOBSTER price units; no guessed tick size.
        } else {
            commands = synthetic(options.events);
        }
        const auto count = replay ? feed.size() : commands.size();
        auto run = [&](OrderBook& book, ReplayCounters& counters, bool sample,
                       std::vector<std::uint64_t> samples[]) {
            std::uint64_t rejected = 0;
            for (std::size_t i = 0; i < count; ++i) {
                auto kind = replay ? Kind::feed : commands[i].kind;
                bool measure = sample && (i % options.sample_every == 0);
                auto start = measure ? Clock::now() : Clock::time_point{};
                auto status = replay ? apply_feed_event(book, feed[i], counters)
                                     : apply(book, commands[i]);
                auto finish = measure ? Clock::now() : Clock::time_point{};
                if (measure) samples[static_cast<int>(kind)].push_back(nanos(finish - start));
                if (status != Status::ok) ++rejected;
            }
            return rejected;
        };

        std::vector<double> throughputs;
        throughputs.reserve(options.rounds);
        ReplayCounters last_counters;
        std::uint64_t last_rejected = 0;
        std::vector<std::uint64_t> empty_samples[static_cast<int>(Kind::count)];
        for (std::size_t round = 0; round < options.rounds; ++round) {
            OrderBook book(config);
            if (!replay) seed_book(book, options.seed_orders);
            ReplayCounters counters;
            auto start = Clock::now();
            auto rejected = run(book, counters, false, empty_samples);
            auto elapsed = Clock::now() - start;
            if (!book.check_invariants()) throw std::runtime_error("book invariant failed");
            throughputs.push_back(static_cast<double>(count) * 1e9 / nanos(elapsed));
            last_counters = counters;
            last_rejected = rejected;
        }
        std::sort(throughputs.begin(), throughputs.end());
        double throughput = throughputs[throughputs.size() / 2];

        OrderBook latency_book(config);
        if (!replay) seed_book(latency_book, options.seed_orders);
        ReplayCounters latency_counters;
        std::vector<std::uint64_t> samples[static_cast<int>(Kind::count)];
        for (auto& sample : samples) sample.reserve(count / options.sample_every + 1);
        auto latency_rejected = run(latency_book, latency_counters, true, samples);
        if (latency_rejected != last_rejected || !latency_book.check_invariants()) {
            throw std::runtime_error("benchmark runs disagree");
        }
        std::vector<std::uint64_t> clock_samples;
        clock_samples.reserve(10000);
        for (int i = 0; i < 10000; ++i) {
            auto first = Clock::now();
            auto second = Clock::now();
            clock_samples.push_back(nanos(second - first));
        }
        auto positive_clock_tick = std::numeric_limits<std::uint64_t>::max();
        for (auto sample : clock_samples) {
            if (sample > 0) positive_clock_tick = std::min(positive_clock_tick, sample);
        }
        if (positive_clock_tick == std::numeric_limits<std::uint64_t>::max()) {
            positive_clock_tick = 0;
        }
        auto clock_stats = summarize(clock_samples);
        Stats stats[static_cast<int>(Kind::count)];
        for (int i = 0; i < static_cast<int>(Kind::count); ++i) stats[i] = summarize(samples[i]);

        if (options.json) {
            std::cout << "{\"mode\":\"" << (replay ? "lobster_replay" : "synthetic")
                      << "\",\"events\":" << count << ",\"rounds\":" << options.rounds
                      << ",\"sample_every\":" << options.sample_every
                      << ",\"capacity\":" << options.capacity
                      << ",\"seed_orders\":" << (replay ? 0 : options.seed_orders)
                      << ",\"throughput_ops_per_sec\":" << std::fixed << std::setprecision(0)
                      << throughput << ",\"clock_pair_p50_ns\":" << clock_stats.p50
                      << ",\"clock_min_positive_step_ns\":" << positive_clock_tick
                      << ",\"rejected\":" << last_rejected
                      << ",\"final_orders\":" << latency_book.order_count()
                      << ",\"matched_trades\":" << latency_book.total_trades()
                      << ",\"operations\":{";
            bool first = true;
            for (int i = 0; i < static_cast<int>(Kind::count); ++i) {
                if (stats[i].samples == 0) continue;
                if (!first) std::cout << ',';
                first = false;
                std::cout << '"' << name(static_cast<Kind>(i)) << "\":{\"samples\":"
                          << stats[i].samples << ",\"p50_ns\":" << stats[i].p50
                          << ",\"p99_ns\":" << stats[i].p99
                          << ",\"p999_ns\":" << stats[i].p999 << '}';
            }
            std::cout << "},\"feed_missing_id\":" << last_counters.missing_id
                      << ",\"feed_ignored\":" << last_counters.ignored_hidden_cross_halt
                      << "}\n";
        } else {
            std::cout << "mode: " << (replay ? "LOBSTER replay" : "synthetic matching")
                      << "\nevents: " << count << " | rounds: " << options.rounds
                      << " | sample every: " << options.sample_every
                      << " | seed orders: " << (replay ? 0 : options.seed_orders)
                      << "\nthroughput: " << std::fixed << std::setprecision(0) << throughput
                      << " ops/sec (median round, separate timing)"
                      << "\nclock pair p50: " << clock_stats.p50 << " ns"
                      << " | minimum observed positive step: " << positive_clock_tick << " ns"
                      << "\nrejected: " << last_rejected << " | final orders: "
                      << latency_book.order_count() << " | matched trades: "
                      << latency_book.total_trades() << '\n';
            for (int i = 0; i < static_cast<int>(Kind::count); ++i) {
                if (stats[i].samples == 0) continue;
                std::cout << name(static_cast<Kind>(i)) << " (n=" << stats[i].samples
                          << "): p50=" << stats[i].p50 << " ns p99=" << stats[i].p99
                          << " ns p99.9=" << stats[i].p999 << " ns\n";
            }
            if (replay) std::cout << "feed missing IDs: " << last_counters.missing_id
                                  << " | ignored hidden/cross/halt: "
                                  << last_counters.ignored_hidden_cross_halt << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
