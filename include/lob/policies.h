#ifndef ORDERBOOK_POLICIES_H
#define ORDERBOOK_POLICIES_H

#include <algorithm>

#include "types.h"
#include "concepts.h"

namespace lob {
    struct FifoPolicy {
        template <FifoLevel Level, typename OnFill>
        static Quantity fill_level(Level& level, Quantity taker_qty, OnFill&& on_fill) {
            // Consume orders until level runs out or taker runs out of qty
            while (!level.empty() && taker_qty.value > 0) {
                auto& maker {level.front()};

                Quantity fill_qty {std::min(maker.qty, taker_qty)};

                maker.qty -= fill_qty;
                taker_qty -= fill_qty;

                on_fill(maker, fill_qty);

                if (maker.qty.value == 0) {
                    level.pop_front();
                }
            }

            return taker_qty;
        }
    };
}

#endif //ORDERBOOK_POLICIES_H
