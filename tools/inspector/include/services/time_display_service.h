#pragma once

#include <cstdint>
#include <string>

#include "gui/gui_types.h"

namespace VtxServices {

class TimeDisplayService {
public:
    // Formats raw tick value as an integer string.
    static std::string FormatTicksRaw(uint64_t ticks);

    // Formats tick duration as "xh xm xs xms", omitting h/m when zero.
    static std::string FormatDurationFromTicks(uint64_t ticks);

    // Formats UTC ticks as RSS text.
    static std::string FormatUtcTicksAsRss(uint64_t ticks);

    // Formats UTC ticks as RSS text plus the raw tick value.
    static std::string FormatUtcTicksAsRssWithRaw(uint64_t ticks);

    // Formats unix-seconds UTC as RSS text plus the raw seconds value.
    static std::string FormatUnixSecondsAsRssWithRaw(int64_t unix_seconds);

    // Formats game-time ticks according to selected display mode.
    static std::string FormatGameTimeTicks(uint64_t ticks, TimeDisplayFormat format);

    // Formats UTC ticks according to selected display mode.
    static std::string FormatUtcTicks(uint64_t ticks, TimeDisplayFormat format);
};

} // namespace VtxServices

