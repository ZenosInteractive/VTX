#include "windows/track_property_window.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

#include "gui/portable-file-dialogs.h"
#include "inspector_session.h"

namespace {

std::vector<std::string> ExtractBucketNamesFromSchema(const std::string& property_mapping) {
    std::vector<std::string> names;
    if (property_mapping.empty()) return names;
    const std::string key = "\"buckets\"";
    size_t key_pos = property_mapping.find(key);
    if (key_pos == std::string::npos) return names;
    size_t open_bracket = property_mapping.find('[', key_pos + key.size());
    if (open_bracket == std::string::npos) return names;
    bool in_string = false;
    bool escaping = false;
    std::string current;
    for (size_t i = open_bracket + 1; i < property_mapping.size(); ++i) {
        char c = property_mapping[i];
        if (c == ']' && !in_string) break;
        if (!in_string) { if (c == '"') { in_string = true; current.clear(); } continue; }
        if (escaping) { current.push_back(c); escaping = false; continue; }
        if (c == '\\') { escaping = true; continue; }
        if (c == '"') { in_string = false; if (!current.empty()) names.push_back(current); continue; }
        current.push_back(c);
    }
    return names;
}

std::string ToLowerCopy(const std::string& value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

bool ContainsCaseInsensitive(const std::string& text, const std::string& query_lower) {
    if (query_lower.empty()) return true;
    return ToLowerCopy(text).find(query_lower) != std::string::npos;
}

std::vector<std::string> ParseCommaSeparated(const char* input) {
    std::vector<std::string> result;
    std::istringstream stream(input);
    std::string token;
    while (std::getline(stream, token, ',')) {
        auto start = token.find_first_not_of(" \t");
        auto end = token.find_last_not_of(" \t");
        if (start != std::string::npos) {
            result.push_back(token.substr(start, end - start + 1));
        }
    }
    return result;
}

} // namespace

TrackPropertyWindow::TrackPropertyWindow(
    std::shared_ptr<InspectorSession> session,
    int instance_id)
    : ImGuiWindow("Track Property #" + std::to_string(instance_id), session)
    , inspector_session_(std::move(session))
{
}

void TrackPropertyWindow::PopulateBucketNames() {
    bucket_names_.clear();

    const auto& schema = inspector_session_->GetContextualSchema();
    bucket_names_ = ExtractBucketNamesFromSchema(schema.property_mapping);

    if (bucket_names_.empty()) {
        auto* reader = inspector_session_->GetReader();
        if (!reader) return;
        const auto* frame = reader->GetFrame(inspector_session_->GetCurrentFrame());
        if (!frame || frame->bucket_map.empty()) return;
        for (const auto& [name, idx] : frame->bucket_map) {
            bucket_names_.push_back(name);
        }
        std::sort(bucket_names_.begin(), bucket_names_.end());
    }

    end_frame_ = inspector_session_->GetTotalFrames() - 1;
    has_populated_buckets_ = true;
}

void TrackPropertyWindow::DrawContent() {
    if (!inspector_session_->HasLoadedReplay() || !inspector_session_->GetReader()) {
        ImGui::TextDisabled("Load a replay to use analysis.");
        return;
    }

    if (!has_populated_buckets_) {
        PopulateBucketNames();
    }

    if (scanner_.IsRunning()) {
        DrawProgressPanel();
    } else if (scanner_.GetTrackPropResult().is_complete) {
        DrawResultsPanel();
    } else {
        DrawConfigPanel();
    }
}

void TrackPropertyWindow::DrawConfigPanel() {
    ImGui::SeparatorText("Configuration");

    int total = inspector_session_->GetTotalFrames();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Start Frame", &start_frame_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("End Frame", &end_frame_);
    ImGui::SameLine();
    if (ImGui::Button("Whole File")) {
        start_frame_ = 0;
        end_frame_ = total - 1;
    }
    start_frame_ = std::clamp(start_frame_, 0, total - 1);
    end_frame_ = std::clamp(end_frame_, start_frame_, total - 1);

    // Bucket selector
    if (!bucket_names_.empty()) {
        if (ImGui::BeginCombo("Bucket", bucket_names_[selected_bucket_index_].c_str())) {
            for (int i = 0; i < static_cast<int>(bucket_names_.size()); ++i) {
                bool is_selected = (i == selected_bucket_index_);
                if (ImGui::Selectable(bucket_names_[i].c_str(), is_selected)) {
                    selected_bucket_index_ = i;
                }
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextDisabled("No buckets found in frame 0.");
    }

    // UID filter
    ImGui::InputTextWithHint("##UidFilter", "Filter by UniqueIDs (comma-separated, empty = all)",
        uid_filter_input_, IM_ARRAYSIZE(uid_filter_input_));

    // Property names (required)
    ImGui::InputTextWithHint("##PropertyNames",
        "Property names to track (comma-separated, e.g. Health,Score)",
        property_names_input_, IM_ARRAYSIZE(property_names_input_));

    ImGui::Spacing();

    auto prop_names = ParseCommaSeparated(property_names_input_);
    bool can_run = !bucket_names_.empty() && start_frame_ <= end_frame_ && !prop_names.empty();
    if (!can_run) ImGui::BeginDisabled();
    if (ImGui::Button("Run Analysis", ImVec2(140.0f, 0.0f))) {
        VtxServices::AnalysisScanner::ScanConfig config;
        config.start_frame = start_frame_;
        config.end_frame = end_frame_;
        config.bucket_index = selected_bucket_index_;
        config.bucket_name = bucket_names_[selected_bucket_index_];
        config.filter_unique_ids = ParseCommaSeparated(uid_filter_input_);
        config.property_names = prop_names;

        auto* reader = inspector_session_->GetReader();
        auto cache = reader->GetPropertyAddressCache();
        auto footer = reader->GetFooter();

        scanner_.Start(VtxServices::AnalysisScanner::ScanType::TrackProperty,
            config, reader, cache, footer);
    }
    if (!can_run) ImGui::EndDisabled();

    if (prop_names.empty() && property_names_input_[0] != '\0') {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Enter at least one property name.");
    }
}

void TrackPropertyWindow::DrawProgressPanel() {
    float progress = scanner_.GetProgress();
    const int current_frame = scanner_.GetCurrentFrame();
    if (current_frame >= 0 && current_frame != inspector_session_->GetCurrentFrame()) {
        inspector_session_->SetCurrentFrame(current_frame);
    }

    ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));
    ImGui::TextUnformatted(scanner_.GetStatus().c_str());
    if (current_frame >= 0) {
        ImGui::TextDisabled("Timeline frame: %d", current_frame);
    }

    if (ImGui::Button("Cancel")) {
        scanner_.Cancel();
    }
}

void TrackPropertyWindow::DrawResultsPanel() {
    const auto& result = scanner_.GetTrackPropResult();

    // Top bar: search + export + new scan
    ImGui::SetNextItemWidth(250.0f);
    ImGui::InputTextWithHint("##SearchFilter", "Search results...",
        search_filter_, IM_ARRAYSIZE(search_filter_));
    ImGui::SameLine();
    if (ImGui::Button("Export CSV")) {
        auto dest = pfd::save_file("Export Track Property CSV", "track_property.csv",
            { "CSV Files (.csv)", "*.csv", "All Files", "*" });
        if (!dest.result().empty()) {
            VtxServices::ExportTrackPropertyCsv(dest.result(), result);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("New Scan")) {
        search_filter_[0] = '\0';
        scanner_.Reset();
        has_populated_buckets_ = false;
        return;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(%zu spans)", result.spans.size());

    if (!result.error_message.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "%s", result.error_message.c_str());
    }

    ImGui::Separator();

    if (result.spans.empty()) {
        ImGui::TextDisabled("No rows to display.");
        return;
    }

    constexpr ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Sortable;

    if (ImGui::BeginTable("##TrackPropTable", 5, table_flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("UniqueId", ImGuiTableColumnFlags_DefaultSort);
        ImGui::TableSetupColumn("Property");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Range (Frame)");
        ImGui::TableSetupColumn("Range (GameTime)");
        ImGui::TableHeadersRow();

        const std::string filter_query = ToLowerCopy(search_filter_);

        for (const auto& span : result.spans) {
            std::string frame_range = VtxServices::FormatSingleFrameRange(span.range);
            std::string time_range = VtxServices::FormatSingleGameTimeRange(span.range);

            if (!filter_query.empty()) {
                bool matches = ContainsCaseInsensitive(span.unique_id, filter_query)
                    || ContainsCaseInsensitive(span.property_name, filter_query)
                    || ContainsCaseInsensitive(span.value, filter_query)
                    || ContainsCaseInsensitive(frame_range, filter_query)
                    || ContainsCaseInsensitive(time_range, filter_query);
                if (!matches) continue;
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(span.unique_id.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(span.property_name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(span.value.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(frame_range.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(time_range.c_str());
        }

        ImGui::EndTable();
    }
}
