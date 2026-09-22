#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nanobook {

enum class Side : std::uint8_t { buy, sell };
enum class Status : std::uint8_t {
    ok, invalid_order, duplicate_id, unknown_id, invalid_price, capacity_exceeded
};

struct Trade {
    std::uint64_t incoming_id;
    std::uint64_t resting_id;
    std::int64_t price;
    std::uint32_t quantity;
};

using TradeCallback = void (*)(const Trade&, void*);

struct Result {
    Status status = Status::ok;
    std::uint64_t executed_quantity = 0;
    std::uint32_t trade_count = 0;
    std::uint32_t resting_quantity = 0;
};

struct Quote {
    std::int64_t price = 0;
    std::uint64_t quantity = 0;
    bool present = false;
};

struct Config {
    std::int64_t min_price = 500000;    // $50, in units of $0.0001
    std::int64_t max_price = 2500000;   // $250
    std::int64_t tick_size = 100;       // $0.01
    std::size_t max_orders = 100000;
};

// Single-threaded engine. Construction allocates every table and order slot.
// All public order operations perform no heap allocation after construction.
class OrderBook {
public:
    explicit OrderBook(Config config);

    Result add(std::uint64_t id, Side side, std::int64_t price,
               std::uint32_t quantity, TradeCallback callback = nullptr,
               void* context = nullptr);
    // Feed replay inserts the displayed order without synthesizing a trade.
    Result add_passive(std::uint64_t id, Side side, std::int64_t price,
                       std::uint32_t quantity);
    Result cancel(std::uint64_t id);
    Result reduce(std::uint64_t id, std::uint32_t quantity);
    // Same-price decreases preserve priority. Increases and reprices cancel-replace.
    Result modify(std::uint64_t id, std::int64_t price, std::uint32_t quantity,
                  TradeCallback callback = nullptr, void* context = nullptr);

    Quote best_bid() const;
    Quote best_ask() const;
    std::size_t order_count() const { return live_orders_; }
    std::uint64_t total_traded() const { return total_traded_; }
    std::uint64_t total_trades() const { return total_trades_; }
    bool contains(std::uint64_t id) const;
    bool check_invariants() const;

private:
    static constexpr std::uint32_t none = UINT32_MAX;
    struct Order {
        std::uint64_t id = 0;
        std::uint32_t quantity = 0;
        std::uint32_t prev = none;
        std::uint32_t next = none;
        std::uint32_t price_index = 0;
        Side side = Side::buy;
    };
    struct Level {
        std::uint32_t head = none;
        std::uint32_t tail = none;
        std::uint64_t quantity = 0;
        std::uint32_t orders = 0;
    };
    struct IdSlot {
        std::uint64_t key = 0;
        std::uint32_t index = none;
        std::uint8_t state = 0; // 0 empty, 1 occupied
    };

    Config config_;
    std::size_t level_count_;
    std::vector<Level> bids_;
    std::vector<Level> asks_;
    std::vector<std::uint64_t> bid_bits_;
    std::vector<std::uint64_t> ask_bits_;
    std::vector<Order> orders_;
    std::vector<IdSlot> ids_;
    std::uint32_t next_unused_ = 0;
    std::uint32_t free_head_ = none;
    std::size_t live_orders_ = 0;
    std::uint64_t total_traded_ = 0;
    std::uint64_t total_trades_ = 0;
    std::uint32_t best_bid_index_ = none;
    std::uint32_t best_ask_index_ = none;

    bool price_index(std::int64_t price, std::uint32_t& index) const;
    std::int64_t price_at(std::uint32_t index) const;
    Level& level(Side side, std::uint32_t index);
    const Level& level(Side side, std::uint32_t index) const;
    void set_occupied(Side side, std::uint32_t index, bool occupied);
    std::uint32_t best_index(Side side) const;
    std::uint32_t scan_best(Side side) const;
    bool can_fully_match(Side incoming_side, std::uint32_t limit_index,
                         std::uint32_t quantity) const;
    std::size_t id_position(std::uint64_t id) const;
    std::size_t insertion_position(std::uint64_t id) const;
    std::uint32_t find(std::uint64_t id) const;
    void insert_id(std::uint64_t id, std::uint32_t index);
    void erase_id(std::uint64_t id);
    std::uint32_t acquire();
    void release(std::uint32_t index);
    void append(std::uint32_t order_index);
    void unlink(std::uint32_t order_index);
    void remove(std::uint32_t order_index);
    Result add_impl(std::uint64_t id, Side side, std::int64_t price,
                    std::uint32_t quantity, bool match,
                    TradeCallback callback, void* context);
};

} // namespace nanobook
