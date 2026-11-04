#ifndef ORDERBOOK_BOOK_TEST_SUPPORT_H
#define ORDERBOOK_BOOK_TEST_SUPPORT_H
#include <cstdint>
#include <ostream>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "lob/commands.h"
#include "lob/events.h"
#include "lob/types.h"

namespace lob {
    // gtest printers, so a failing EXPECT_EQ shows field values instead of raw bytes.
    // They must live in namespace lob so gtest finds them by argument-dependent lookup.
    template <typename Tag, typename Rep>
    void PrintTo(const Strong<Tag, Rep>& s, std::ostream* os) {
        *os << s.value;
    }

    inline void PrintTo(Side side, std::ostream* os) {
        *os << (side == Side::Buy ? "Buy" : "Sell");
    }

    inline void PrintTo(const OrderAck& e, std::ostream* os) {
        *os << "OrderAck{seq=" << e.seq.value << " id=" << e.id.value
            << " side=" << (e.side == Side::Buy ? "Buy" : "Sell")
            << " price=" << e.price.value << " qty=" << e.qty.value << "}";
    }

    inline void PrintTo(const Trade& e, std::ostream* os) {
        *os << "Trade{seq=" << e.seq.value << " maker=" << e.maker_id.value
            << " taker=" << e.taker_id.value << " price=" << e.price.value
            << " qty=" << e.qty.value << "}";
    }
}

namespace lob::test {

    inline NewOrder new_order(Side side, OrderId id, Price price, Quantity qty) {
        return NewOrder {
            .side = side,
            .tif = TimeInForce::GTC,
            .qty = qty,
            .seq = SeqNo{1},
            .id = id,
            .price = price,
        };
    }

    inline NewOrder buy(OrderId id, Price price, Quantity qty) {
        return new_order(Side::Buy, id, price, qty);
    }

    inline NewOrder sell(OrderId id, Price price, Quantity qty) {
        return new_order(Side::Sell, id, price, qty);
    }

    inline Side opposite(Side side) {
        return side == Side::Buy ? Side::Sell : Side::Buy;
    }

    // Everything the book can emit, in emission order. One overload of on() per event type,
    // so the listener interface the book must call is spelled out here.
    using Event = std::variant<OrderAck, Trade>;

    struct RecordingListener {
        std::vector<Event> events;

        void on(const OrderAck& e) { events.emplace_back(e); }
        void on(const Trade& e) { events.emplace_back(e); }
    };

    // Shared fixture: a book wired to a RecordingListener.
    // send() stamps increasing sequence numbers (1, 2, 3, ...) and returns the one it used,
    // so tests can state which seq a Trade must carry. Tests use ids 10, 20, 30, ...
    // so that an id accidentally used as a seq (or vice versa) fails.
    template <typename Book>
    class BookTest : public ::testing::Test {
    protected:
        RecordingListener listener;
        Book book{listener};

        SeqNo send(NewOrder order) {
            order.seq = SeqNo{next_seq_++};
            book.apply(order);
            return order.seq;
        }

        [[nodiscard]] std::vector<Trade> trades() const {
            std::vector<Trade> out;
            for (const auto& e : listener.events) {
                if (const auto* t = std::get_if<Trade>(&e)) {
                    out.push_back(*t);
                }
            }
            return out;
        }

    private:
        std::uint64_t next_seq_ {1};
    };
}

#endif //ORDERBOOK_BOOK_TEST_SUPPORT_H
