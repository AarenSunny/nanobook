#include "nanobook/order_book.hpp"

#include <bit>
#include <limits>
#include <stdexcept>

namespace nanobook {
namespace {
std::uint64_t mix(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::size_t table_size(std::size_t max_orders) {
    if (max_orders == 0 || max_orders > UINT32_MAX ||
        max_orders > (std::numeric_limits<std::size_t>::max() / 2)) {
        throw std::invalid_argument("max_orders must be between 1 and UINT32_MAX");
    }
    return std::bit_ceil(max_orders * 2);
}
} // namespace

OrderBook::OrderBook(Config config)
    : config_(config),
      level_count_([&] {
          if (config.tick_size <= 0 || config.min_price <= 0 ||
              config.max_price < config.min_price ||
              (config.max_price - config.min_price) / config.tick_size >= UINT32_MAX) {
              throw std::invalid_argument("invalid price ladder");
          }
          return static_cast<std::size_t>((config.max_price - config.min_price) /
                                          config.tick_size) + 1;
      }()),
      bids_(level_count_), asks_(level_count_),
      bid_bits_((level_count_ + 63) / 64), ask_bits_((level_count_ + 63) / 64),
      orders_(config.max_orders), ids_(table_size(config.max_orders)) {}

bool OrderBook::price_index(std::int64_t price, std::uint32_t& index) const {
    if (price < config_.min_price || price > config_.max_price ||
        (price - config_.min_price) % config_.tick_size != 0) return false;
    index = static_cast<std::uint32_t>((price - config_.min_price) / config_.tick_size);
    return true;
}

std::int64_t OrderBook::price_at(std::uint32_t index) const {
    return config_.min_price + static_cast<std::int64_t>(index) * config_.tick_size;
}

OrderBook::Level& OrderBook::level(Side side, std::uint32_t index) {
    return (side == Side::buy ? bids_ : asks_)[index];
}

const OrderBook::Level& OrderBook::level(Side side, std::uint32_t index) const {
    return (side == Side::buy ? bids_ : asks_)[index];
}

void OrderBook::set_occupied(Side side, std::uint32_t index, bool occupied) {
    auto& bits = side == Side::buy ? bid_bits_ : ask_bits_;
    auto& best = side == Side::buy ? best_bid_index_ : best_ask_index_;
    auto mask = std::uint64_t{1} << (index % 64);
    if (occupied) {
        bits[index / 64] |= mask;
        if (best == none || (side == Side::buy ? index > best : index < best)) best = index;
    } else {
        bits[index / 64] &= ~mask;
        if (best == index) best = scan_best(side);
    }
}

std::uint32_t OrderBook::best_index(Side side) const {
    return side == Side::buy ? best_bid_index_ : best_ask_index_;
}

std::uint32_t OrderBook::scan_best(Side side) const {
    const auto& bits = side == Side::buy ? bid_bits_ : ask_bits_;
    if (side == Side::sell) {
        for (std::size_t word = 0; word < bits.size(); ++word) {
            if (bits[word]) return static_cast<std::uint32_t>(word * 64 + std::countr_zero(bits[word]));
        }
    } else {
        for (std::size_t word = bits.size(); word-- > 0;) {
            if (bits[word]) return static_cast<std::uint32_t>(word * 64 + 63 - std::countl_zero(bits[word]));
        }
    }
    return none;
}

bool OrderBook::can_fully_match(Side incoming_side, std::uint32_t limit_index,
                                std::uint32_t quantity) const {
    std::uint64_t needed = quantity;
    if (incoming_side == Side::buy) {
        for (std::size_t index = 0; index <= limit_index; ++index) {
            auto available = asks_[index].quantity;
            if (available >= needed) return true;
            needed -= available;
        }
    } else {
        for (std::size_t index = level_count_; index-- > limit_index;) {
            auto available = bids_[index].quantity;
            if (available >= needed) return true;
            needed -= available;
        }
    }
    return false;
}

std::size_t OrderBook::id_position(std::uint64_t id) const {
    auto pos = static_cast<std::size_t>(mix(id)) & (ids_.size() - 1);
    while (ids_[pos].state != 0) {
        if (ids_[pos].key == id) return pos;
        pos = (pos + 1) & (ids_.size() - 1);
    }
    return ids_.size();
}

std::size_t OrderBook::insertion_position(std::uint64_t id) const {
    auto pos = static_cast<std::size_t>(mix(id)) & (ids_.size() - 1);
    while (ids_[pos].state != 0) pos = (pos + 1) & (ids_.size() - 1);
    return pos;
}

std::uint32_t OrderBook::find(std::uint64_t id) const {
    auto pos = id_position(id);
    return pos == ids_.size() ? none : ids_[pos].index;
}

bool OrderBook::contains(std::uint64_t id) const { return find(id) != none; }

void OrderBook::insert_id(std::uint64_t id, std::uint32_t index) {
    auto& slot = ids_[insertion_position(id)];
    slot = {id, index, 1};
}

void OrderBook::erase_id(std::uint64_t id) {
    const auto mask = ids_.size() - 1;
    auto hole = id_position(id);
    auto next = (hole + 1) & mask;
    while (ids_[next].state != 0) {
        auto home = static_cast<std::size_t>(mix(ids_[next].key)) & mask;
        if (((next - home) & mask) > ((hole - home) & mask)) {
            ids_[hole] = ids_[next];
            hole = next;
        }
        next = (next + 1) & mask;
    }
    ids_[hole].state = 0;
}

std::uint32_t OrderBook::acquire() {
    std::uint32_t index;
    if (free_head_ != none) {
        index = free_head_;
        free_head_ = orders_[index].next;
    } else {
        index = next_unused_++;
    }
    ++live_orders_;
    return index;
}

void OrderBook::release(std::uint32_t index) {
    orders_[index].next = free_head_;
    free_head_ = index;
    --live_orders_;
}

void OrderBook::append(std::uint32_t order_index) {
    auto& order = orders_[order_index];
    auto& price_level = level(order.side, order.price_index);
    order.prev = price_level.tail;
    order.next = none;
    if (price_level.tail != none) orders_[price_level.tail].next = order_index;
    else price_level.head = order_index;
    price_level.tail = order_index;
    price_level.quantity += order.quantity;
    ++price_level.orders;
    if (price_level.orders == 1) set_occupied(order.side, order.price_index, true);
}

void OrderBook::unlink(std::uint32_t order_index) {
    auto& order = orders_[order_index];
    auto& price_level = level(order.side, order.price_index);
    if (order.prev == none) price_level.head = order.next;
    else orders_[order.prev].next = order.next;
    if (order.next == none) price_level.tail = order.prev;
    else orders_[order.next].prev = order.prev;
    price_level.quantity -= order.quantity;
    if (--price_level.orders == 0) set_occupied(order.side, order.price_index, false);
}

void OrderBook::remove(std::uint32_t order_index) {
    auto id = orders_[order_index].id;
    unlink(order_index);
    erase_id(id);
    release(order_index);
}

Result OrderBook::add_impl(std::uint64_t id, Side side, std::int64_t price,
                           std::uint32_t quantity, bool match,
                           TradeCallback callback, void* context) {
    if (id == 0 || quantity == 0) return {Status::invalid_order};
    std::uint32_t index;
    if (!price_index(price, index)) return {Status::invalid_price};
    if (contains(id)) return {Status::duplicate_id};
    if (live_orders_ == orders_.size() &&
        (!match || !can_fully_match(side, index, quantity))) {
        return {Status::capacity_exceeded};
    }

    Result result;
    auto remaining = quantity;
    if (match) {
        auto opposite = side == Side::buy ? Side::sell : Side::buy;
        while (remaining > 0) {
            auto best = best_index(opposite);
            if (best == none) break;
            auto resting_price = price_at(best);
            if ((side == Side::buy && resting_price > price) ||
                (side == Side::sell && resting_price < price)) break;
            auto resting_index = level(opposite, best).head;
            auto& resting = orders_[resting_index];
            auto fill = remaining < resting.quantity ? remaining : resting.quantity;
            if (callback) callback({id, resting.id, resting_price, fill}, context);
            remaining -= fill;
            result.executed_quantity += fill;
            ++result.trade_count;
            total_traded_ += fill;
            ++total_trades_;
            if (fill == resting.quantity) remove(resting_index);
            else {
                resting.quantity -= fill;
                level(opposite, best).quantity -= fill;
            }
        }
    }
    if (remaining != 0) {
        auto slot = acquire();
        orders_[slot] = {id, remaining, none, none, index, side};
        append(slot);
        insert_id(id, slot);
        result.resting_quantity = remaining;
    }
    return result;
}

Result OrderBook::add(std::uint64_t id, Side side, std::int64_t price,
                      std::uint32_t quantity, TradeCallback callback, void* context) {
    return add_impl(id, side, price, quantity, true, callback, context);
}

Result OrderBook::add_passive(std::uint64_t id, Side side, std::int64_t price,
                              std::uint32_t quantity) {
    return add_impl(id, side, price, quantity, false, nullptr, nullptr);
}

Result OrderBook::cancel(std::uint64_t id) {
    auto index = find(id);
    if (index == none) return {Status::unknown_id};
    remove(index);
    return {};
}

Result OrderBook::reduce(std::uint64_t id, std::uint32_t quantity) {
    auto index = find(id);
    if (index == none) return {Status::unknown_id};
    if (quantity == 0 || quantity > orders_[index].quantity) return {Status::invalid_order};
    if (quantity == orders_[index].quantity) return cancel(id);
    auto& order = orders_[index];
    order.quantity -= quantity;
    level(order.side, order.price_index).quantity -= quantity;
    return {};
}

Result OrderBook::modify(std::uint64_t id, std::int64_t price,
                         std::uint32_t quantity, TradeCallback callback, void* context) {
    auto index = find(id);
    if (index == none) return {Status::unknown_id};
    if (quantity == 0) return {Status::invalid_order};
    std::uint32_t price_slot;
    if (!price_index(price, price_slot)) return {Status::invalid_price};
    auto old = orders_[index];
    if (old.price_index == price_slot && quantity <= old.quantity) {
        auto delta = old.quantity - quantity;
        orders_[index].quantity = quantity;
        level(old.side, old.price_index).quantity -= delta;
        return {};
    }
    remove(index);
    return add_impl(id, old.side, price, quantity, true, callback, context);
}

Quote OrderBook::best_bid() const {
    auto index = best_index(Side::buy);
    return index == none ? Quote{} : Quote{price_at(index), bids_[index].quantity, true};
}

Quote OrderBook::best_ask() const {
    auto index = best_index(Side::sell);
    return index == none ? Quote{} : Quote{price_at(index), asks_[index].quantity, true};
}

bool OrderBook::check_invariants() const {
    if (best_bid_index_ != scan_best(Side::buy) ||
        best_ask_index_ != scan_best(Side::sell)) return false;
    std::size_t counted = 0;
    for (auto side : {Side::buy, Side::sell}) {
        const auto& bits = side == Side::buy ? bid_bits_ : ask_bits_;
        for (std::size_t p = 0; p < level_count_; ++p) {
            const auto& price_level = level(side, static_cast<std::uint32_t>(p));
            bool occupied = (bits[p / 64] >> (p % 64)) & 1;
            if (occupied != (price_level.orders != 0)) return false;
            if ((price_level.head == none) != (price_level.orders == 0)) return false;
            std::uint64_t quantity = 0;
            std::uint32_t orders = 0;
            auto prev = none;
            for (auto i = price_level.head; i != none; i = orders_[i].next) {
                if (++orders > live_orders_) return false;
                const auto& order = orders_[i];
                if (order.prev != prev || order.side != side || order.price_index != p ||
                    order.quantity == 0 || find(order.id) != i) return false;
                quantity += order.quantity;
                prev = i;
            }
            if (prev != price_level.tail || orders != price_level.orders ||
                quantity != price_level.quantity) return false;
            counted += orders;
        }
    }
    std::size_t indexed = 0;
    for (const auto& slot : ids_) {
        if (slot.state == 1) {
            ++indexed;
            if (slot.index >= orders_.size() || orders_[slot.index].id != slot.key) return false;
        }
    }
    return counted == live_orders_ && indexed == live_orders_;
}

} // namespace nanobook
