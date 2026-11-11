#ifndef ORDERBOOK_TYPES_H
#define ORDERBOOK_TYPES_H
#include <cstdint>
#include <compare> // IWYU pragma: keep  (needed for defaulted <=>, even though no name from it is written)
#include <limits>

namespace lob {
    template <typename Tag, typename Rep>
    struct Strong {
        explicit constexpr Strong(Rep v) noexcept : value(v) {}

        constexpr auto operator<=>(const Strong&) const noexcept = default;
        Rep value;
    };

    using Price = Strong<struct PriceTag, std::int64_t>;

    constexpr bool is_invalid_price(Price price) {
        constexpr Price min_price {std::numeric_limits<std::int64_t>::min()};
        constexpr Price max_price {std::numeric_limits<std::int64_t>::max()};

        return price == min_price || price == max_price;
    }

    using Quantity = Strong<struct QuantityTag, std::uint32_t>;

    constexpr Quantity& operator-=(Quantity& x, Quantity y) noexcept {
        x.value -= y.value;
        return x;
    }

    using OrderId = Strong<struct OrderIdTag, std::uint64_t>;
    using SeqNo = Strong<struct SeqNoTag, std::uint64_t>;

    enum class Side : std::uint8_t { Buy, Sell };
    // GTC: rests until filled or cancelled. IOC: trades what it can now, the rest is cancelled.
    // Market: like IOC, but with no limit price (its price field is ignored).
    enum class TimeInForce: std::uint8_t { GTC, IOC, Market };

    [[nodiscard]] constexpr bool rests(TimeInForce tif) {
        switch (tif) {
            case TimeInForce::GTC:
                return true;
            case TimeInForce::IOC:
            case TimeInForce::Market:
                return false;
        }

        __builtin_unreachable();
    }

    [[nodiscard]] constexpr bool is_market(TimeInForce tif) {
        switch (tif) {
            case TimeInForce::GTC:
            case TimeInForce::IOC:
                return false;
            case TimeInForce::Market:
                return true;
        }

        __builtin_unreachable();
    }
}

#endif //ORDERBOOK_TYPES_H
