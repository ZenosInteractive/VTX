#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vtx/reader/core/vtx_reader_facade.h"

namespace VtxServices {

/// Cached state for avoiding redundant string rebuilds each frame.
struct ReaderProfileCacheState {
    std::vector<int32_t> last_loaded;
    std::vector<int32_t> last_loading;
    std::string cached_loaded_str;
    std::string cached_loading_str;
};

/// Presentation model for the reader profiler widget.
struct ReaderProfileViewModel {
    size_t loaded_count = 0;
    size_t loading_count = 0;
    std::string loaded_chunk_ids;
    std::string loading_chunk_ids;
    bool has_pending_chunks = false;
    char spinner = '|';
};

/// Single row in the reader profile table.
struct ReaderProfileRow {
    std::string state_label;
    std::string chunk_text;
    bool highlighted = false;
    bool disabled = false;
};

/// UI presentation helpers for chunk-state visualization.
/// Core state tracking now lives in the SDK (VTX::ReaderChunkState).
class ReaderChunkStateService {
public:
    static ReaderProfileViewModel BuildProfileViewModel(
        const VTX::ReaderChunkSnapshot& snapshot,
        double ui_time_seconds,
        ReaderProfileCacheState& io_cache);
    static std::vector<ReaderProfileRow> BuildProfileRows(const ReaderProfileViewModel& view_model);
};

} // namespace VtxServices
