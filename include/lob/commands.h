#ifndef ORDERBOOK_COMMANDS_H
#define ORDERBOOK_COMMANDS_H
#include <variant>
#include <type_traits>

#include "types.h"

namespace lob {
    struct NewOrder {
        Side side;
        TimeInForce tif;
        Quantity qty;
        SeqNo seq;
        OrderId id;
        Price price;
    };

    static_assert(sizeof(NewOrder) == 32);
    static_assert(std::is_trivially_copyable_v<NewOrder>);

    struct Cancel {
        SeqNo seq;
        OrderId id;
    };

    static_assert(sizeof(Cancel) == 16);
    static_assert(std::is_trivially_copyable_v<Cancel>);

    struct Modify {
        Side side;
        Quantity new_quantity;
        SeqNo seq;
        OrderId id;
        Price new_price;
    };

    static_assert(sizeof(Modify) == 32);
    static_assert(std::is_trivially_copyable_v<Modify>);

    using Command = std::variant<NewOrder, Cancel, Modify>;
}

#endif //ORDERBOOK_COMMANDS_H
