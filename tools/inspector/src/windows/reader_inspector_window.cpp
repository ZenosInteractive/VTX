#include "windows/reader_inspector_window.h"

#include <algorithm>
#include <imgui.h>
#include <utility>

#include "inspector_session.h"

namespace {

    // Resolves which chunk currently owns the requested frame.
    int FindActiveChunkIndex(const std::vector<VTX::ChunkIndexEntry>& seek_table, int frame_index) {
        const auto it =
            std::lower_bound(seek_table.begin(), seek_table.end(), frame_index,
                             [](const VTX::ChunkIndexEntry& entry, int value) { return entry.end_frame < value; });

        if (it == seek_table.end() || frame_index < it->start_frame) {
            return -1;
        }

        return it->chunk_index;
    }

    // Builds text showing active chunk and neighborhood window.
    std::string BuildActiveChunkWindowText(const std::vector<VTX::ChunkIndexEntry>& seek_table, int active_chunk_index,
                                           int backward_count, int forward_count) {
        if (seek_table.empty() || active_chunk_index < 0) {
            return "Unavailable";
        }

        const int start = std::max(0, active_chunk_index - backward_count);
        const int end = std::min(static_cast<int>(seek_table.size()) - 1, active_chunk_index + forward_count);

        std::string text;
        for (int chunk_index = start; chunk_index <= end; ++chunk_index) {
            if (!text.empty()) {
                text += ' ';
            }

            if (chunk_index == active_chunk_index) {
                text += '[';
                text += std::to_string(chunk_index);
                text += ']';
                continue;
            }

            text += std::to_string(chunk_index);
        }

        return text;
    }

    // Builds text showing loaded chunks within the configured cache window.
    std::string BuildLoadedChunkWindowText(const std::vector<int32_t>& loaded_chunks,
                                           const std::vector<VTX::ChunkIndexEntry>& seek_table, int active_chunk_index,
                                           int backward_count, int forward_count) {
        if (seek_table.empty() || active_chunk_index < 0) {
            return "Unavailable";
        }

        const int start = std::max(0, active_chunk_index - backward_count);
        const int end = std::min(static_cast<int>(seek_table.size()) - 1, active_chunk_index + forward_count);

        std::string text;
        for (int chunk_index = start; chunk_index <= end; ++chunk_index) {
            // Show only chunks that are currently resident in memory.
            if (std::find(loaded_chunks.begin(), loaded_chunks.end(), chunk_index) == loaded_chunks.end()) {
                continue;
            }

            if (!text.empty()) {
                text += ' ';
            }

            if (chunk_index == active_chunk_index) {
                text += '[';
                text += std::to_string(chunk_index);
                text += ']';
                continue;
            }

            text += std::to_string(chunk_index);
        }

        return text.empty() ? "None" : text;
    }

} // namespace

// Constructs reader profiler window bound to Inspector session.
ReaderInspectorWindow::ReaderInspectorWindow(std::shared_ptr<InspectorSession> session)
    : ImGuiWindow(VtxGuiNames::ReaderProfilerWindow, session)
    , inspector_session_(std::move(session)) {}

// Renders cache window controls and live chunk-state profile table.
void ReaderInspectorWindow::DrawContent() {
    VTX::IVtxReaderFacade* reader = inspector_session_->GetReader();
    if (!inspector_session_->HasLoadedReplay() || !reader) {
        last_applied_reader_ = nullptr;
        ImGui::TextDisabled("Reader offline.");
        return;
    }

    ImGui::Text("Cache Window");
    ImGui::Separator();
    ImGui::Spacing();

    bool cache_window_changed = false;

    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputInt("Backward Chunks", &cache_window_backward_, 0, 0)) {
        cache_window_changed = true;
    }
    cache_window_backward_ = std::max(0, cache_window_backward_);

    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputInt("Forward Chunks", &cache_window_forward_, 0, 0)) {
        cache_window_changed = true;
    }
    cache_window_forward_ = std::max(0, cache_window_forward_);

    const auto& seek_table = reader->GetSeekTable();
    const int active_chunk_index = FindActiveChunkIndex(seek_table, inspector_session_->GetCurrentFrame());
    ImGui::TextDisabled("Chunks kept in RAM behind and ahead of the active chunk.");
    ImGui::Spacing();

    // Step 1: Push cache window changes to reader if needed.
    ApplyCacheWindowIfNeeded(reader, cache_window_changed || last_applied_reader_ != reader);

    // Step 2: Build and render current profile rows.
    const auto snapshot = inspector_session_->GetReaderChunkSnapshot();
    const auto vm =
        VtxServices::ReaderChunkStateService::BuildProfileViewModel(snapshot, ImGui::GetTime(), profile_cache_);
    auto rows = VtxServices::ReaderChunkStateService::BuildProfileRows(vm);
    if (!rows.empty()) {
        // Replace the "Loaded" row with a focused cache-window snapshot.
        const std::string loaded_chunk_window = BuildLoadedChunkWindowText(
            snapshot.loaded_chunks, seek_table, active_chunk_index, cache_window_backward_, cache_window_forward_);
        rows.front().chunk_text = loaded_chunk_window;
        rows.front().disabled = loaded_chunk_window == "Unavailable" || loaded_chunk_window == "None";
    }

    ImGui::Text("Active Reader Profile");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTable("ChunksTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Event State", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Chunk IDs", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& row : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (row.highlighted) {
                ImGui::TextColored(row.state_label.rfind("Loaded", 0) == 0 ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f)
                                                                           : ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                   "%s", row.state_label.c_str());
            } else if (row.disabled) {
                ImGui::TextDisabled("%s", row.state_label.c_str());
            } else {
                ImGui::Text("%s", row.state_label.c_str());
            }

            ImGui::TableNextColumn();
            if (row.highlighted) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", row.chunk_text.c_str());
            } else if (row.disabled) {
                ImGui::TextDisabled("%s", row.chunk_text.c_str());
            } else {
                ImGui::TextWrapped("%s", row.chunk_text.c_str());
            }
        }

        ImGui::EndTable();
    }
}

// Applies cache window to reader only when values changed or forced.
void ReaderInspectorWindow::ApplyCacheWindowIfNeeded(VTX::IVtxReaderFacade* reader, bool force_apply) {
    if (!reader) {
        return;
    }

    if (!force_apply && last_applied_reader_ == reader && last_applied_backward_ == cache_window_backward_ &&
        last_applied_forward_ == cache_window_forward_) {
        // Avoid redundant SetCacheWindow calls while values are unchanged.
        return;
    }

    reader->SetCacheWindow(static_cast<uint32_t>(cache_window_backward_), static_cast<uint32_t>(cache_window_forward_));

    last_applied_reader_ = reader;
    last_applied_backward_ = cache_window_backward_;
    last_applied_forward_ = cache_window_forward_;
}
