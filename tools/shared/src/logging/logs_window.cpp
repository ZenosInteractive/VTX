#include "logging/logs_window.h"

#include <imgui.h>
#include <string>
#include <vector>

#include "gui/gui_types.h"

namespace {

    struct LogColor {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
    };

    struct LogRowViewModel {
        uint64_t sequence = 0;
        std::string timestamp;
        std::string level_label;
        std::string message;
        LogColor level_color;
    };

    struct LogsInputState {
        bool auto_scroll_enabled = true;
        bool clear_requested = false;
    };

    struct LogsEffect {
        bool clear_entries = false;
    };

    struct LogsViewModel {
        size_t entry_count = 0;
        std::vector<LogRowViewModel> rows;
    };

    struct LogsScreenResult {
        LogsViewModel view_model;
        LogsEffect effects;
    };

    // Maps logger level enum to a short on-screen label.
    const char* ToLevelLabel(VTX::Logger::Level level) {
        switch (level) {
        case VTX::Logger::Level::Info:
            return "Info";
        case VTX::Logger::Level::Warning:
            return "Warn";
        case VTX::Logger::Level::Error:
            return "Error";
        case VTX::Logger::Level::Debug:
            return "Debug";
        default:
            return "Unknown";
        }
    }

    // Maps logger level enum to the row color used in the log panel.
    LogColor ToLevelColor(VTX::Logger::Level level) {
        switch (level) {
        case VTX::Logger::Level::Info:
            return LogColor {0.55f, 0.80f, 1.00f, 1.00f};
        case VTX::Logger::Level::Warning:
            return LogColor {1.00f, 0.80f, 0.35f, 1.00f};
        case VTX::Logger::Level::Error:
            return LogColor {1.00f, 0.45f, 0.45f, 1.00f};
        case VTX::Logger::Level::Debug:
            return LogColor {0.60f, 0.95f, 0.95f, 1.00f};
        default:
            return LogColor {};
        }
    }

    // Converts raw log entries into view-model rows for rendering.
    std::vector<LogRowViewModel> BuildLogRows(const std::vector<VtxLogEntry>& entries) {
        std::vector<LogRowViewModel> rows;
        rows.reserve(entries.size());
        for (const auto& entry : entries) {
            rows.push_back(LogRowViewModel {
                .sequence = entry.sequence,
                .timestamp = entry.timestamp,
                .level_label = ToLevelLabel(entry.level),
                .message = entry.message,
                .level_color = ToLevelColor(entry.level),
            });
        }
        return rows;
    }

    // Computes render data and side effects from current logs + UI input.
    LogsScreenResult BuildLogScreen(const std::vector<VtxLogEntry>& entries, const LogsInputState& input) {
        LogsScreenResult result;
        result.effects.clear_entries = input.clear_requested;

        if (input.clear_requested) {
            return result;
        }

        result.view_model.rows = BuildLogRows(entries);
        result.view_model.entry_count = result.view_model.rows.size();
        return result;
    }

} // namespace

// Creates the logs window and applies non-focus-stealing behavior.
LogsWindow::LogsWindow(std::shared_ptr<VtxSessionBase> session)
    : ImGuiWindow(VtxGuiNames::LogWindow, std::move(session)) {
    flags_ |= ImGuiWindowFlags_NoFocusOnAppearing;
}

// Renders the log UI, then applies clear/scroll side effects.
void LogsWindow::DrawContent() {
    // Step 1: Capture current user intent for this frame.
    const auto logs = session_->GetLogsSnapshot();
    const bool clear_requested = ImGui::Button("Clear");

    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &auto_scroll_);

    const bool was_scrolled_to_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f;
    const auto screen = BuildLogScreen(logs, LogsInputState {
                                                 .auto_scroll_enabled = auto_scroll_,
                                                 .clear_requested = clear_requested,
                                             });

    ImGui::SameLine();
    ImGui::TextDisabled("%zu entries", screen.view_model.entry_count);

    ImGui::Separator();

    // Step 2: Draw current rows.
    if (ImGui::BeginChild("LogsScrollRegion", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar)) {
        // Capture follow-state inside the scrollable child, not the parent window.
        const bool was_scrolled_to_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f;

        // Render from the prepared view model so this widget remains stateless.
        for (const auto& row : screen.view_model.rows) {
            ImGui::TextDisabled("#%llu", row.sequence);
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", row.timestamp.c_str());
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(row.level_color.r, row.level_color.g, row.level_color.b, row.level_color.a),
                               "[%s]", row.level_label.c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s", row.message.c_str());
        }

        // Only follow new logs if user is already at the bottom.
        if (auto_scroll_ && was_scrolled_to_bottom) {
            ImGui::SetScrollHereY(1.0f);
        }
    }

    ImGui::EndChild();

    // Step 3: Apply deferred effects after draw.
    if (screen.effects.clear_entries) {
        // Apply side effects after rendering to keep UI read flow straightforward.
        session_->ClearLogs();
    }
}
