#pragma once

#include "nanobook/gateway.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace nanobook {

enum class SubmitStatus : std::uint8_t {
    accepted,
    unknown_symbol,
    queue_full,
    stopped
};

struct ShardStats {
    std::uint64_t accepted = 0;
    std::uint64_t processed = 0;
    std::uint64_t output_events = 0;
    std::uint64_t queue_full = 0;
    std::uint64_t dropped_on_shutdown = 0;
};

// A bounded SPSC ring. Storage is allocated once during construction; push and
// pop contain no locks and perform no allocation.
template <typename T>
class SpscRing {
public:
    explicit SpscRing(std::size_t capacity)
        : storage_(std::make_unique<T[]>(capacity)), capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("ring capacity must be positive");
    }

    bool try_push(const T& value) {
        const auto head = head_.value.load(std::memory_order_relaxed);
        const auto tail = tail_.value.load(std::memory_order_acquire);
        if (head - tail == capacity_) return false;
        storage_[head % capacity_] = value;
        head_.value.store(head + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& value) {
        const auto tail = tail_.value.load(std::memory_order_relaxed);
        const auto head = head_.value.load(std::memory_order_acquire);
        if (tail == head) return false;
        value = storage_[tail % capacity_];
        tail_.value.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return tail_.value.load(std::memory_order_acquire) ==
               head_.value.load(std::memory_order_acquire);
    }

private:
    struct alignas(64) Counter { std::atomic<std::uint64_t> value{0}; };
    std::unique_ptr<T[]> storage_;
    std::size_t capacity_;
    Counter head_;
    Counter tail_;
};

// Symbols are assigned to shards once at startup. Each shard owns its books
// and is their only writer. Exactly one sequencer thread submits commands and
// one egress thread polls events; worker threads never share an OrderBook.
class ShardedExchange final : public CommandProcessor {
public:
    ShardedExchange(std::span<const Symbol> symbols, Config config,
                    std::size_t shard_count, std::size_t queue_capacity = 65'536);
    ~ShardedExchange() override;
    ShardedExchange(const ShardedExchange&) = delete;
    ShardedExchange& operator=(const ShardedExchange&) = delete;

    void start();
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    SubmitStatus try_submit(const Command& command);
    bool try_poll(Event& event);

    std::size_t shard_count() const { return shards_.size(); }
    std::size_t shard_for(Symbol symbol) const;
    ShardStats stats(std::size_t shard) const;

    // Implements CommandProcessor only for deterministic recovery before
    // start(). Live traffic must use try_submit()/try_poll().
    void process(const Command& command, EventCallback callback, void* context) override;

    Quote quote(Symbol symbol, Side side) const;
    bool check_invariants() const;

private:
    struct WorkItem {
        Command command;
        std::uint32_t book_index = 0;
    };
    struct BookSlot;
    struct Shard;
    struct Route {
        Symbol symbol;
        std::uint32_t shard = 0;
        std::uint32_t book = 0;
    };

    std::vector<std::unique_ptr<Shard>> shards_;
    std::vector<Route> routes_;
    std::atomic<bool> running_{false};
    std::size_t next_poll_shard_ = 0;

    const Route* find_route(Symbol symbol) const;
    static std::size_t hash_symbol(Symbol symbol);
    static void emit_to_shard(const Event& event, void* context);
    static void run_shard(Shard* shard, std::atomic<bool>* running);
};

} // namespace nanobook
