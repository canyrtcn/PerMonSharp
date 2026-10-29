#pragma once

#include "display/DisplayInfo.h"
#include <vector>

namespace pms {

class DisplayManager {
public:
    // Enumerates all active connected displays across all GPU adapters, sorted with Primary first,
    // then left-to-right / top-to-bottom, with duplicate model names automatically disambiguated.
    static std::vector<DisplayInfo> EnumerateDisplays();
};

} // namespace pms
