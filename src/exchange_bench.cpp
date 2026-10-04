#include "nanobook/journal.hpp"
#include "nanobook/sharded_exchange.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace nanobook;

namespace {

struct Options {
    std::uint64_t events = 1'000'000;
    std::uint64_t warmup = 50'000;
    std::uint64_t rate = 0;
    std::size_t symbols = 64;
    std::size_t shards = 1;
    std::size_t queue_capacity = 65'536;
    std::optional<std::string> journal_path;
    std::uint32_t sync_every = 0;
    bool json = false;
};

std::uint64_t parse_number(std::string_view value, std::string_view name) {
    if (value.empty() || value.front() == '-') throw std::invalid_argument(std::string(name));
    std::string text(value);
    char* end = nullptr;
    const auto number = std::strtoull(text.c_str(), &end, 10);
    if (end != text.c_str() + text.size()) throw std::invalid_argument(std::string(name));
    return number;
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view name = argv[index];
        auto value = [&]() -> std::string_view {
            if (++index == argc) throw std::invalid_argument("missing value for " + std::string(name));
            return argv[index];
        };
        if (name == "--events") options.events = parse_number(value(), name);
        else if (name == "--warmup") options.warmup = parse_number(value(), name);
        else if (name == "--rate") options.rate = parse_number(value(), name);
        else if (name == "--symbols") options.symbols = parse_number(value(), name);
        else if (name == "--shards") options.shards = parse_number(value(), name);
        else if (name == "--queue") options.queue_capacity = parse_number(value(), name);
        else if (name == "--journal") options.journal_path = std::string(value());
        else if (name == "--sync-every") options.sync_every = parse_number(value(), name);
        else if (name == "--json") options.json = true;
        else throw std::invalid_argument("unknown option " + std::string(name));
    }
    if (options.events == 0 || options.symbols == 0 || options.shards == 0 ||
        options.queue_capacity == 0 || options.shards > options.symbols) {
        throw std::invalid_argument("events, symbols, shards, and queue must be positive; shards <= symbols");
    }
    if (options.symbols > 9'999'999 || options.sync_every > UINT32_MAX) {
        throw std::invalid_argument("symbols or sync interval exceeds supported range");
    }
    if (options.sync_every != 0 && !options.journal_path) {
        throw std::invalid_argument("--sync-every requires --journal");
    }
    return options;
}

std::vector<Symbol> make_symbols(std::size_t count) {
    std::vector<Symbol> symbols;
    symbols.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        char text[9]{};
        std::snprintf(text, sizeof(text), "S%07zu", index);
        symbols.push_back(Symbol::from_string(text));
    }
    return symbols;
}

class NoopProcessor final : public CommandProcessor {
public:
    void process(const Command&, EventCallback, void*) override {}
};

struct Distribution {
    std::uint64_t count = 0;
    double p50 = 0;
    double p99 = 0;
    double p999 = 0;
    double p9999 = 0;
    double maximum = 0;
};

Distribution summarize(std::vector<std::uint64_t> samples) {
    if (samples.empty()) return {};
    std::sort(samples.begin(), samples.end());
    auto percentile = [&](double fraction) {
        const auto rank = static_cast<std::size_t>(
            std::ceil(fraction * static_cast<double>(samples.size())));
        return static_cast<double>(samples[std::max<std::size_t>(1, rank) - 1]);
    };
    return {samples.size(), percentile(0.50), percentile(0.99),
            percentile(0.999), percentile(0.9999),
            static_cast<double>(samples.back())};
}

const char* operation_name(std::size_t index) {
    constexpr std::array names{"enter", "cancel", "replace"};
    return names[index];
}

struct PhaseResult {
    double seconds = 0;
    double throughput = 0;
    Distribution overall;
    std::array<Distribution, 3> operations;
};

class Runner {
public:
    Runner(const Options& options, std::vector<Symbol> symbols,
           ShardedExchange& exchange, JournaledProcessor* journal)
        : options_(options), symbols_(std::move(symbols)), exchange_(exchange),
          journal_(journal), state_(symbols_.size()), ids_(symbols_.size(), 1) {
        next_sequence_ = journal_ ? journal_->last_sequence() + 1 : 1;
    }

