#include "windows/filter_entities_window.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

#include <imgui.h>

#include "gui/portable-file-dialogs.h"
#include "inspector_session.h"
#include "vtx/reader/core/vtx_reader_facade.h"

namespace {

    const ImVec4 kOk {0.40f, 0.85f, 0.45f, 1.0f};
    const ImVec4 kWarn {0.95f, 0.80f, 0.30f, 1.0f};
    const ImVec4 kErr {0.95f, 0.45f, 0.45f, 1.0f};
    const ImVec4 kDim {0.65f, 0.65f, 0.65f, 1.0f};

    std::string Trim(std::string s) {
        const auto not_space = [](unsigned char c) {
            return !std::isspace(c);
        };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
        s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
        return s;
    }

    // Non-empty, trimmed lines of a text buffer.
    std::vector<std::string> Lines(const char* text) {
        std::vector<std::string> out;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            line = Trim(line);
            if (!line.empty()) {
                out.push_back(line);
            }
        }
        return out;
    }

    std::string FormatBytes(uint64_t bytes) {
        char buffer[64];
        if (bytes >= 1024ull * 1024ull) {
            std::snprintf(buffer, sizeof(buffer), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        } else if (bytes >= 1024ull) {
            std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
        } else {
            std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
        }
        return buffer;
    }

} // namespace

FilterEntitiesWindow::FilterEntitiesWindow(std::shared_ptr<InspectorSession> session)
    : session_(std::move(session)) {
    if (session_ && session_->HasLoadedReplay()) {
        if (VTX::IVtxReaderFacade* reader = session_->GetReader()) {
            schema_cache_ = reader->GetPropertyAddressCache();
            for (const auto& [type_id, struct_cache] : schema_cache_.structs) {
                structs_.push_back(StructRow {struct_cache.name, type_id, false});
            }
            std::sort(structs_.begin(), structs_.end(),
                      [](const StructRow& a, const StructRow& b) { return a.name < b.name; });
            for (size_t b = 0; b < schema_cache_.bucket_names.size(); ++b) {
                buckets_.push_back(BucketRow {schema_cache_.bucket_names[b], static_cast<int32_t>(b), false});
            }
        }
    }
}

FilterEntitiesWindow::~FilterEntitiesWindow() {
    JoinWorker();
}

void FilterEntitiesWindow::JoinWorker() {
    if (future_.valid()) {
        if (progress_) {
            progress_->cancel.store(true);
        }
        future_.wait();
    }
}

void FilterEntitiesWindow::OnRender() {
    if (!is_open_) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(640, 700), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Filter Entities", &is_open_)) {
        DrawContent();
    }
    ImGui::End();
}

VTX::ReplayFilterSpec FilterEntitiesWindow::BuildSpec() const {
    VTX::ReplayFilterSpec spec;
    spec.mode = mode_ == 1 ? VTX::ReplayFilterMode::Keep : VTX::ReplayFilterMode::Drop;
    spec.prune_events = prune_events_;
    for (const BucketRow& row : buckets_) {
        if (row.selected) {
            spec.rules.push_back(VTX::ReplayFilterRule::Bucket(std::to_string(row.index)));
        }
    }
    for (const StructRow& row : structs_) {
        if (row.selected) {
            spec.rules.push_back(VTX::ReplayFilterRule::StructName(row.name, case_insensitive_));
        }
    }
    for (const std::string& pattern : Lines(unique_id_patterns_)) {
        spec.rules.push_back(VTX::ReplayFilterRule::UniqueId(pattern, case_insensitive_));
    }
    for (const PropertyRow& row : property_rules_) {
        const std::string field = Trim(row.field_name);
        const std::string pattern = Trim(row.pattern);
        if (field.empty() && pattern.empty()) {
            continue; // untouched row
        }
        spec.rules.push_back(VTX::ReplayFilterRule::Property(Trim(row.struct_name), field, pattern, case_insensitive_));
    }
    return spec;
}

std::string FilterEntitiesWindow::SpecSignature(const VTX::ReplayFilterSpec& spec) {
    std::string key = spec.mode == VTX::ReplayFilterMode::Keep ? "keep" : "drop";
    key += spec.prune_events ? "|prune" : "|carry";
    for (const VTX::ReplayFilterRule& rule : spec.rules) {
        key += '|';
        key += std::to_string(static_cast<int>(rule.kind));
        key += rule.case_insensitive ? "i:" : "c:";
        key += rule.struct_name;
        key += '.';
        key += rule.field_name;
        key += '=';
        key += rule.pattern;
    }
    return key;
}

