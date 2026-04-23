#include "services/analysis_scanner.h"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace VtxServices {

    namespace {

        // Resolves the selected analysis bucket using the same index ordering as the bucket tree.
        const VTX::Bucket* ResolveScanBucket(const VTX::Frame& frame, const AnalysisScanner::ScanConfig& config) {
            const auto& buckets = frame.GetBuckets();
            if (config.bucket_index >= 0 && static_cast<size_t>(config.bucket_index) < buckets.size()) {
                return &buckets[static_cast<size_t>(config.bucket_index)];
            }

            const auto bucket_it = frame.bucket_map.find(config.bucket_name);
            if (bucket_it == frame.bucket_map.end()) {
                return nullptr;
            }

            const size_t resolved_index = static_cast<size_t>(bucket_it->second);
            return resolved_index < buckets.size() ? &buckets[resolved_index] : nullptr;
        }

        // Builds a readable label for bucket-related scan diagnostics.
        std::string BuildBucketDebugLabel(const AnalysisScanner::ScanConfig& config) {
            return std::format("'{}' (index {})", config.bucket_name, config.bucket_index);
        }

    } // namespace

    AnalysisScanner::~AnalysisScanner() {
        cancel_requested_.store(true);
        if (worker_.valid()) {
            worker_.wait();
        }
    }

    void AnalysisScanner::Start(ScanType type, const ScanConfig& config, VTX::IVtxReaderFacade* reader,
                                const VTX::PropertyAddressCache& cache, const VTX::FileFooter& footer) {
        if (is_running_.load())
            return;

        // Step 1: Reset the reusable scanner state before starting a new worker task.
        cancel_requested_.store(false);
        progress_.store(0.0f);
        current_frame_.store(-1);
        lifetime_result_ = {};
        unique_props_result_ = {};
        track_prop_result_ = {};
        is_running_.store(true);

        // Step 2: Run the selected scan on a worker thread and mark the scanner idle on exit.
        worker_ = std::async(std::launch::async, [this, type, config, reader, cache, footer]() {
            switch (type) {
            case ScanType::EntityLifeTime:
                RunEntityLifeTime(config, reader, cache, footer);
                break;
            case ScanType::UniqueProperties:
                RunUniqueProperties(config, reader, cache, footer);
                break;
            case ScanType::TrackProperty:
                RunTrackProperty(config, reader, cache, footer);
                break;
            }
            is_running_.store(false);
        });
    }

    void AnalysisScanner::Cancel() {
        cancel_requested_.store(true);
    }

    void AnalysisScanner::Reset() {
        Cancel();
        if (worker_.valid()) {
            worker_.wait();
        }
        is_running_.store(false);
        cancel_requested_.store(false);
        progress_.store(0.0f);
        current_frame_.store(-1);
        {
            std::scoped_lock lock(status_mutex_);
            status_message_.clear();
        }
        lifetime_result_ = {};
        unique_props_result_ = {};
        track_prop_result_ = {};
    }

    std::string AnalysisScanner::GetStatus() const {
        std::scoped_lock lock(status_mutex_);
        return status_message_;
    }

    void AnalysisScanner::SetStatus(const std::string& msg) {
        std::scoped_lock lock(status_mutex_);
        status_message_ = msg;
    }

    uint64_t AnalysisScanner::GetGameTime(const VTX::FileFooter& footer, int32_t frame_index) {
        auto idx = static_cast<size_t>(frame_index);
        if (idx < footer.times.game_time.size()) {
            return footer.times.game_time[idx];
        }
        return 0;
    }

    bool AnalysisScanner::PassesUidFilter(const std::vector<std::string>& filter, const std::string& uid) {
        if (filter.empty())
            return true;
        return std::find(filter.begin(), filter.end(), uid) != filter.end();
    }

    // Scans the requested frame span and records every contiguous lifetime range per entity.
    void AnalysisScanner::RunEntityLifeTime(const ScanConfig& config, VTX::IVtxReaderFacade* reader,
                                            const VTX::PropertyAddressCache& cache, const VTX::FileFooter& footer) {
        SetStatus("Scanning entity lifetimes...");

        const int32_t total_range = config.end_frame - config.start_frame + 1;
        int32_t frames_with_bucket = 0;

        // Step 1: Track every entity that appears in the selected bucket.
        std::unordered_map<std::string, EntityLifeTimeEntry> entity_map;
        std::unordered_set<std::string> present_last_frame;
        std::unordered_set<std::string> has_open_range;

        for (int32_t f = config.start_frame; f <= config.end_frame; ++f) {
            if (cancel_requested_.load()) {
                SetStatus("Cancelled.");
                lifetime_result_.error_message = "Cancelled by user.";
                return;
            }

            current_frame_.store(f);
            progress_.store(static_cast<float>(f - config.start_frame + 1) / static_cast<float>(total_range));
            SetStatus(std::format("Scanning entity lifetimes... frame {} / {}", f, config.end_frame));

            const auto* frame = reader->GetFrameSync(f);
            if (!frame)
                continue;

            const auto* bucket = ResolveScanBucket(*frame, config);
            if (!bucket)
                continue;
            ++frames_with_bucket;

            // Step 2: Open or extend ranges for entities present in the current frame.
            std::unordered_set<std::string> present_this_frame;
            for (size_t i = 0; i < bucket->unique_ids.size(); ++i) {
                const auto& uid = bucket->unique_ids[i];
                if (!PassesUidFilter(config.filter_unique_ids, uid))
                    continue;
                present_this_frame.insert(uid);

                if (!has_open_range.contains(uid)) {
                    auto& entry = entity_map[uid];
                    if (entry.unique_id.empty()) {
                        entry.unique_id = uid;
                        if (i < bucket->entities.size()) {
                            entry.type_id = bucket->entities[i].entity_type_id;
                            auto sc_it = cache.structs.find(entry.type_id);
                            if (sc_it != cache.structs.end()) {
                                entry.type_name = sc_it->second.name;
                            }
                        }
                    }
                    entry.ranges.push_back(FrameRange {
                        .start_frame = f,
                        .end_frame = f,
                        .start_game_time = GetGameTime(footer, f),
                        .end_game_time = GetGameTime(footer, f),
                    });
                    has_open_range.insert(uid);
                } else {
                    auto& entry = entity_map[uid];
                    if (!entry.ranges.empty()) {
                        entry.ranges.back().end_frame = f;
                        entry.ranges.back().end_game_time = GetGameTime(footer, f);
                    }
                }
            }

            // Step 3: Close ranges for entities that disappeared in this frame.
            for (const auto& uid : present_last_frame) {
                if (!present_this_frame.contains(uid)) {
                    has_open_range.erase(uid);
                }
            }

            present_last_frame = std::move(present_this_frame);
        }

        // Step 4: Build the final result set and surface empty-result diagnostics.
        lifetime_result_.entries.reserve(entity_map.size());
        for (auto& [uid, entry] : entity_map) {
            lifetime_result_.entries.push_back(std::move(entry));
        }

        std::sort(lifetime_result_.entries.begin(), lifetime_result_.entries.end(),
                  [](const EntityLifeTimeEntry& a, const EntityLifeTimeEntry& b) {
                      int32_t a_start = a.ranges.empty() ? 0 : a.ranges.front().start_frame;
                      int32_t b_start = b.ranges.empty() ? 0 : b.ranges.front().start_frame;
                      return a_start < b_start;
                  });

        lifetime_result_.is_complete = true;
        progress_.store(1.0f);
        if (frames_with_bucket == 0) {
            lifetime_result_.error_message =
                std::format("Selected bucket {} was not present in frames {}-{}.", BuildBucketDebugLabel(config),
                            config.start_frame, config.end_frame);
        } else if (lifetime_result_.entries.empty()) {
            lifetime_result_.error_message = "No entities matched the selected filters in the scanned range.";
        }
        SetStatus(std::format("Done. {} entities found.", lifetime_result_.entries.size()));
    }

    // Scans scalar property combinations and groups unique value sets by contributing entity IDs.
    void AnalysisScanner::RunUniqueProperties(const ScanConfig& config, VTX::IVtxReaderFacade* reader,
                                              const VTX::PropertyAddressCache& cache, const VTX::FileFooter& footer) {
        SetStatus("Scanning unique property combinations...");

        const int32_t total_range = config.end_frame - config.start_frame + 1;
        int32_t frames_with_bucket = 0;
        int32_t matched_entities = 0;
        int32_t entities_with_all_properties = 0;

        // Step 1: Collect unique (uid, prop_values) tuples across the requested frame range.
        std::unordered_set<std::string> unique_combinations;

        for (int32_t f = config.start_frame; f <= config.end_frame; ++f) {
            if (cancel_requested_.load()) {
                SetStatus("Cancelled.");
                unique_props_result_.error_message = "Cancelled by user.";
                return;
            }

            current_frame_.store(f);
            progress_.store(static_cast<float>(f - config.start_frame + 1) / static_cast<float>(total_range));
            SetStatus(std::format("Scanning unique property combinations... frame {} / {}", f, config.end_frame));

            const auto* frame = reader->GetFrameSync(f);
            if (!frame)
                continue;

            const auto* bucket = ResolveScanBucket(*frame, config);
            if (!bucket)
                continue;
            ++frames_with_bucket;

            for (size_t i = 0; i < bucket->entities.size(); ++i) {
                const std::string& uid = i < bucket->unique_ids.size() ? bucket->unique_ids[i] : "";
                if (!PassesUidFilter(config.filter_unique_ids, uid))
                    continue;
                ++matched_entities;

                const auto& entity = bucket->entities[i];
                auto sc_it = cache.structs.find(entity.entity_type_id);
                if (sc_it == cache.structs.end())
                    continue;

                std::string combined_key = uid;
                combined_key += '\0';

                bool has_all_props = true;
                for (size_t p = 0; p < config.property_names.size(); ++p) {
                    if (p > 0)
                        combined_key += '|';

                    auto prop_it = sc_it->second.properties.find(config.property_names[p]);
                    if (prop_it == sc_it->second.properties.end()) {
                        has_all_props = false;
                        break;
                    }
                    combined_key += ExtractScalarAsString(entity, prop_it->second);
                }

                if (has_all_props) {
                    ++entities_with_all_properties;
                    unique_combinations.insert(combined_key);
                }
            }
        }

        // Step 2: Group identical property sets and count distinct contributing entities.
        std::unordered_map<std::string, UniquePropertiesGroup> groups;

        for (const auto& combined_key : unique_combinations) {
            auto sep_pos = combined_key.find('\0');
            std::string uid = combined_key.substr(0, sep_pos);
            std::string props_key = combined_key.substr(sep_pos + 1);

            auto& group = groups[props_key];
            if (group.property_values.empty()) {
                size_t start = 0;
                for (size_t j = 0; j < config.property_names.size(); ++j) {
                    size_t pipe_pos = props_key.find('|', start);
                    if (pipe_pos == std::string::npos) {
                        group.property_values.push_back(props_key.substr(start));
                        start = props_key.size();
                    } else {
                        group.property_values.push_back(props_key.substr(start, pipe_pos - start));
                        start = pipe_pos + 1;
                    }
                }
            }
            group.contributing_uids.push_back(uid);
            group.count = static_cast<int32_t>(group.contributing_uids.size());
        }

        // Step 3: Finalize, sort, and explain empty outputs when nothing matched.
        unique_props_result_.property_names = config.property_names;
        unique_props_result_.groups.reserve(groups.size());
        for (auto& [key, group] : groups) {
            std::sort(group.contributing_uids.begin(), group.contributing_uids.end());
            unique_props_result_.groups.push_back(std::move(group));
        }

        std::sort(unique_props_result_.groups.begin(), unique_props_result_.groups.end(),
                  [](const UniquePropertiesGroup& a, const UniquePropertiesGroup& b) { return a.count > b.count; });

        unique_props_result_.is_complete = true;
        progress_.store(1.0f);
        if (frames_with_bucket == 0) {
            unique_props_result_.error_message =
                std::format("Selected bucket {} was not present in frames {}-{}.", BuildBucketDebugLabel(config),
                            config.start_frame, config.end_frame);
        } else if (matched_entities == 0) {
            unique_props_result_.error_message = "No entities matched the selected filters in the scanned range.";
        } else if (entities_with_all_properties == 0) {
            unique_props_result_.error_message =
                "None of the matched entities contained all requested scalar properties.";
        }
        SetStatus(std::format("Done. {} unique combinations found.", unique_props_result_.groups.size()));
    }

    // Tracks contiguous spans for each requested scalar property value per entity.
    void AnalysisScanner::RunTrackProperty(const ScanConfig& config, VTX::IVtxReaderFacade* reader,
                                           const VTX::PropertyAddressCache& cache, const VTX::FileFooter& footer) {
        SetStatus("Tracking property changes...");

        const int32_t total_range = config.end_frame - config.start_frame + 1;
        int32_t frames_with_bucket = 0;
        int32_t matched_entities = 0;
        int32_t tracked_property_hits = 0;

        // Step 1: Maintain one open span per entity/property pair while walking frames in order.
        struct OpenSpan {
            std::string value;
            FrameRange range;
        };
        std::unordered_map<std::string, OpenSpan> open_spans;

        auto make_span_key = [](const std::string& uid, const std::string& prop) {
            return uid + '\0' + prop;
        };

        for (int32_t f = config.start_frame; f <= config.end_frame; ++f) {
            if (cancel_requested_.load()) {
                SetStatus("Cancelled.");
                track_prop_result_.error_message = "Cancelled by user.";
                return;
            }

            current_frame_.store(f);
            progress_.store(static_cast<float>(f - config.start_frame + 1) / static_cast<float>(total_range));
            SetStatus(std::format("Tracking property changes... frame {} / {}", f, config.end_frame));

            const auto* frame = reader->GetFrameSync(f);
            if (!frame)
                continue;

            const auto* bucket = ResolveScanBucket(*frame, config);
            if (!bucket)
                continue;
            ++frames_with_bucket;

            // Step 2: Update active spans with the values present in this frame.
            std::unordered_set<std::string> active_keys;

            for (size_t i = 0; i < bucket->entities.size(); ++i) {
                const std::string& uid = i < bucket->unique_ids.size() ? bucket->unique_ids[i] : "";
                if (!PassesUidFilter(config.filter_unique_ids, uid))
                    continue;
                ++matched_entities;

                const auto& entity = bucket->entities[i];
                auto sc_it = cache.structs.find(entity.entity_type_id);
                if (sc_it == cache.structs.end())
                    continue;

                for (const auto& prop_name : config.property_names) {
                    auto prop_it = sc_it->second.properties.find(prop_name);
                    if (prop_it == sc_it->second.properties.end())
                        continue;
                    ++tracked_property_hits;

                    std::string value = ExtractScalarAsString(entity, prop_it->second);
                    std::string span_key = make_span_key(uid, prop_name);
                    active_keys.insert(span_key);

                    auto it = open_spans.find(span_key);
                    if (it == open_spans.end()) {
                        open_spans[span_key] = OpenSpan {
                            .value = value,
                            .range =
                                FrameRange {
                                    .start_frame = f,
                                    .end_frame = f,
                                    .start_game_time = GetGameTime(footer, f),
                                    .end_game_time = GetGameTime(footer, f),
                                },
                        };
                    } else if (it->second.value != value) {
                        auto& old_span = it->second;
                        track_prop_result_.spans.push_back(PropertyValueSpan {
                            .unique_id = uid,
                            .property_name = prop_name,
                            .value = old_span.value,
                            .range = old_span.range,
                        });

                        it->second = OpenSpan {
                            .value = value,
                            .range =
                                FrameRange {
                                    .start_frame = f,
                                    .end_frame = f,
                                    .start_game_time = GetGameTime(footer, f),
                                    .end_game_time = GetGameTime(footer, f),
                                },
                        };
                    } else {
                        it->second.range.end_frame = f;
                        it->second.range.end_game_time = GetGameTime(footer, f);
                    }
                }
            }

            // Step 3: Close spans that are no longer present in the current frame.
            for (auto it = open_spans.begin(); it != open_spans.end();) {
                if (!active_keys.contains(it->first)) {
                    auto sep_pos = it->first.find('\0');
                    std::string uid = it->first.substr(0, sep_pos);
                    std::string prop_name = it->first.substr(sep_pos + 1);

                    track_prop_result_.spans.push_back(PropertyValueSpan {
                        .unique_id = uid,
                        .property_name = prop_name,
                        .value = it->second.value,
                        .range = it->second.range,
                    });
                    it = open_spans.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Step 4: Flush the remaining spans and explain empty outputs clearly.
        for (auto& [span_key, open_span] : open_spans) {
            auto sep_pos = span_key.find('\0');
            std::string uid = span_key.substr(0, sep_pos);
            std::string prop_name = span_key.substr(sep_pos + 1);

            track_prop_result_.spans.push_back(PropertyValueSpan {
                .unique_id = uid,
                .property_name = prop_name,
                .value = open_span.value,
                .range = open_span.range,
            });
        }

        std::sort(track_prop_result_.spans.begin(), track_prop_result_.spans.end(),
                  [](const PropertyValueSpan& a, const PropertyValueSpan& b) {
                      if (a.unique_id != b.unique_id)
                          return a.unique_id < b.unique_id;
                      if (a.property_name != b.property_name)
                          return a.property_name < b.property_name;
                      return a.range.start_frame < b.range.start_frame;
                  });

        track_prop_result_.property_names = config.property_names;
        track_prop_result_.is_complete = true;
        progress_.store(1.0f);
        if (frames_with_bucket == 0) {
            track_prop_result_.error_message =
                std::format("Selected bucket {} was not present in frames {}-{}.", BuildBucketDebugLabel(config),
                            config.start_frame, config.end_frame);
        } else if (matched_entities == 0) {
            track_prop_result_.error_message = "No entities matched the selected filters in the scanned range.";
        } else if (tracked_property_hits == 0) {
            track_prop_result_.error_message = "None of the requested properties were found on the matched entities.";
        }
        SetStatus(std::format("Done. {} value spans recorded.", track_prop_result_.spans.size()));
    }

} // namespace VtxServices