    PhaseResult run(std::uint64_t count, bool record) {
        first_sequence_ = next_sequence_;
        phase_count_ = count;
        completed_.store(0, std::memory_order_relaxed);
        abort_.store(false, std::memory_order_relaxed);
        record_ = record;
        starts_.assign(count, Clock::time_point{});
        types_.assign(count, CommandType::enter);
        overall_.clear();
        for (auto& values : by_operation_) values.clear();
        if (record) {
            overall_.reserve(count);
            for (auto& values : by_operation_) values.reserve(count);
        }

        decoder_.reset();
        const auto phase_start = Clock::now();
        std::exception_ptr egress_error;
        std::thread egress([&] {
            try {
                while (!abort_.load(std::memory_order_acquire) &&
                       completed_.load(std::memory_order_acquire) != count) {
                    if (!drain_one()) std::this_thread::yield();
                }
            } catch (...) {
                egress_error = std::current_exception();
                abort_.store(true, std::memory_order_release);
            }
        });
        try {
            for (std::uint64_t index = 0; index < count; ++index) {
                if (options_.rate != 0) {
                    const auto offset = std::chrono::nanoseconds(
                        (index * 1'000'000'000ULL) / options_.rate);
                    scheduled_start_ = phase_start + offset;
                    wait_until(scheduled_start_);
                } else {
                    scheduled_start_ = Clock::now();
                }
                const auto command = next_workload_command(index % symbols_.size());
                WireFrame frame;
                if (command.type == CommandType::enter) {
                    frame = encode_enter(command.symbol, command.order_id, command.side,
                                         command.price, command.quantity);
                } else if (command.type == CommandType::cancel) {
                    frame = encode_cancel(command.symbol, command.order_id);
                } else {
                    frame = encode_replace(command.symbol, command.order_id,
                                           command.price, command.quantity);
                }
                decoder_.consume({frame.bytes.data(), frame.size}, on_command, on_decode_error, this);
            }
        } catch (...) {
            abort_.store(true, std::memory_order_release);
            egress.join();
            throw;
        }
        egress.join();
        if (egress_error) std::rethrow_exception(egress_error);
        const auto phase_end = Clock::now();
        const double seconds = std::chrono::duration<double>(phase_end - phase_start).count();
        PhaseResult result;
        result.seconds = seconds;
        result.throughput = static_cast<double>(count) / seconds;
        if (record) {
            result.overall = summarize(overall_);
            for (std::size_t index = 0; index < by_operation_.size(); ++index) {
                result.operations[index] = summarize(by_operation_[index]);
            }
        }
        return result;
    }

private:
    const Options& options_;
    std::vector<Symbol> symbols_;
    ShardedExchange& exchange_;
    JournaledProcessor* journal_;
    FrameDecoder decoder_;
    std::vector<std::uint8_t> state_;
    std::vector<std::uint64_t> ids_;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t first_sequence_ = 1;
    std::uint64_t phase_count_ = 0;
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<bool> abort_{false};
    bool record_ = false;
    Clock::time_point scheduled_start_{};
    std::vector<Clock::time_point> starts_;
    std::vector<CommandType> types_;
    std::vector<std::uint64_t> overall_;
    std::array<std::vector<std::uint64_t>, 3> by_operation_;

    Command next_workload_command(std::size_t symbol_index) {
        auto& step = state_[symbol_index];
        auto& id = ids_[symbol_index];
        Command command;
        command.symbol = symbols_[symbol_index];
        switch (step) {
        case 0:
            command = {CommandType::enter, 0, command.symbol, id,
                       Side::buy, 500, 10};
            break;
        case 1:
            command = {CommandType::replace, 0, command.symbol, id,
                       Side::buy, 500, 8};
            break;
        case 2:
            command = {CommandType::cancel, 0, command.symbol, id,
                       Side::buy, 0, 0};
            break;
        case 3:
            command = {CommandType::enter, 0, command.symbol, id + 1,
                       Side::sell, 600, 5};
            break;
        default:
            command = {CommandType::enter, 0, command.symbol, id + 2,
                       Side::buy, 600, 5};
            id += 3;
            break;
        }
        step = static_cast<std::uint8_t>((step + 1) % 5);
        return command;
    }

    static void wait_until(Clock::time_point target) {
        while (true) {
            const auto now = Clock::now();
            if (now >= target) return;
            if (target - now > std::chrono::microseconds(100)) {
                std::this_thread::sleep_for(target - now - std::chrono::microseconds(50));
            } else {
                std::this_thread::yield();
            }
        }
    }

    static void on_command(const Command& decoded, void* opaque) {
        static_cast<Runner*>(opaque)->submit(decoded);
    }

    static void on_decode_error(ProtocolError, void*) {
        throw std::runtime_error("benchmark generated an invalid frame");
    }

    void submit(const Command& decoded) {
        auto command = decoded;
        command.sequence = next_sequence_++;
        const auto index = command.sequence - first_sequence_;
        if (index >= phase_count_) throw std::logic_error("sequence escaped benchmark phase");
        starts_[index] = scheduled_start_;
        types_[index] = command.type;
        if (journal_) {
            journal_->process(command, nullptr, nullptr);
            if (!journal_->healthy()) throw std::runtime_error("journal write failed");
        }
        while (true) {
            const auto status = exchange_.try_submit(command);
            if (status == SubmitStatus::accepted) break;
            if (status != SubmitStatus::queue_full) throw std::runtime_error("submit failed");
            if (abort_.load(std::memory_order_acquire)) throw std::runtime_error("egress failed");
            std::this_thread::yield();
        }
    }

    bool drain_one() {
        Event event;
        if (!exchange_.try_poll(event)) return false;
        const auto wire = encode_event(event);
        Event decoded;
        if (!decode_event({wire.bytes.data(), wire.size}, decoded)) {
            throw std::runtime_error("response encoding failed");
        }
        if (decoded.type == EventType::executed) return true;
        if (decoded.sequence < first_sequence_ ||
            decoded.sequence >= first_sequence_ + phase_count_) {
            throw std::logic_error("response escaped benchmark phase");
        }
        const auto index = decoded.sequence - first_sequence_;
        if (record_) {
            const auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - starts_[index]).count();
            const auto value = static_cast<std::uint64_t>(std::max<std::int64_t>(0, latency));
            overall_.push_back(value);
            by_operation_[static_cast<std::size_t>(types_[index])].push_back(value);
        }
        completed_.fetch_add(1, std::memory_order_release);
        return true;
    }
};

void print_distribution(std::string_view name, const Distribution& value) {
    std::cout << std::left << std::setw(9) << name << std::right
              << " n=" << std::setw(9) << value.count
              << " p50=" << std::setw(9) << static_cast<std::uint64_t>(value.p50)
              << " p99=" << std::setw(9) << static_cast<std::uint64_t>(value.p99)
              << " p99.9=" << std::setw(9) << static_cast<std::uint64_t>(value.p999)
              << " p99.99=" << std::setw(9) << static_cast<std::uint64_t>(value.p9999)
              << " max=" << static_cast<std::uint64_t>(value.maximum) << " ns\n";
}

void print_json(const Options& options, const PhaseResult& result,
                const ShardedExchange& exchange) {
    auto distribution = [](const Distribution& value) {
        std::cout << "{\"count\":" << value.count
                  << ",\"p50_ns\":" << static_cast<std::uint64_t>(value.p50)
                  << ",\"p99_ns\":" << static_cast<std::uint64_t>(value.p99)
                  << ",\"p999_ns\":" << static_cast<std::uint64_t>(value.p999)
                  << ",\"p9999_ns\":" << static_cast<std::uint64_t>(value.p9999)
                  << ",\"max_ns\":" << static_cast<std::uint64_t>(value.maximum) << '}';
    };
    std::uint64_t queue_full = 0;
    for (std::size_t index = 0; index < exchange.shard_count(); ++index) {
        queue_full += exchange.stats(index).queue_full;
    }
    std::cout << "{\"events\":" << options.events
              << ",\"symbols\":" << options.symbols
              << ",\"shards\":" << options.shards
              << ",\"target_rate\":" << options.rate
              << ",\"seconds\":" << result.seconds
              << ",\"throughput\":" << result.throughput
              << ",\"queue_full\":" << queue_full
              << ",\"overall\":";
    distribution(result.overall);
    std::cout << ",\"operations\":{";
    for (std::size_t index = 0; index < result.operations.size(); ++index) {
        if (index != 0) std::cout << ',';
        std::cout << '"' << operation_name(index) << "\":";
        distribution(result.operations[index]);
    }
    std::cout << "}}\n";
}

} // namespace

