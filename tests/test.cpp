#include "nanobook/feed.hpp"
#include "nanobook/order_book.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <new>
#include <sstream>
#include <vector>

static std::size_t allocations = 0;
void* operator new(std::size_t size) {
    ++allocations;
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    ++allocations;
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

using namespace nanobook;

#define CHECK(expr) do { if (!(expr)) { \
    std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": " << #expr << '\n'; \
    std::exit(1); \
} } while (0)

struct Reference {
    struct Order { Side side; std::int64_t price; std::uint32_t quantity; std::uint64_t seq; };
    std::map<std::uint64_t, Order> orders;
    std::uint64_t next_seq = 0;

    Result add(std::uint64_t id, Side side, std::int64_t price, std::uint32_t quantity,
               std::vector<Trade>& trades) {
        if (id == 0 || quantity == 0) return {Status::invalid_order};
        if (price < 1 || price > 20) return {Status::invalid_price};
        if (orders.contains(id)) return {Status::duplicate_id};
        Result result;
        while (quantity > 0) {
            auto best = orders.end();
            for (auto it = orders.begin(); it != orders.end(); ++it) {
                auto& resting = it->second;
                if (resting.side == side) continue;
                if ((side == Side::buy && resting.price > price) ||
                    (side == Side::sell && resting.price < price)) continue;
                if (best == orders.end() ||
                    (side == Side::buy ? resting.price < best->second.price
                                       : resting.price > best->second.price) ||
                    (resting.price == best->second.price && resting.seq < best->second.seq)) best = it;
            }
            if (best == orders.end()) break;
            auto fill = std::min(quantity, best->second.quantity);
            trades.push_back({id, best->first, best->second.price, fill});
            quantity -= fill;
            best->second.quantity -= fill;
            result.executed_quantity += fill;
            ++result.trade_count;
            if (best->second.quantity == 0) orders.erase(best);
        }
        if (quantity > 0) {
            orders.emplace(id, Order{side, price, quantity, next_seq++});
            result.resting_quantity = quantity;
        }
        return result;
    }

    Result cancel(std::uint64_t id) {
        return orders.erase(id) == 1 ? Result{} : Result{Status::unknown_id};
    }

    Result reduce(std::uint64_t id, std::uint32_t quantity) {
        auto it = orders.find(id);
        if (it == orders.end()) return {Status::unknown_id};
        if (quantity == 0 || quantity > it->second.quantity) return {Status::invalid_order};
        it->second.quantity -= quantity;
        if (it->second.quantity == 0) orders.erase(it);
        return {};
    }

    Result modify(std::uint64_t id, std::int64_t price, std::uint32_t quantity,
                  std::vector<Trade>& trades) {
        auto it = orders.find(id);
        if (it == orders.end()) return {Status::unknown_id};
        if (quantity == 0) return {Status::invalid_order};
        if (price < 1 || price > 20) return {Status::invalid_price};
        if (it->second.price == price && quantity <= it->second.quantity) {
            it->second.quantity = quantity;
            return {};
        }
        auto side = it->second.side;
        orders.erase(it);
        return add(id, side, price, quantity, trades);
    }

    Quote best(Side side) const {
        Quote result;
        for (const auto& [id, order] : orders) {
            (void)id;
            if (order.side != side) continue;
            if (!result.present || (side == Side::buy ? order.price > result.price
                                                      : order.price < result.price)) {
                result = {order.price, order.quantity, true};
            } else if (order.price == result.price) result.quantity += order.quantity;
        }
        return result;
    }
};

void collect(const Trade& trade, void* context) {
    static_cast<std::vector<Trade>*>(context)->push_back(trade);
}

void same_quote(Quote actual, Quote expected) {
    CHECK(actual.present == expected.present);
    if (expected.present) {
        CHECK(actual.price == expected.price);
        CHECK(actual.quantity == expected.quantity);
    }
}

