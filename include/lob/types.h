#ifndef ORDERBOOK_TYPES_H
#define ORDERBOOK_TYPES_H
#include <cstdint>

namespace lob {
    template <typename Tag, typename Rep>
    struct Strong { explicit constexpr Strong(Rep v) noexcept : value(v) {} Rep value; };

    using Price = Strong<struct PriceTag, std::int64_t>;
    using Quantity = Strong<struct QuantityTag, std::uint32_t>;
    using OrderId = Strong<struct OrderIdTag, std::uint64_t>;
    using SeqNo = Strong<struct SeqNoTag, std::uint64_t>;

    enum class Side : std::uint8_t { Buy, Sell };
    enum class TimeInForce: std::uint8_t { GTC, IOC };
}

#endif //ORDERBOOK_TYPES_H