void FilterEntitiesWindow::RefreshPreview(const VTX::ReplayFilterSpec& spec, const std::string& key) {
    preview_ = Preview {};
    preview_.key = key;
    preview_.per_struct_seen.assign(structs_.size(), 0);
    preview_.per_bucket_seen.assign(buckets_.size(), 0);

    VTX::IVtxReaderFacade* reader = session_ ? session_->GetReader() : nullptr;
    if (!reader) {
        return;
    }
    preview_.frame_index = std::max(0, session_->GetCurrentFrame());
    const VTX::Frame* frame = reader->GetFrameSync(preview_.frame_index);
    if (!frame) {
        return;
    }

    std::unordered_map<int32_t, size_t> struct_rows;
    for (size_t i = 0; i < structs_.size(); ++i) {
        struct_rows[structs_[i].type_id] = i;
    }

    std::unique_ptr<VTX::ReplayFilterMatcher> matcher;
    if (!spec.rules.empty()) {
        matcher = std::make_unique<VTX::ReplayFilterMatcher>(spec, schema_cache_);
        if (!matcher->valid()) {
            preview_.error = matcher->error();
            matcher.reset();
        }
    }
    preview_.has_matcher = matcher != nullptr;

    const auto& buckets = frame->GetBuckets();
    for (size_t b = 0; b < buckets.size(); ++b) {
        const VTX::Bucket& bucket = buckets[b];
        if (b < preview_.per_bucket_seen.size()) {
            preview_.per_bucket_seen[b] = bucket.entities.size();
        }
        uint64_t kept = 0;
        for (size_t i = 0; i < bucket.entities.size(); ++i) {
            const auto row = struct_rows.find(bucket.entities[i].entity_type_id);
            if (row != struct_rows.end()) {
                ++preview_.per_struct_seen[row->second];
            }
            if (matcher && matcher->Keeps(static_cast<int32_t>(b), bucket, i)) {
                ++kept;
            }
        }
        std::string name = b < schema_cache_.bucket_names.size() ? schema_cache_.bucket_names[b] : std::string();
        if (name.empty()) {
            name = "bucket " + std::to_string(b);
        }
        preview_.buckets.emplace_back(std::move(name),
                                      std::make_pair(kept, static_cast<uint64_t>(bucket.entities.size())));
        preview_.seen += bucket.entities.size();
        preview_.kept += kept;
    }
}

void FilterEntitiesWindow::StartFilter(const VTX::ReplayFilterSpec& spec, const std::string& dest_path) {
    JoinWorker();
    phase_ = Phase::Running;
    outcome_ = VTX::ReplayFilterResult {};
    outcome_dest_ = dest_path;
    outcome_logged_ = false;
    progress_ = std::make_shared<Progress>();

    // The worker touches only its by-value captures and the shared progress block.
    const std::shared_ptr<Progress> progress = progress_;
    const std::string source = session_->current_file_path_;
    future_ = std::async(std::launch::async, [source, dest_path, spec, progress]() {
        return VTX::FilterReplayFile(source, dest_path, spec, [progress](int32_t done, int32_t total) {
            progress->done.store(done);
            progress->total.store(total);
            return !progress->cancel.load();
        });
    });
}

