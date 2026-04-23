#pragma once
#include "commands/command_registry.h"
#include "commands/command_helpers.h"
#include <span>
#include <string>

namespace VtxCli {

    namespace detail {
        /// Write bucket_count and entity_count for a given frame.
        /// Always writes both keys (0 if frame is null).
        template <FormatWriter Fmt>
        void WriteFrameStats(Fmt& w, const VTX::Frame* frame) {
            if (frame) {
                const auto& buckets = frame->GetBuckets();
                w.Key("bucket_count").WriteInt(static_cast<int32_t>(buckets.size()));
                int32_t total_entities = 0;
                for (const auto& bucket : buckets) {
                    total_entities += static_cast<int32_t>(bucket.entities.size());
                }
                w.Key("entity_count").WriteInt(total_entities);
            } else {
                w.Key("bucket_count").WriteInt(0);
                w.Key("entity_count").WriteInt(0);
            }
        }
    } // namespace detail

    template <FormatWriter Fmt>
    struct FrameCommand {
        static constexpr std::string_view Name = "frame";
        static constexpr std::string_view Help = "frame [n] - Query or navigate to frame n";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& w) {
            if (!RequireLoaded(context, w, Name))
                return;

            // No args: query current frame
            if (args.empty()) {
                const auto* frame = context.session.GetCurrentFrameData();

                ResponseOk(w, Name)
                    .Key("current_frame")
                    .WriteInt(context.session.GetCurrentFrame())
                    .Key("total_frames")
                    .WriteInt(context.session.GetTotalFrames());
                detail::WriteFrameStats(w, frame);
                EndResponse(w);
                return;
            }

            // Parse frame number
            int32_t frame_number;
            try {
                frame_number = std::stoi(std::string(args[0]));
            } catch (...) {
                ResponseError(w, Name, "Invalid frame number: " + std::string(args[0]));
                return;
            }

            if (!context.session.SetFrame(frame_number)) {
                ResponseError(w, Name, context.session.GetLastError());
                return;
            }

            const auto* frame = context.session.GetCurrentFrameData();

            ResponseOk(w, Name)
                .Key("current_frame")
                .WriteInt(context.session.GetCurrentFrame())
                .Key("total_frames")
                .WriteInt(context.session.GetTotalFrames());
            detail::WriteFrameStats(w, frame);
            EndResponse(w);
        }
    };

} // namespace VtxCli
