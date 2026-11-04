#ifndef ORDERBOOK_TYPES_H
#define ORDERBOOK_TYPES_H
#include <cstdint>
#include <compare> // IWYU pragma: keep  (needed for defaulted <=>, even though no name from it is written)

namespace lob {
    template <typename Tag, typename Rep>
    struct Strong {
        explicit constexpr Strong(Rep v) noexcept : value(v) {}

        constexpr auto operator<=>(const Strong&) const noexcept = default;
        Rep value;
    };

    using Price = Strong<struct PriceTag, std::int64_t>;

    using Quantity = Strong<struct QuantityTag, std::uint32_t>;

    constexpr Quantity& operator-=(Quantity& x, Quantity y) noexcept {
        x.value -= y.value;
        return x;
    }

    using OrderId = Strong<struct OrderIdTag, std::uint64_t>;
    using SeqNo = Strong<struct SeqNoTag, std::uint64_t>;

    enum class Side : std::uint8_t { Buy, Sell };
    enum class TimeInForce: std::uint8_t { GTC, IOC };
}

#endif //ORDERBOOK_TYPES_H