int main(int argc, char** argv) try {
    const auto options = parse_options(argc, argv);
    auto symbols = make_symbols(options.symbols);
    ShardedExchange exchange(symbols, {100, 1'000, 100, 64},
                             options.shards, options.queue_capacity);
    NoopProcessor noop;
    std::unique_ptr<JournaledProcessor> journal;
    if (options.journal_path) {
        JournalOptions journal_options;
        if (options.sync_every == 1) journal_options.durability = Durability::every_command;
        else if (options.sync_every > 1) {
            journal_options.durability = Durability::batch;
            journal_options.batch_size = options.sync_every;
        }
        journal = std::make_unique<JournaledProcessor>(*options.journal_path, noop, journal_options);
    }
    exchange.start();
    Runner runner(options, symbols, exchange, journal.get());
    if (options.warmup != 0) runner.run(options.warmup, false);
    const auto result = runner.run(options.events, true);
    if (journal && !journal->flush()) throw std::runtime_error("journal flush failed");
    exchange.stop();

    if (options.json) {
        print_json(options, result, exchange);
    } else {
        std::cout << "Nanobook pipeline benchmark\n"
                  << "mode: " << (options.rate == 0 ? "saturated" : "open-loop")
                  << ", events: " << options.events
                  << ", symbols: " << options.symbols
                  << ", shards: " << options.shards
                  << ", hardware threads: " << std::thread::hardware_concurrency() << '\n'
                  << std::fixed << std::setprecision(0)
                  << "throughput: " << result.throughput << " commands/sec\n";
        print_distribution("overall", result.overall);
        for (std::size_t index = 0; index < result.operations.size(); ++index) {
            print_distribution(operation_name(index), result.operations[index]);
        }
        std::uint64_t queue_full = 0;
        for (std::size_t index = 0; index < exchange.shard_count(); ++index) {
            const auto stats = exchange.stats(index);
            queue_full += stats.queue_full;
            std::cout << "shard " << index << ": accepted=" << stats.accepted
                      << " processed=" << stats.processed
                      << " events=" << stats.output_events << '\n';
        }
        std::cout << "input queue full retries: " << queue_full << '\n';
    }
    return exchange.check_invariants() ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "benchmark error: " << error.what() << '\n';
    return 1;
}
