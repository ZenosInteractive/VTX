#pragma once
#include <string>
#include <memory>
#include "gui/gui_manager.h"

namespace VtxGuiNames {
    inline const char* SessionName = "VtxSessionBase";
    inline const char* TimelineWindow = "Timeline";
    inline const char* FilePropertiesWindow = "File Properties";
    inline const char* BucketsWindow = "Buckets";
    inline const char* ReaderProfilerWindow = "Dynamic Chunk Loading";
    inline const char* SchemaViewerWindow = "Contextual Schema";
    inline const char* ReplayTimeDataWindow = "Time Data";
    inline const char* ChunkIndexWindow = "Chunk Index";
    inline const char* TimelineEventsWindow = "Timeline Events";
    inline const char* EntityDetailsWindow = "Entity Details";
    inline const char* LogWindow = "Logs";
} // namespace VtxGuiNames


enum class TimeDisplayFormat { Ticks = 0, Formatted };
