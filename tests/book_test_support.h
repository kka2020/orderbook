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

    inline void PrintTo(const CancelAck& e, std::ostream* os) {
        *os << "CancelAck{seq=" << e.seq.value << " id=" << e.id.value
            << " side=" << (e.side == Side::Buy ? "Buy" : "Sell")
            << " price=" << e.price.value << " qty=" << e.qty.value << "}";
    }

    inline void PrintTo(const CancelReject& e, std::ostream* os) {
        *os << "CancelReject{seq=" << e.seq.value << " id=" << e.id.value << "}";
    }

    inline void PrintTo(const ModifyAck& e, std::ostream* os) {
        *os << "ModifyAck{seq=" << e.seq.value << " id=" << e.id.value
            << " side=" << (e.side == Side::Buy ? "Buy" : "Sell")
            << " price=" << e.price.value << " qty=" << e.qty.value << "}";
    }

    inline void PrintTo(const ModifyReject& e, std::ostream* os) {
        *os << "ModifyReject{seq=" << e.seq.value << " id=" << e.id.value << "}";
    }
}

namespace lob::test {

    inline NewOrder new_order(Side side, OrderId id, Price price, Quantity qty,
                              TimeInForce tif = TimeInForce::GTC) {
        return NewOrder {
            .side = side,
            .tif = tif,
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

    inline NewOrder ioc_buy(OrderId id, Price price, Quantity qty) {
        return new_order(Side::Buy, id, price, qty, TimeInForce::IOC);
    }

    inline NewOrder ioc_sell(OrderId id, Price price, Quantity qty) {
        return new_order(Side::Sell, id, price, qty, TimeInForce::IOC);
    }

    // Market orders are sent with price 0; the book must ignore it.
    inline NewOrder market_buy(OrderId id, Quantity qty) {
        return new_order(Side::Buy, id, Price{0}, qty, TimeInForce::Market);
    }

    inline NewOrder market_sell(OrderId id, Quantity qty) {
        return new_order(Side::Sell, id, Price{0}, qty, TimeInForce::Market);
    }

    inline Cancel cancel(OrderId id) {
        return Cancel {.seq = SeqNo{1}, .id = id};
    }

    // new_qty is the order's new open quantity (not added to what is left).
    inline Modify modify(OrderId id, Side side, Price new_price, Quantity new_qty) {
        return Modify {.side = side, .new_quantity = new_qty, .seq = SeqNo{1}, .id = id, .new_price = new_price};
    }

    inline Side opposite(Side side) {
        return side == Side::Buy ? Side::Sell : Side::Buy;
    }

    // Everything the book can emit, in emission order. One overload of on() per event type,
    // so the listener interface the book must call is spelled out here.
    using Event = std::variant<OrderAck, Trade, CancelAck, CancelReject, ModifyAck, ModifyReject>;

    struct RecordingListener {
        std::vector<Event> events;

        void on(const OrderAck& e) { events.emplace_back(e); }
        void on(const Trade& e) { events.emplace_back(e); }
        void on(const CancelAck& e) { events.emplace_back(e); }
        void on(const CancelReject& e) { events.emplace_back(e); }
        void on(const ModifyAck& e) { events.emplace_back(e); }
        void on(const ModifyReject& e) { events.emplace_back(e); }
    };

    // Shared fixture: a book wired to a RecordingListener.
    // send() stamps increasing sequence numbers (1, 2, 3, ...) on any command and returns the one it used,
    // so tests can state which seq a Trade must carry. Tests use ids 10, 20, 30, ...
    // so that an id accidentally used as a seq (or vice versa) fails.
    template <typename Book>
    class BookTest : public ::testing::Test {
    protected:
        RecordingListener listener;
        Book book{listener};

        template <typename C>
        SeqNo send(C command) {
            command.seq = SeqNo{next_seq_++};
            book.apply(command);
            return command.seq;
        }

        // All recorded events of one type, in emission order.
        template <typename E>
        [[nodiscard]] std::vector<E> events_of() const {
            std::vector<E> out;
            for (const auto& e : listener.events) {
                if (const auto* match = std::get_if<E>(&e)) {
                    out.push_back(*match);
                }
            }
            return out;
        }

        [[nodiscard]] std::vector<Trade> trades() const { return events_of<Trade>(); }
        [[nodiscard]] std::vector<CancelAck> cancel_acks() const { return events_of<CancelAck>(); }
        [[nodiscard]] std::vector<CancelReject> cancel_rejects() const { return events_of<CancelReject>(); }
        [[nodiscard]] std::vector<ModifyAck> modify_acks() const { return events_of<ModifyAck>(); }
        [[nodiscard]] std::vector<ModifyReject> modify_rejects() const { return events_of<ModifyReject>(); }

    private:
        std::uint64_t next_seq_ {1};
    };
}

#endif //ORDERBOOK_BOOK_TEST_SUPPORT_H
