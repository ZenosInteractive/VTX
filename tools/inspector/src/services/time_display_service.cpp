#include "services/time_display_service.h"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace {

    constexpr int64_t kTicksPerSecond = 10'000'000;
    constexpr int64_t kTicksPerMillisecond = 10'000;
    constexpr int64_t kTicksAtUnixEpoch = 621'355'968'000'000'000;

    // Converts mixed tick epochs into unix-relative ticks for display.
    int64_t NormalizeToUnixTicks(uint64_t ticks) {
        const int64_t signed_ticks = static_cast<int64_t>(ticks);
        if (signed_ticks >= kTicksAtUnixEpoch) {
            return signed_ticks - kTicksAtUnixEpoch;
        }
        return signed_ticks;
    }

    // Formats a unix-seconds timestamp as an RSS-like UTC string with milliseconds.
    std::string FormatUnixSecondsAsRss(int64_t unix_seconds, int32_t milliseconds) {
        const std::time_t time_value = static_cast<std::time_t>(unix_seconds);
        std::tm utc_time {};
#if defined(_WIN32)
        if (gmtime_s(&utc_time, &time_value) != 0) {
            return "Invalid UTC";
        }
#else
        if (gmtime_r(&time_value, &utc_time) == nullptr) {
            return "Invalid UTC";
        }
#endif

        std::ostringstream stream;
        stream << std::put_time(&utc_time, "%d %b %Y %H:%M:%S") << "." << std::setw(3) << std::setfill('0')
               << milliseconds << " UTC";
        return stream.str();
    }

} // namespace

namespace VtxServices {

    std::string TimeDisplayService::FormatTicksRaw(uint64_t ticks) {
        return std::to_string(ticks);
    }

    std::string TimeDisplayService::FormatDurationFromTicks(uint64_t ticks) {
        const int64_t total_milliseconds =
            static_cast<int64_t>((ticks + (kTicksPerMillisecond / 2)) / kTicksPerMillisecond);
        const int64_t hours = total_milliseconds / 3'600'000;
        const int64_t minutes = (total_milliseconds % 3'600'000) / 60'000;
        const int64_t seconds = (total_milliseconds % 60'000) / 1'000;
        const int64_t milliseconds = total_milliseconds % 1'000;

        std::ostringstream stream;
        if (hours > 0) {
            stream << hours << "h ";
        }
        if (minutes > 0 || hours > 0) {
            stream << minutes << "m ";
        }
        stream << seconds << "s " << milliseconds << "ms";
        return stream.str();
    }

    std::string TimeDisplayService::FormatUtcTicksAsRss(uint64_t ticks) {
        const int64_t unix_ticks = NormalizeToUnixTicks(ticks);
        const int64_t unix_seconds = unix_ticks / kTicksPerSecond;
        const int64_t milliseconds_part = (unix_ticks % kTicksPerSecond) / kTicksPerMillisecond;
        const int32_t milliseconds = milliseconds_part >= 0 ? static_cast<int32_t>(milliseconds_part)
                                                            : static_cast<int32_t>(milliseconds_part + 1000);
        return FormatUnixSecondsAsRss(unix_seconds, milliseconds);
    }

    std::string TimeDisplayService::FormatUtcTicksAsRssWithRaw(uint64_t ticks) {
        return FormatUtcTicksAsRss(ticks) + " (" + FormatTicksRaw(ticks) + ")";
    }

    std::string TimeDisplayService::FormatUnixSecondsAsRssWithRaw(int64_t unix_seconds) {
        return FormatUnixSecondsAsRss(unix_seconds, 0) + " (" + std::to_string(unix_seconds) + ")";
    }

    std::string TimeDisplayService::FormatGameTimeTicks(uint64_t ticks, TimeDisplayFormat format) {
        return format == TimeDisplayFormat::Formatted ? FormatDurationFromTicks(ticks) : FormatTicksRaw(ticks);
    }

    std::string TimeDisplayService::FormatUtcTicks(uint64_t ticks, TimeDisplayFormat format) {
        return format == TimeDisplayFormat::Formatted ? FormatUtcTicksAsRss(ticks) : FormatTicksRaw(ticks);
    }

} // namespace VtxServices
