#include "zero_depth_local.h"

#include <stdexcept>
#include <string>

int resolveZeroDepthLocalSlot(int exactLocalCountMinusOne, bool atCfgMergeLabel,
                              int lastInvisibleVarSlot, int knownJoinSlot,
                              int offset, const char* backendTag) {
    if (knownJoinSlot >= 0) {
        if (knownJoinSlot != exactLocalCountMinusOne) {
            throw std::runtime_error(
                std::string(backendTag) + ": offset " + std::to_string(offset) +
                " is a recorded and/or join for slot " +
                std::to_string(knownJoinSlot) +
                ", but the analysis reports localCount - 1 = " +
                std::to_string(exactLocalCountMinusOne));
        }
        return knownJoinSlot;
    }
    if (atCfgMergeLabel && lastInvisibleVarSlot != exactLocalCountMinusOne) {
        throw std::runtime_error(std::string(backendTag) + ": offset " +
                                 std::to_string(offset) +
                                 " is a CFG merge; localCount - 1 (" +
                                 std::to_string(exactLocalCountMinusOne) +
                                 ") disagrees with the forward-walk tracker (" +
                                 std::to_string(lastInvisibleVarSlot) + ")");
    }
    return exactLocalCountMinusOne;
}