void basic_tests() {
    OrderBook book({1, 20, 1, 8});
    CHECK(book.add(1, Side::sell, 10, 5).status == Status::ok);
    CHECK(book.add(2, Side::sell, 10, 7).status == Status::ok);
    CHECK(book.add(3, Side::sell, 11, 2).status == Status::ok);
    std::vector<Trade> trades;
    auto result = book.add(4, Side::buy, 11, 13, collect, &trades);
    CHECK(result.status == Status::ok && result.executed_quantity == 13);
    CHECK(trades.size() == 3);
    CHECK(trades[0].resting_id == 1 && trades[0].quantity == 5 && trades[0].price == 10);
    CHECK(trades[1].resting_id == 2 && trades[1].quantity == 7 && trades[1].price == 10);
    CHECK(trades[2].resting_id == 3 && trades[2].quantity == 1 && trades[2].price == 11);
    CHECK(book.best_ask().price == 11 && book.best_ask().quantity == 1);
    CHECK(book.cancel(3).status == Status::ok);
    CHECK(!book.best_ask().present);
    CHECK(book.add(7, Side::buy, 9, 10).status == Status::ok);
    CHECK(book.reduce(7, 3).status == Status::ok);
    CHECK(book.best_bid().quantity == 7);
    CHECK(book.modify(7, 9, 5).status == Status::ok);
    CHECK(book.best_bid().quantity == 5);
    CHECK(book.modify(7, 9, 8).status == Status::ok);
    CHECK(book.best_bid().quantity == 8);
    CHECK(book.add(8, Side::buy, 9, 1).status == Status::ok);
    trades.clear();
    book.add(9, Side::sell, 9, 8, collect, &trades);
    CHECK(trades.size() == 1 && trades[0].resting_id == 7);
    CHECK(book.best_bid().quantity == 1);
    CHECK(book.check_invariants());

    CHECK(book.add(8, Side::buy, 8, 1).status == Status::duplicate_id);
    CHECK(book.cancel(987).status == Status::unknown_id);
    CHECK(book.add(0, Side::buy, 8, 1).status == Status::invalid_order);
    CHECK(book.add(10, Side::buy, 21, 1).status == Status::invalid_price);
    CHECK(book.reduce(8, 2).status == Status::invalid_order);
    CHECK(book.modify(8, 21, 1).status == Status::invalid_price);
    CHECK(book.check_invariants());
}

void feed_tests() {
    std::istringstream input("34200.000000001,1,1,100,10,1\n"
                             "34200.000000002,1,2,50,11,-1\n"
                             "34200.000000003,2,1,20,10,1\n"
                             "34200.000000004,4,2,20,11,-1\n"
                             "34200.000000005,5,0,10,11,-1\n"
                             "34200.000000006,6,0,10,11,-1\n"
                             "34200.000000007,7,0,0,-1,-1\n"
                             "34200.000000008,3,1,80,10,1\n");
    auto events = read_lobster(input);
    CHECK(events.size() == 8);
    OrderBook book({1, 20, 1, 10});
    ReplayCounters counters;
    for (const auto& event : events) CHECK(apply_feed_event(book, event, counters) == Status::ok);
    CHECK(!book.best_bid().present);
    CHECK(book.best_ask().price == 11 && book.best_ask().quantity == 30);
    CHECK(counters.ignored_hidden_cross_halt == 3);
    CHECK(book.check_invariants());
    CHECK(apply_feed_event(book, {2, 999, 1, 10, Side::buy}, counters) == Status::unknown_id);
    CHECK(counters.missing_id == 1);
}

