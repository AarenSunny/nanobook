#include "nanobook/sharded_exchange.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace nanobook {

struct ShardedExchange::BookSlot {
    Symbol symbol;
    OrderBook book;
    SingleBookProcessor processor;

    BookSlot(Symbol value, Config config)
        : symbol(value), book(config), processor(symbol, book) {}
};

struct ShardedExchange::Shard {
    SpscRing<WorkItem> input;
    SpscRing<Event> output;
    std::vector<std::unique_ptr<BookSlot>> books;
    std::thread worker;
    std::atomic<bool>* running = nullptr;
    alignas(64) std::atomic<std::uint64_t> accepted{0};
    alignas(64) std::atomic<std::uint64_t> processed{0};
    alignas(64) std::atomic<std::uint64_t> output_events{0};
    alignas(64) std::atomic<std::uint64_t> queue_full{0};
    alignas(64) std::atomic<std::uint64_t> dropped_on_shutdown{0};

    Shard(std::size_t capacity, std::atomic<bool>* running_flag)
        : input(capacity), output(capacity * 4), running(running_flag) {}
};

namespace {
bool less_symbol(Symbol left, Symbol right) { return left.bytes < right.bytes; }
}

std::size_t ShardedExchange::hash_symbol(Symbol symbol) {
    std::size_t hash = sizeof(std::size_t) == 8 ? 1469598103934665603ULL : 2166136261U;
    const std::size_t prime = sizeof(std::size_t) == 8 ? 1099511628211ULL : 16777619U;
    for (const auto byte : symbol.bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= prime;
    }
    return hash;
}

ShardedExchange::ShardedExchange(std::span<const Symbol> symbols, Config config,
                                 std::size_t shard_count, std::size_t queue_capacity) {
    if (symbols.empty()) throw std::invalid_argument("at least one symbol is required");
    if (shard_count == 0) throw std::invalid_argument("at least one shard is required");
    shards_.reserve(shard_count);
    for (std::size_t index = 0; index < shard_count; ++index) {
        shards_.push_back(std::make_unique<Shard>(queue_capacity, &running_));
    }
    routes_.reserve(symbols.size());
    for (const auto symbol : symbols) {
        if (symbol.empty()) throw std::invalid_argument("empty symbol");
        const auto shard_index = hash_symbol(symbol) % shard_count;
        auto& shard = *shards_[shard_index];
        const auto book_index = static_cast<std::uint32_t>(shard.books.size());
        shard.books.push_back(std::make_unique<BookSlot>(symbol, config));
        routes_.push_back({symbol, static_cast<std::uint32_t>(shard_index), book_index});
    }
    std::sort(routes_.begin(), routes_.end(),
              [](const Route& left, const Route& right) { return less_symbol(left.symbol, right.symbol); });
    for (std::size_t index = 1; index < routes_.size(); ++index) {
        if (routes_[index - 1].symbol == routes_[index].symbol) {
            throw std::invalid_argument("duplicate symbol");
        }
    }
}

ShardedExchange::~ShardedExchange() { stop(); }

const ShardedExchange::Route* ShardedExchange::find_route(Symbol symbol) const {
    const auto iterator = std::lower_bound(
        routes_.begin(), routes_.end(), symbol,
        [](const Route& route, Symbol value) { return less_symbol(route.symbol, value); });
    return iterator != routes_.end() && iterator->symbol == symbol ? &*iterator : nullptr;
}

void ShardedExchange::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    for (auto& shard : shards_) {
        shard->worker = std::thread(run_shard, shard.get(), &running_);
    }
}

void ShardedExchange::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    for (auto& shard : shards_) {
        if (shard->worker.joinable()) shard->worker.join();
    }
}

SubmitStatus ShardedExchange::try_submit(const Command& command) {
    if (!running()) return SubmitStatus::stopped;
    const auto* route = find_route(command.symbol);
    if (!route) return SubmitStatus::unknown_symbol;
    auto& shard = *shards_[route->shard];
    if (!shard.input.try_push({command, route->book})) {
        shard.queue_full.fetch_add(1, std::memory_order_relaxed);
        return SubmitStatus::queue_full;
    }
    shard.accepted.fetch_add(1, std::memory_order_relaxed);
    return SubmitStatus::accepted;
}

bool ShardedExchange::try_poll(Event& event) {
    for (std::size_t count = 0; count < shards_.size(); ++count) {
        const auto index = (next_poll_shard_ + count) % shards_.size();
        if (shards_[index]->output.try_pop(event)) {
            next_poll_shard_ = (index + 1) % shards_.size();
            return true;
        }
    }
    return false;
}

std::size_t ShardedExchange::shard_for(Symbol symbol) const {
    const auto* route = find_route(symbol);
    return route ? route->shard : shards_.size();
}

ShardStats ShardedExchange::stats(std::size_t shard) const {
    if (shard >= shards_.size()) throw std::out_of_range("shard index");
    const auto& value = *shards_[shard];
    return {value.accepted.load(std::memory_order_relaxed),
            value.processed.load(std::memory_order_relaxed),
            value.output_events.load(std::memory_order_relaxed),
            value.queue_full.load(std::memory_order_relaxed),
            value.dropped_on_shutdown.load(std::memory_order_relaxed)};
}

void ShardedExchange::emit_to_shard(const Event& event, void* opaque) {
    auto& shard = *static_cast<Shard*>(opaque);
    while (!shard.output.try_push(event)) {
        if (!shard.running->load(std::memory_order_acquire)) {
            shard.dropped_on_shutdown.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::this_thread::yield();
    }
    shard.output_events.fetch_add(1, std::memory_order_relaxed);
}

void ShardedExchange::run_shard(Shard* shard, std::atomic<bool>* running) {
    WorkItem item;
    while (running->load(std::memory_order_acquire) || !shard->input.empty()) {
        if (!shard->input.try_pop(item)) {
            std::this_thread::yield();
            continue;
        }
        shard->books[item.book_index]->processor.process(item.command, emit_to_shard, shard);
        shard->processed.fetch_add(1, std::memory_order_relaxed);
    }
}

void ShardedExchange::process(const Command& command, EventCallback callback, void* context) {
    if (running()) {
        if (callback) {
            callback(Event{EventType::rejected, command.sequence, command.symbol,
                           command.order_id, 0, command.price, command.quantity, 0,
                           Status::invalid_order, ProtocolError::none}, context);
        }
        return;
    }
    const auto* route = find_route(command.symbol);
    if (!route) {
        if (callback) {
            callback(Event{EventType::rejected, command.sequence, command.symbol,
                           command.order_id, 0, command.price, command.quantity, 0,
                           Status::invalid_order, ProtocolError::unknown_symbol}, context);
        }
        return;
    }
    shards_[route->shard]->books[route->book]->processor.process(command, callback, context);
}

Quote ShardedExchange::quote(Symbol symbol, Side side) const {
    if (running()) throw std::logic_error("quotes require a stopped exchange");
    const auto* route = find_route(symbol);
    if (!route) return {};
    const auto& book = shards_[route->shard]->books[route->book]->book;
    return side == Side::buy ? book.best_bid() : book.best_ask();
}

bool ShardedExchange::check_invariants() const {
    if (running()) return false;
    for (const auto& shard : shards_) {
        for (const auto& slot : shard->books) {
            if (!slot->book.check_invariants()) return false;
        }
    }
    return true;
}

} // namespace nanobook
