// flash1 adapter: exposes lob::BaselineBook through the flash1 matching-engine benchmark's C ABI
// (api/matching_engine_api.h in https://github.com/flash1-dev/matching-engine-benchmark).
//
// The harness dlopen()s the .so built from this file, calls engine_on_* once per message, and checks
// the report stream we push (SHA-256 against the consensus) plus best bid/ask/depth at random points.
//
// This file is translation only: raw C structs -> lob commands, lob events -> me_report_t.
// All matching happens inside lob::BaselineBook. The global book instance lives here and nowhere else
// (the C ABI has no handle parameter, so a global is unavoidable at this boundary).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>

#include "matching_engine_api.h"

#include "lob/baseline.h"

namespace {

    // Receives lob events and pushes the matching flash1 report into the harness transport.
    // Report fields the canonical hash reads (src/correctness.cpp):
    //   OrderAck     seq, side, order_id, price, qty   (OrderReject has no flash1 equivalent)
    //   Trade        seq, price, qty, maker_id, taker_id
    //   CancelAck    seq, side, order_id, price        (qty is NOT hashed)
    //   ModifyAck    seq, side, order_id, price, qty
    //   Cancel/ModifyReject  seq, order_id
    class ReportSink {
    public:
        ReportSink(const me_transport_t* transport, void* sink) noexcept : transport_{transport}, sink_{sink} {}

        void on(const lob::OrderAck& e) const noexcept {
            push(ack(ME_ORDER_ACK, e.side, e.seq, e.id, e.price, e.qty));
        }

        // flash1's ABI has no new-order reject: its workload is contract-legal (qty > 0, no live
        // duplicate ids, prices far from the int64 extremes), so a reject means the adapter or
        // book is wrong. Fail loudly rather than emit a report the harness cannot represent.
        [[noreturn]] void on(const lob::OrderReject& e) const noexcept {
            std::fprintf(stderr, "flash1 adapter: book rejected order id %llu (seq %llu, reason %u)\n",
                         static_cast<unsigned long long>(e.id.value),
                         static_cast<unsigned long long>(e.seq.value), static_cast<unsigned>(e.reason));
            std::abort();
        }

        void on(const lob::Trade& e) const noexcept {
            me_report_t r{};
            r.type = ME_TRADE;
            r.sequence_number = e.seq.value;
            r.price_ticks = e.price.value;
            r.quantity = e.qty.value;
            r.maker_order_id = e.maker_id.value;
            r.taker_order_id = e.taker_id.value;
            push(r);
        }

        void on(const lob::CancelAck& e) const noexcept {
            push(ack(ME_CANCEL_ACK, e.side, e.seq, e.id, e.price, e.qty));
        }

        void on(const lob::ModifyAck& e) const noexcept {
            push(ack(ME_MODIFY_ACK, e.side, e.seq, e.id, e.price, e.qty));
        }

        void on(const lob::CancelReject& e) const noexcept {
            push(reject(ME_CANCEL_REJECT, e.seq, e.id));
        }

        void on(const lob::ModifyReject& e) const noexcept {
            push(reject(ME_MODIFY_REJECT, e.seq, e.id));
        }

    private:
        static me_report_t ack(me_report_type_t type, lob::Side side, lob::SeqNo seq, lob::OrderId id,
                               lob::Price price, lob::Quantity qty) noexcept {
            me_report_t r{};
            r.type = static_cast<std::uint8_t>(type);
            r.side = side == lob::Side::Buy ? 0 : 1;
            r.sequence_number = seq.value;
            r.order_id = id.value;
            r.price_ticks = price.value;
            r.quantity = qty.value;
            return r;
        }

        static me_report_t reject(me_report_type_t type, lob::SeqNo seq, lob::OrderId id) noexcept {
            me_report_t r{};
            r.type = static_cast<std::uint8_t>(type);
            r.sequence_number = seq.value;
            r.order_id = id.value;
            return r;
        }

        // The transport returns 0 when its queue is full; the drainer thread empties it, so spin.
        void push(const me_report_t& r) const noexcept {
            while (transport_->push(sink_, &r) == 0) {
                __builtin_ia32_pause();
            }
        }

        const me_transport_t* transport_;
        void* sink_;
    };