void FilterEntitiesWindow::DrawContent() {
    if (phase_ == Phase::Running && future_.valid() &&
        future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        outcome_ = future_.get();
        phase_ = Phase::Done;
        if (!outcome_logged_ && session_) {
            outcome_logged_ = true;
            if (outcome_.ok()) {
                session_->AddGuiInfoLog("Filter Entities: wrote " + outcome_dest_ + " (" +
                                        std::to_string(outcome_.entities_kept) + " entities kept, " +
                                        std::to_string(outcome_.entities_dropped) + " dropped)");
            } else {
                session_->AddGuiErrorLog("Filter Entities failed: " + outcome_.error);
            }
        }
    }

    if (!session_ || !session_->HasLoadedReplay()) {
        ImGui::TextColored(kWarn, "Open a replay first.");
        return;
    }

    const std::filesystem::path source(session_->current_file_path_);
    ImGui::TextWrapped("Source: %s", source.filename().string().c_str());
    ImGui::TextColored(kDim,
                       "%d frames, %d buckets, %d structs in the schema. Every frame is kept; only the entity "
                       "payload changes.",
                       session_->GetTotalFrames(), static_cast<int>(buckets_.size()),
                       static_cast<int>(structs_.size()));
    ImGui::Separator();

    const bool busy = phase_ == Phase::Running;
    ImGui::BeginDisabled(busy);
    ImGui::TextUnformatted("Mode");
    ImGui::SameLine();
    ImGui::RadioButton("Drop matching (blacklist)", &mode_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Keep only matching (whitelist)", &mode_, 1);
    ImGui::Spacing();
    DrawRules();
    ImGui::EndDisabled();

    const VTX::ReplayFilterSpec spec = BuildSpec();
    ImGui::Separator();
    DrawPreview(spec);
    ImGui::Separator();
    DrawRunControls(spec);
    DrawOutcome();
}

void FilterEntitiesWindow::DrawRules() {
    if (ImGui::CollapsingHeader("Buckets", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SmallButton("Select all##buckets")) {
            for (BucketRow& row : buckets_) {
                row.selected = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##buckets")) {
            for (BucketRow& row : buckets_) {
                row.selected = false;
            }
        }
        ImGui::SameLine();
        ImGui::TextColored(kDim, "(a dropped bucket is emptied in every frame; its slot stays)");
        if (buckets_.empty()) {
            ImGui::TextColored(kDim, "The schema names no buckets.");
        }
        for (size_t i = 0; i < buckets_.size(); ++i) {
            BucketRow& row = buckets_[i];
            ImGui::PushID(static_cast<int>(i) + 2000);
            const uint64_t count = i < preview_.per_bucket_seen.size() ? preview_.per_bucket_seen[i] : 0;
            char label[256];
            std::snprintf(label, sizeof(label), "%d: %s  (%llu)", row.index, row.name.c_str(),
                          static_cast<unsigned long long>(count));
            ImGui::Checkbox(label, &row.selected);
            ImGui::PopID();
        }
    }

    if (ImGui::CollapsingHeader("Structs", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SmallButton("Select all")) {
            for (StructRow& row : structs_) {
                row.selected = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            for (StructRow& row : structs_) {
                row.selected = false;
            }
        }
        ImGui::SameLine();
        ImGui::TextColored(kDim, "(count = entities of that struct in the current frame)");

        const float row_height = ImGui::GetFrameHeightWithSpacing();
        const float height =
            std::min(220.0f, row_height * static_cast<float>(std::max<size_t>(structs_.size(), 1)) + 8.0f);
        if (ImGui::BeginChild("##structs", ImVec2(0, height), ImGuiChildFlags_Border)) {
            for (size_t i = 0; i < structs_.size(); ++i) {
                StructRow& row = structs_[i];
                ImGui::PushID(static_cast<int>(i));
                const uint64_t count = i < preview_.per_struct_seen.size() ? preview_.per_struct_seen[i] : 0;
                char label[256];
                std::snprintf(label, sizeof(label), "%s  (%llu)", row.name.c_str(),
                              static_cast<unsigned long long>(count));
                ImGui::Checkbox(label, &row.selected);
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    if (ImGui::CollapsingHeader("Unique id patterns", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextColored(kDim, "One glob per line: * matches any run of characters, ? exactly one.");
        ImGui::InputTextMultiline("##unique_ids", unique_id_patterns_, sizeof(unique_id_patterns_),
                                  ImVec2(-FLT_MIN, 70));
    }

    if (ImGui::CollapsingHeader("Property rules", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextColored(kDim, "Match one scalar field (bool/int/float/double/string) by its text value. "
                                 "Leave the struct empty for any struct with that field.");
        int remove_index = -1;
        for (size_t i = 0; i < property_rules_.size(); ++i) {
            PropertyRow& row = property_rules_[i];
            ImGui::PushID(static_cast<int>(i) + 1000);
            ImGui::SetNextItemWidth(170);
            ImGui::InputTextWithHint("##struct", "struct (any)", row.struct_name, sizeof(row.struct_name));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            ImGui::InputTextWithHint("##field", "field", row.field_name, sizeof(row.field_name));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-40);
            ImGui::InputTextWithHint("##value", "value glob", row.pattern, sizeof(row.pattern));
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                remove_index = static_cast<int>(i);
            }
            ImGui::PopID();
        }
        if (remove_index >= 0) {
            property_rules_.erase(property_rules_.begin() + remove_index);
        }
        if (ImGui::SmallButton("Add property rule")) {
            property_rules_.emplace_back();
        }
    }

    ImGui::Spacing();
    ImGui::Checkbox("Case-insensitive patterns", &case_insensitive_);
    ImGui::SameLine();
    ImGui::Checkbox("Prune timeline events of dropped entities", &prune_events_);
}

void FilterEntitiesWindow::DrawPreview(const VTX::ReplayFilterSpec& spec) {
    const std::string key = SpecSignature(spec) + "#" + std::to_string(session_->GetCurrentFrame());
    if (key != preview_.key) {
        RefreshPreview(spec, key);
    }

    if (spec.rules.empty()) {
        ImGui::TextColored(kWarn, "Add at least one rule: tick a bucket or struct, enter a unique id pattern or add a "
                                  "property rule.");
        return;
    }
    if (!preview_.error.empty()) {
        ImGui::TextColored(kErr, "Rules cannot be resolved:");
        ImGui::TextWrapped("%s", preview_.error.c_str());
        return;
    }
    ImGui::Text("Preview on frame %d: keeps %llu of %llu entities, drops %llu", preview_.frame_index,
                static_cast<unsigned long long>(preview_.kept), static_cast<unsigned long long>(preview_.seen),
                static_cast<unsigned long long>(preview_.seen - preview_.kept));
    for (const auto& [name, counts] : preview_.buckets) {
        ImGui::TextColored(kDim, "  %s: %llu of %llu kept", name.c_str(), static_cast<unsigned long long>(counts.first),
                           static_cast<unsigned long long>(counts.second));
    }
    ImGui::TextColored(kDim, "The rewrite applies the same rules to every frame.");
}

void FilterEntitiesWindow::DrawRunControls(const VTX::ReplayFilterSpec& spec) {
    const bool busy = phase_ == Phase::Running;
    const bool can_run = !busy && !spec.rules.empty() && preview_.error.empty();

    ImGui::BeginDisabled(!can_run);
    if (ImGui::Button("Filter && Save As...", ImVec2(170, 0))) {
        namespace fs = std::filesystem;
        const fs::path source(session_->current_file_path_);
        const std::string default_name = source.stem().string() + "_filtered.vtx";
        auto dialog = pfd::save_file("Save filtered replay", (source.parent_path() / default_name).string(),
                                     {"VTX Files (.vtx)", "*.vtx", "All Files", "*"});
        std::string dest = dialog.result();
        if (!dest.empty()) {
            if (fs::path(dest).extension().empty()) {
                dest += ".vtx";
            }
            StartFilter(spec, dest);
        }
    }
    ImGui::EndDisabled();

    if (busy) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel") && progress_) {
            progress_->cancel.store(true);
        }
    }
    if (phase_ == Phase::Done) {
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            phase_ = Phase::Idle;
            outcome_ = VTX::ReplayFilterResult {};
            outcome_dest_.clear();
            outcome_logged_ = false;
        }
    }

    if (busy && progress_) {
        const int32_t done = progress_->done.load();
        const int32_t total = progress_->total.load();
        const float fraction = total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%d / %d chunks", done, total);
        ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), overlay);
        ImGui::TextColored(kDim, "Reading every chunk, dropping entities and rewriting it...");
    }
}

void FilterEntitiesWindow::DrawOutcome() {
    if (phase_ != Phase::Done) {
        return;
    }
    ImGui::Spacing();
    if (!outcome_.ok()) {
        ImGui::TextColored(kErr, "Filter FAILED");
        ImGui::TextWrapped("%s", outcome_.error.c_str());
        return;
    }
    ImGui::TextColored(kOk, "Filter SUCCEEDED");
    ImGui::TextWrapped("Wrote %s", outcome_dest_.c_str());
    ImGui::Text("%d frames in %d chunks, %.2f s", outcome_.total_frames, outcome_.chunks_rewritten,
                outcome_.elapsed_seconds);
    ImGui::Text("Entities: %llu kept, %llu dropped (of %llu)", static_cast<unsigned long long>(outcome_.entities_kept),
                static_cast<unsigned long long>(outcome_.entities_dropped),
                static_cast<unsigned long long>(outcome_.entities_seen));
    if (outcome_.events_kept > 0 || outcome_.events_dropped > 0) {
        ImGui::Text("Timeline events: %llu kept, %llu dropped", static_cast<unsigned long long>(outcome_.events_kept),
                    static_cast<unsigned long long>(outcome_.events_dropped));
    }
    ImGui::Text("Size: %s -> %s", FormatBytes(outcome_.source_bytes).c_str(),
                FormatBytes(outcome_.output_bytes).c_str());
    if (!outcome_.dropped_by_bucket.empty()) {
        ImGui::TextColored(kDim, "Dropped by bucket:");
        for (const auto& [name, count] : outcome_.dropped_by_bucket) {
            ImGui::TextColored(kDim, "  %s: %llu", name.c_str(), static_cast<unsigned long long>(count));
        }
    }
    if (!outcome_.dropped_by_struct.empty()) {
        ImGui::TextColored(kDim, "Dropped by struct:");
        for (const auto& [name, count] : outcome_.dropped_by_struct) {
            ImGui::TextColored(kDim, "  %s: %llu", name.c_str(), static_cast<unsigned long long>(count));
        }
    }
    ImGui::TextColored(kDim, "The new file opens like any other replay: same frames, same timestamps, same schema.");
}
