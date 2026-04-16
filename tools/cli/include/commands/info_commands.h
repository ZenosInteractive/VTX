#pragma once
#include "commands/command_registry.h"
#include "commands/command_helpers.h"
#include <span>
#include <string>

#include "format/vtx_type_serializer.h"

namespace VtxCli {

    template<FormatWriter Fmt>
    struct InfoCommand {
        static constexpr std::string_view Name = "info";
        static constexpr std::string_view Help = "info - Show summary of the loaded replay";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer)
        {
            if (!RequireLoaded(context, writer, Name)) return;

            const auto& header = context.session.GetHeader();
            const auto& footer = context.session.GetFooter();

            ResponseOk(writer, Name)
             .Key("file").WriteString(context.session.GetFilePath())
             .Key("format").WriteString(context.session.GetFormat() == VTX::VtxFormat::FlatBuffers ? "flatbuffers" : "protobuf")
             .Key("file_size_mb").WriteFloat(context.session.GetFileSizeMb())
             .Key("replay_name").WriteString(header.replay_name)
             .Key("replay_uuid").WriteString(header.replay_uuid)
             .Key("total_frames").WriteInt(context.session.GetTotalFrames())
             .Key("duration_seconds").WriteFloat(footer.duration_seconds)
             .Key("current_frame").WriteInt(context.session.GetCurrentFrame())
             .Key("chunk_count").WriteInt(static_cast<int32_t>(footer.chunk_index.size()))
             .Key("event_count").WriteInt(static_cast<int32_t>(footer.events.size()));
            EndResponse(writer);
        }
    };

    template<FormatWriter Fmt>
    struct HeaderCommand
    {
        static constexpr std::string_view Name = "header";
        static constexpr std::string_view Help = "header - Show file header details";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer)
        {
            if (!RequireLoaded(context, writer, Name)) return;

            const auto& header = context.session.GetHeader();
            const auto& version = header.version;

            ResponseOk(writer, Name);

            // version info
            writer.Key("version");
            writer.BeginObject()
                .Key("format_major").WriteUInt(version.format_major)
                .Key("format_minor").WriteUInt(version.format_minor)
                .Key("schema_version").WriteUInt(version.schema_version)
            .EndObject();

            writer.Key("replay_name").WriteString(header.replay_name)
                .Key("replay_uuid").WriteString(header.replay_uuid)
                .Key("recorded_utc_timestamp").WriteInt64(header.recorded_utc_timestamp);

            if (!header.custom_json_metadata.empty()) {
                writer.Key("custom_metadata").WriteRaw(header.custom_json_metadata);
            } else {
                writer.Key("custom_metadata").WriteNull();
            }

            EndResponse(writer);
        }
    };

    template<FormatWriter Fmt>
    struct FooterCommand {
        static constexpr std::string_view Name = "footer";
        static constexpr std::string_view Help = "footer - Show file footer details";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer)
        {
            if (!RequireLoaded(context, writer, Name)) return;

            const auto& footer = context.session.GetFooter();

            ResponseOk(writer, Name)
             .Key("total_frames").WriteInt(footer.total_frames)
             .Key("duration_seconds").WriteFloat(footer.duration_seconds)
             .Key("chunk_count").WriteInt(static_cast<int32_t>(footer.chunk_index.size()))
             .Key("event_count").WriteInt(static_cast<int32_t>(footer.events.size()))
             .Key("payload_checksum").WriteUInt64(footer.payload_checksum);
            EndResponse(writer);
        }
    };

    // schema, raw json
    template<FormatWriter Fmt>
    struct SchemaCommand {
        static constexpr std::string_view Name = "schema";
        static constexpr std::string_view Help = "schema - Show the contextual schema";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer)
        {
            if (!RequireLoaded(context, writer, Name)) return;

            const auto& schema = context.session.GetContextualSchema();

            ResponseOk(writer, Name)
             .Key("data_identifier").WriteString(schema.data_identifier)
             .Key("data_version").WriteInt(schema.data_version)
             .Key("data_version_string").WriteString(schema.data_version_string);

            // property_mapping is already JSON ,pass through raw
            if (!schema.property_mapping.empty()) {
                writer.Key("property_mapping").WriteRaw(schema.property_mapping);
            } else {
                writer.Key("property_mapping").WriteNull();
            }

            EndResponse(writer);
        }
    };

    template<FormatWriter Fmt>
    struct ChunksCommand {
        static constexpr std::string_view Name = "chunks";
        static constexpr std::string_view Help = "chunks - Show chunk seek table";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer)
        {
            if (!RequireLoaded(context, writer, Name)) return;

            const auto& footer = context.session.GetFooter();

            ResponseOk(writer, Name)
             .Key("count").WriteInt(static_cast<int32_t>(footer.chunk_index.size()));

            writer.Key("chunks");
            writer.BeginArray();
            for (const auto& chunk : footer.chunk_index) {
                writer.BeginObject()
                 .Key("chunk_index").WriteInt(chunk.chunk_index)
                 .Key("start_frame").WriteInt(chunk.start_frame)
                 .Key("end_frame").WriteInt(chunk.end_frame)
                 .Key("file_offset").WriteUInt64(chunk.file_offset)
                 .Key("chunk_size_bytes").WriteUInt(chunk.chunk_size_bytes)
                 .EndObject();
            }
            writer.EndArray();
            EndResponse(writer);
        }
    };

    template<FormatWriter Fmt>
    struct EventsCommand {
        static constexpr std::string_view Name = "events";
        static constexpr std::string_view Help = "events - Show timeline events";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer)
        {
            if (!RequireLoaded(context, writer, Name)) return;

            const auto& footer = context.session.GetFooter();

            ResponseOk(writer, Name)
                .Key("count").WriteInt(static_cast<int32_t>(footer.events.size()));

                writer.Key("events");
                writer.BeginArray();
                for (const auto& evt : footer.events) {
                    writer.BeginObject()
                     .Key("game_time").WriteFloat(evt.game_time)
                     .Key("event_type").WriteString(evt.event_type)
                     .Key("label").WriteString(evt.label)
                     .Key("entity_unique_id").WriteString(evt.entity_unique_id);
                    writer.Key("location");
                    Serialize(writer, evt.location);
                    writer.EndObject();
                }
                writer.EndArray();
            EndResponse(writer);
        }
    };


} // namespace VtxCli