    // engine_init constructs these; engine_shutdown destroys them. The book holds a reference to the
    // sink, so the sink is declared first (constructed first, destroyed last).
    // unique_ptr rather than std::optional for the book: GCC 14 -O3 gives a false-positive
    // -Wmaybe-uninitialized inside optional<BaselineBook>::emplace.
    std::optional<ReportSink> g_sink;
    std::unique_ptr<lob::BaselineBook<ReportSink>> g_book;

    // seq is the offending message's sequence number, or nullopt for a query (queries have none).
    [[noreturn]] void reject_input(const char* what, unsigned value,
                                   std::optional<std::uint64_t> seq) noexcept {
        if (seq) {
            std::fprintf(stderr, "flash1 adapter: invalid %s = %u at sequence %llu\n", what, value,
                         static_cast<unsigned long long>(*seq));
        } else {
            std::fprintf(stderr, "flash1 adapter: invalid %s = %u in a query\n", what, value);
        }
        std::abort();
    }

    // Raw ints from the C structs are validated here, at the boundary, so the book only ever sees
    // valid enum values (types.h's switch helpers treat any other value as unreachable).
    lob::Side to_side(std::uint8_t raw, std::optional<std::uint64_t> seq) noexcept {
        switch (raw) {
            case 0: return lob::Side::Buy;
            case 1: return lob::Side::Sell;
            default: reject_input("side", raw, seq);
        }
    }

    // flash1 has no market orders: every new order is a limit order, GTC or IOC.
    lob::TimeInForce to_tif(std::uint8_t ioc, std::uint64_t seq) noexcept {
        switch (ioc) {
            case 0: return lob::TimeInForce::GTC;
            case 1: return lob::TimeInForce::IOC;
            default: reject_input("ioc flag", ioc, seq);
        }
    }

    // An exception must not cross the C ABI into the harness. The API header declares the engine_*
    // functions without noexcept (and C++ forbids redeclaring them with it), so everything that can
    // throw - allocation when creating the book or inserting an order (std::bad_alloc) - goes through
    // these noexcept helpers instead: a throw inside them calls std::terminate, which aborts the run.
    // (-fno-exceptions would NOT do this: operator new lives in libstdc++, which still throws, and
    // the exception would unwind straight through this file into the caller.)
    void init(const me_transport_t* transport, void* report_sink) noexcept {
        g_book.reset();
        g_sink.emplace(transport, report_sink);
        g_book = std::make_unique<lob::BaselineBook<ReportSink>>(*g_sink);
    }

    void apply(const lob::Command& command) noexcept {
        g_book->apply(command);
    }

} // namespace

extern "C" {

void engine_init(std::uint64_t /*seed*/, const me_transport_t* transport, void* report_sink) {
    init(transport, report_sink);
}

void engine_shutdown() {
    g_book.reset();
    g_sink.reset();
}

void engine_on_new_order(const new_order_t* o) {
    apply(lob::NewOrder{
        .side = to_side(o->side, o->sequence_number),
        .tif = to_tif(o->ioc, o->sequence_number),
        .qty = lob::Quantity{o->quantity},
        .seq = lob::SeqNo{o->sequence_number},
        .id = lob::OrderId{o->order_id},
        .price = lob::Price{o->price_ticks},
    });
}

void engine_on_cancel(const cancel_t* c) {
    apply(lob::Cancel{
        .seq = lob::SeqNo{c->sequence_number},
        .id = lob::OrderId{c->order_id},
    });
}

void engine_on_modify(const modify_t* m) {
    apply(lob::Modify{
        .side = to_side(m->side, m->sequence_number),
        .new_quantity = lob::Quantity{m->new_quantity},
        .seq = lob::SeqNo{m->sequence_number},
        .id = lob::OrderId{m->order_id},
        .new_price = lob::Price{m->new_price_ticks},
    });
}

// BaselineBook matches synchronously on the calling thread, so nothing is ever pending.
void engine_flush() {}

// The ABI uses sentinels for an empty side; lob uses std::optional. The mapping lives only here.
std::int64_t engine_query_best_bid() {
    const auto best = g_book->best_bid();
    return best ? best->value : std::numeric_limits<std::int64_t>::min();
}

std::int64_t engine_query_best_ask() {
    const auto best = g_book->best_ask();
    return best ? best->value : std::numeric_limits<std::int64_t>::max();
}

std::uint64_t engine_query_depth_at(std::int64_t price_ticks, std::uint8_t side) {
    return g_book->depth_at(lob::Price{price_ticks}, to_side(side, std::nullopt));
}

} // extern "C"