std::uint64_t random_state = 0x12345678abcdefULL;
std::uint64_t random_number() {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

void differential_test() {
    OrderBook book({1, 20, 1, 1000});
    Reference reference;
    std::uint64_t next_id = 1;
    for (int step = 0; step < 5000; ++step) {
        auto choice = random_number() % 100;
        auto id = reference.orders.empty() || random_number() % 5 == 0
                    ? next_id + 1000 : std::next(reference.orders.begin(),
                          random_number() % reference.orders.size())->first;
        auto price = static_cast<std::int64_t>(random_number() % 20 + 1);
        auto quantity = static_cast<std::uint32_t>(random_number() % 20 + 1);
        auto side = random_number() % 2 ? Side::buy : Side::sell;
        std::vector<Trade> actual_trades, expected_trades;
        Result actual, expected;
        if (choice < 50) {
            id = next_id++;
            actual = book.add(id, side, price, quantity, collect, &actual_trades);
            expected = reference.add(id, side, price, quantity, expected_trades);
        } else if (choice < 70) {
            actual = book.cancel(id); expected = reference.cancel(id);
        } else if (choice < 85) {
            actual = book.reduce(id, quantity); expected = reference.reduce(id, quantity);
        } else {
            actual = book.modify(id, price, quantity, collect, &actual_trades);
            expected = reference.modify(id, price, quantity, expected_trades);
        }
        CHECK(actual.status == expected.status);
        CHECK(actual.executed_quantity == expected.executed_quantity);
        CHECK(actual.resting_quantity == expected.resting_quantity);
        CHECK(actual_trades.size() == expected_trades.size());
        for (std::size_t i = 0; i < actual_trades.size(); ++i) {
            CHECK(actual_trades[i].resting_id == expected_trades[i].resting_id);
            CHECK(actual_trades[i].price == expected_trades[i].price);
            CHECK(actual_trades[i].quantity == expected_trades[i].quantity);
        }
        same_quote(book.best_bid(), reference.best(Side::buy));
        same_quote(book.best_ask(), reference.best(Side::sell));
        CHECK(book.order_count() == reference.orders.size());
        CHECK(book.check_invariants());
    }
}

void allocation_and_capacity_test() {
    OrderBook book({1, 20, 1, 2});
    auto before = allocations;
    CHECK(book.add(1, Side::buy, 9, 10).status == Status::ok);
    CHECK(book.add(2, Side::sell, 11, 10).status == Status::ok);
    CHECK(book.add(3, Side::buy, 8, 10).status == Status::capacity_exceeded);
    CHECK(book.add(4, Side::buy, 11, 10).executed_quantity == 10);
    CHECK(!book.contains(2));
    CHECK(book.add(2, Side::sell, 11, 10).status == Status::ok);
    CHECK(book.reduce(1, 2).status == Status::ok);
    CHECK(book.modify(1, 9, 9).status == Status::ok);
    CHECK(book.cancel(2).status == Status::ok);
    CHECK(book.add(3, Side::sell, 10, 10).status == Status::ok);
    CHECK(book.cancel(1).status == Status::ok);
    CHECK(book.cancel(3).status == Status::ok);
    CHECK(allocations == before);
    CHECK(book.check_invariants());
}

void long_churn_test() {
    OrderBook book({1, 20, 1, 8});
    auto before = allocations;
    for (std::uint64_t id = 1; id <= 200000; ++id) {
        CHECK(book.add(id, Side::buy, 9, 1).status == Status::ok);
        CHECK(book.contains(id));
        CHECK(book.cancel(id).status == Status::ok);
    }
    CHECK(allocations == before);
    CHECK(book.order_count() == 0);
    CHECK(book.check_invariants());
}

void collision_deletion_test() {
    OrderBook book({1, 20, 1, 8});
    // Fill and empty an eight-slot book repeatedly while other IDs stay live.
    for (std::uint64_t base = 1; base < 10000; base += 8) {
        for (std::uint64_t offset = 0; offset < 8; ++offset) {
            CHECK(book.add(base + offset, Side::buy, 9, 1).status == Status::ok);
        }
        for (std::uint64_t offset = 0; offset < 8; offset += 2) {
            CHECK(book.cancel(base + offset).status == Status::ok);
        }
        for (std::uint64_t offset = 1; offset < 8; offset += 2) {
            CHECK(book.contains(base + offset));
            CHECK(book.cancel(base + offset).status == Status::ok);
        }
    }
    CHECK(book.check_invariants());
}

int main() {
    basic_tests();
    feed_tests();
    differential_test();
    allocation_and_capacity_test();
    long_churn_test();
    collision_deletion_test();
    std::cout << "all tests passed\n";
}
