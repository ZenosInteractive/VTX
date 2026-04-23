#include "services/reader_chunk_state_service.h"

namespace VtxServices {

    ReaderProfileViewModel ReaderChunkStateService::BuildProfileViewModel(const VTX::ReaderChunkSnapshot& snapshot,
                                                                          double ui_time_seconds,
                                                                          ReaderProfileCacheState& io_cache) {
        if (io_cache.last_loaded != snapshot.loaded_chunks || io_cache.last_loading != snapshot.loading_chunks) {
            io_cache.last_loaded = snapshot.loaded_chunks;
            io_cache.last_loading = snapshot.loading_chunks;

            io_cache.cached_loaded_str.clear();
            for (int id : io_cache.last_loaded) {
                io_cache.cached_loaded_str += std::to_string(id) + " ";
            }

            io_cache.cached_loading_str.clear();
            for (int id : io_cache.last_loading) {
                io_cache.cached_loading_str += std::to_string(id) + " ";
            }
        }

        const char* spinner = "|/-\\";
        const int spinner_idx = static_cast<int>(ui_time_seconds * 10.0) % 4;

        ReaderProfileViewModel vm;
        vm.loaded_count = io_cache.last_loaded.size();
        vm.loading_count = io_cache.last_loading.size();
        vm.loaded_chunk_ids = io_cache.cached_loaded_str;
        vm.loading_chunk_ids = io_cache.cached_loading_str;
        vm.has_pending_chunks = !io_cache.last_loading.empty();
        vm.spinner = spinner[spinner_idx];
        return vm;
    }

    std::vector<ReaderProfileRow> ReaderChunkStateService::BuildProfileRows(const ReaderProfileViewModel& view_model) {
        std::vector<ReaderProfileRow> rows;
        rows.push_back(ReaderProfileRow {
            .state_label = "Loaded (RAM) [" + std::to_string(view_model.loaded_count) + "]",
            .chunk_text = view_model.loaded_count == 0 ? "None" : view_model.loaded_chunk_ids,
            .highlighted = true,
            .disabled = view_model.loaded_count == 0,
        });
        rows.push_back(ReaderProfileRow {
            .state_label = view_model.has_pending_chunks
                               ? "Pending (I/O) " + std::string(1, view_model.spinner) + " [" +
                                     std::to_string(view_model.loading_count) + "]"
                               : "Pending (I/O) [" + std::to_string(view_model.loading_count) + "]",
            .chunk_text = view_model.has_pending_chunks ? view_model.loading_chunk_ids : "Idle",
            .highlighted = view_model.has_pending_chunks,
            .disabled = !view_model.has_pending_chunks,
        });
        return rows;
    }

} // namespace VtxServices
