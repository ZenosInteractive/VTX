#pragma once
#include "commands/command_registry.h"
#include "commands/command_helpers.h"
#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "format/vtx_type_serializer.h"

namespace VtxCli {

    template <FormatWriter Fmt>
    struct InfoCommand {
        static constexpr std::string_view Name = "info";
        static constexpr std::string_view Help = "info - Show summary of the loaded replay";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer) {
            if (!RequireLoaded(context, writer, Name))
                return;

            const auto& header = context.session.GetHeader();
            const auto& footer = context.session.GetFooter();

            ResponseOk(writer, Name)
                .Key("file")
                .WriteString(context.session.GetFilePath())
                .Key("format")
                .WriteString(context.session.GetFormat() == VTX::VtxFormat::FlatBuffers ? "flatbuffers" : "protobuf")
                .Key("file_size_mb")
                .WriteFloat(context.session.GetFileSizeMb())
                .Key("replay_name")
                .WriteString(header.replay_name)
                .Key("replay_uuid")
                .WriteString(header.replay_uuid);
            WriteUtcTicks(writer, "recorded_utc", header.recorded_utc_timestamp)
                .Key("total_frames")
                .WriteInt(context.session.GetTotalFrames())
                .Key("duration_seconds")
                .WriteFloat(footer.duration_seconds)
                .Key("current_frame")
                .WriteInt(context.session.GetCurrentFrame())
                .Key("chunk_count")
                .WriteInt(static_cast<int32_t>(footer.chunk_index.size()))
                .Key("event_count")
                .WriteInt(static_cast<int32_t>(footer.events.size()));
            EndResponse(writer);
        }
    };

    template <FormatWriter Fmt>
    struct HeaderCommand {
        static constexpr std::string_view Name = "header";
        static constexpr std::string_view Help = "header - Show file header details";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer) {
            if (!RequireLoaded(context, writer, Name))
                return;

            const auto& header = context.session.GetHeader();
            const auto& version = header.version;

            ResponseOk(writer, Name);

            // version info
            writer.Key("version");
            writer.BeginObject()
                .Key("format_major")
                .WriteUInt(version.format_major)
                .Key("format_minor")
                .WriteUInt(version.format_minor)
                .Key("schema_version")
                .WriteUInt(version.schema_version)
                .EndObject();

            writer.Key("replay_name")
                .WriteString(header.replay_name)
                .Key("replay_uuid")
                .WriteString(header.replay_uuid)
                .Key("recorded_utc_timestamp")
                .WriteInt64(header.recorded_utc_timestamp);
            // The raw field keeps whatever unit the writer stamped (historically
            // unix seconds); the ISO string is derived from the normalized value.
            writer.Key("recorded_utc_iso");
            if (header.recorded_utc_timestamp != 0) {
                writer.WriteString(VTX::TimeUtils::FormatUtcTicksIso8601(
                    VTX::TimeUtils::NormalizeUtcToUeTicks(header.recorded_utc_timestamp)));
            } else {
                writer.WriteNull();
            }

            if (!header.custom_json_metadata.empty()) {
                writer.Key("custom_metadata").WriteRaw(header.custom_json_metadata);
            } else {
                writer.Key("custom_metadata").WriteNull();
            }

            EndResponse(writer);
        }
    };

    template <FormatWriter Fmt>
    struct FooterCommand {
        static constexpr std::string_view Name = "footer";
        static constexpr std::string_view Help = "footer - Show file footer details";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer) {
            if (!RequireLoaded(context, writer, Name))
                return;

            const auto& footer = context.session.GetFooter();

            ResponseOk(writer, Name)
                .Key("total_frames")
                .WriteInt(footer.total_frames)
                .Key("duration_seconds")
                .WriteFloat(footer.duration_seconds)
                .Key("chunk_count")
                .WriteInt(static_cast<int32_t>(footer.chunk_index.size()))
                .Key("event_count")
                .WriteInt(static_cast<int32_t>(footer.events.size()))
                .Key("game_time_count")
                .WriteInt(static_cast<int32_t>(footer.times.game_time.size()))
                .Key("created_utc_count")
                .WriteInt(static_cast<int32_t>(footer.times.created_utc.size()))
                .Key("gap_count")
                .WriteInt(static_cast<int32_t>(footer.times.gaps.size()))
                .Key("segment_count")
                .WriteInt(static_cast<int32_t>(footer.times.segments.size()))
                .Key("payload_checksum")
                .WriteUInt64(footer.payload_checksum);
            EndResponse(writer);
        }
    };

    // schema, raw json
    template <FormatWriter Fmt>
    struct SchemaCommand {
        static constexpr std::string_view Name = "schema";
        static constexpr std::string_view Help = "schema - Show the contextual schema";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer) {
            if (!RequireLoaded(context, writer, Name))
                return;

            const auto& schema = context.session.GetContextualSchema();

            ResponseOk(writer, Name)
                .Key("data_identifier")
                .WriteString(schema.data_identifier)
                .Key("data_version")
                .WriteInt(schema.data_version)
                .Key("data_version_string")
                .WriteString(schema.data_version_string);

            // property_mapping is already JSON ,pass through raw
            if (!schema.property_mapping.empty()) {
                writer.Key("property_mapping").WriteRaw(schema.property_mapping);
            } else {
                writer.Key("property_mapping").WriteNull();
            }

            EndResponse(writer);
        }
    };

    template <FormatWriter Fmt>
    struct ChunksCommand {
        static constexpr std::string_view Name = "chunks";
        static constexpr std::string_view Help = "chunks - Show chunk seek table";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer) {
            if (!RequireLoaded(context, writer, Name))
                return;

            const auto& footer = context.session.GetFooter();

            ResponseOk(writer, Name).Key("count").WriteInt(static_cast<int32_t>(footer.chunk_index.size()));

            writer.Key("chunks");
            writer.BeginArray();
            for (const auto& chunk : footer.chunk_index) {
                writer.BeginObject()
                    .Key("chunk_index")
                    .WriteInt(chunk.chunk_index)
                    .Key("start_frame")
                    .WriteInt(chunk.start_frame)
                    .Key("end_frame")
                    .WriteInt(chunk.end_frame)
                    .Key("file_offset")
                    .WriteUInt64(chunk.file_offset)
                    .Key("chunk_size_bytes")
                    .WriteUInt(chunk.chunk_size_bytes)
                    .Key("checksum")
                    .WriteUInt64(chunk.checksum)
                    .EndObject();
            }
            writer.EndArray();
            EndResponse(writer);
        }
    };

    template <FormatWriter Fmt>
    struct EventsCommand {
        static constexpr std::string_view Name = "events";
        static constexpr std::string_view Help = "events - Show timeline events";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer) {
            if (!RequireLoaded(context, writer, Name))
                return;

            const auto& footer = context.session.GetFooter();
            const int64_t recorded_utc =
                VTX::TimeUtils::NormalizeUtcToUeTicks(context.session.GetHeader().recorded_utc_timestamp);

            ResponseOk(writer, Name).Key("count").WriteInt(static_cast<int32_t>(footer.events.size()));

            writer.Key("events");
            writer.BeginArray();
            for (const auto& evt : footer.events) {
                writer.BeginObject()
                    .Key("game_time")
                    .WriteFloat(evt.game_time)
                    .Key("event_type")
                    .WriteString(evt.event_type)
                    .Key("label")
                    .WriteString(evt.label)
                    .Key("entity_unique_id")
                    .WriteString(evt.entity_unique_id);
                // Wall-clock derived from recording start + game_time offset;
                // null when the file did not record a start timestamp.
                const int64_t evt_utc =
                    recorded_utc != 0
                        ? recorded_utc + static_cast<int64_t>(static_cast<double>(evt.game_time) *
                                                              VTX::TimeUtils::TICKS_PER_SECOND)
                        : 0;
                WriteUtcTicks(writer, "utc", evt_utc);
                writer.Key("location");
                Serialize(writer, evt.location);
                writer.EndObject();
            }
            writer.EndArray();
            EndResponse(writer);
        }
    };

    /// times [start] [end] — dump the footer per-frame time table.
    ///
    /// Without arguments returns a summary: counts, first/last wall-clock,
    /// durations, the typical frame step, and a discontinuity scan (frames whose
    /// wall-clock delta is negative or more than twice the median step).
    /// With a frame range it additionally returns the per-frame slice.
    template <FormatWriter Fmt>
    struct TimesCommand {
        static constexpr std::string_view Name = "times";
        static constexpr std::string_view Help = "times [start] [end] - Show per-frame time table and anomalies";

        static constexpr size_t kMaxReportedAnomalies = 100;

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer) {
            if (!RequireLoaded(context, writer, Name))
                return;

            const auto footer = context.session.GetFooter();
            const auto& times = footer.times;
            const size_t table_size = std::max(times.game_time.size(), times.created_utc.size());

            // Optional [start] [end] slice range
            bool have_range = false;
            int64_t start = 0, end = 0;
            if (!args.empty()) {
                if (args.size() != 2) {
                    ResponseError(writer, Name, "Usage: times [start_frame end_frame]");
                    return;
                }
                try {
                    start = std::stoll(std::string(args[0]));
                    end = std::stoll(std::string(args[1]));
                } catch (...) {
                    ResponseError(writer, Name, "Invalid frame range: " + std::string(args[0]) + " " +
                                                    std::string(args[1]));
                    return;
                }
                if (start < 0 || end < start || static_cast<size_t>(end) >= table_size) {
                    ResponseError(writer, Name,
                                  "Frame range out of bounds (time table has " + std::to_string(table_size) +
                                      " entries)");
                    return;
                }
                have_range = true;
            }

            ResponseOk(writer, Name)
                .Key("total_frames")
                .WriteInt(footer.total_frames)
                .Key("game_time_count")
                .WriteInt(static_cast<int32_t>(times.game_time.size()))
                .Key("created_utc_count")
                .WriteInt(static_cast<int32_t>(times.created_utc.size()))
                .Key("gap_count")
                .WriteInt(static_cast<int32_t>(times.gaps.size()))
                .Key("segment_count")
                .WriteInt(static_cast<int32_t>(times.segments.size()));

            // Wall-clock span
            const int64_t first_utc = times.created_utc.empty() ? 0 : static_cast<int64_t>(times.created_utc.front());
            const int64_t last_utc = times.created_utc.empty() ? 0 : static_cast<int64_t>(times.created_utc.back());
            WriteUtcTicks(writer, "first_created_utc", first_utc);
            WriteUtcTicks(writer, "last_created_utc", last_utc);
            writer.Key("wall_duration_seconds");
            if (first_utc != 0 && last_utc != 0) {
                writer.WriteDouble(VTX::TimeUtils::TicksToSeconds(last_utc - first_utc));
            } else {
                writer.WriteNull();
            }

            // Game-time span
            writer.Key("game_duration_seconds");
            if (!times.game_time.empty()) {
                const auto span = static_cast<int64_t>(times.game_time.back()) -
                                  static_cast<int64_t>(times.game_time.front());
                writer.WriteDouble(VTX::TimeUtils::TicksToSeconds(span));
            } else {
                writer.WriteNull();
            }

            // Median wall-clock step + discontinuity scan
            std::vector<int64_t> deltas;
            if (times.created_utc.size() > 1) {
                deltas.reserve(times.created_utc.size() - 1);
                for (size_t i = 1; i < times.created_utc.size(); ++i) {
                    deltas.push_back(static_cast<int64_t>(times.created_utc[i]) -
                                     static_cast<int64_t>(times.created_utc[i - 1]));
                }
            }

            int64_t median_delta = 0;
            if (!deltas.empty()) {
                std::vector<int64_t> sorted = deltas;
                const size_t mid = sorted.size() / 2;
                std::nth_element(sorted.begin(), sorted.begin() + mid, sorted.end());
                median_delta = sorted[mid];
            }

            writer.Key("median_created_utc_delta_ticks");
            if (!deltas.empty()) {
                writer.WriteInt64(median_delta)
                    .Key("median_created_utc_delta_seconds")
                    .WriteDouble(VTX::TimeUtils::TicksToSeconds(median_delta));
            } else {
                writer.WriteNull().Key("median_created_utc_delta_seconds").WriteNull();
            }

            size_t anomaly_count = 0;
            writer.Key("anomalies");
            writer.BeginArray();
            for (size_t i = 0; i < deltas.size(); ++i) {
                const int64_t d = deltas[i];
                const bool anomalous = d < 0 || (median_delta > 0 && d > 2 * median_delta);
                if (!anomalous)
                    continue;
                ++anomaly_count;
                if (anomaly_count > kMaxReportedAnomalies)
                    continue;
                writer.BeginObject()
                    .Key("frame")
                    .WriteInt(static_cast<int32_t>(i + 1))
                    .Key("delta_ticks")
                    .WriteInt64(d)
                    .Key("delta_seconds")
                    .WriteDouble(VTX::TimeUtils::TicksToSeconds(d))
                    .EndObject();
            }
            writer.EndArray();
            writer.Key("anomaly_count").WriteInt(static_cast<int32_t>(anomaly_count));
            writer.Key("anomalies_truncated").WriteBool(anomaly_count > kMaxReportedAnomalies);

            // Raw gap/segment tables (small)
            writer.Key("gaps");
            writer.BeginArray();
            for (const auto gap : times.gaps) {
                writer.WriteUInt(gap);
            }
            writer.EndArray();
            writer.Key("segments");
            writer.BeginArray();
            for (const auto segment : times.segments) {
                writer.WriteUInt(segment);
            }
            writer.EndArray();

            // Optional per-frame slice
            if (have_range) {
                writer.Key("start_frame").WriteInt(static_cast<int32_t>(start));
                writer.Key("end_frame").WriteInt(static_cast<int32_t>(end));
                writer.Key("frames");
                writer.BeginArray();
                for (int64_t f = start; f <= end; ++f) {
                    const auto idx = static_cast<size_t>(f);
                    writer.BeginObject().Key("frame").WriteInt(static_cast<int32_t>(f));

                    writer.Key("game_time_ticks");
                    if (idx < times.game_time.size()) {
                        writer.WriteInt64(static_cast<int64_t>(times.game_time[idx]));
                    } else {
                        writer.WriteNull();
                    }

                    const int64_t utc =
                        idx < times.created_utc.size() ? static_cast<int64_t>(times.created_utc[idx]) : 0;
                    WriteUtcTicks(writer, "created_utc", utc);

                    writer.Key("delta_ticks");
                    if (idx > 0 && idx < times.created_utc.size()) {
                        writer.WriteInt64(static_cast<int64_t>(times.created_utc[idx]) -
                                          static_cast<int64_t>(times.created_utc[idx - 1]));
                    } else {
                        writer.WriteNull();
                    }

                    writer.EndObject();
                }
                writer.EndArray();
            }

            EndResponse(writer);
        }
    };


} // namespace VtxCli
