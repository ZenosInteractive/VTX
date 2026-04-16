#pragma once
#include <memory>
#include <vector>
#include <optional>
#include "vtx/common/vtx_types.h"


namespace VTX {
    namespace ChunkingPolicy
    {
        struct ThresholdChunkPolicy {
            int32_t max_frames = 1000;
            size_t max_bytes = 10 * 1024 * 1024;
            bool ShouldFlush(size_t current_count, size_t current_bytes, size_t next_frame_size) const {
                return (current_count >= (size_t)max_frames) || (current_bytes + next_frame_size >= max_bytes);
            }
        };

        struct InstantFlushPolicy {
            bool ShouldFlush(size_t, size_t, size_t) const { return true; }
        };
    }


    template <typename SinkPolicy, typename ChunkingPolicy = ChunkingPolicy::ThresholdChunkPolicy>
    class ReplayWriter {
    public:
        using Serializer = typename SinkPolicy::SerializerPolicy;
        using FrameType  = typename SinkPolicy::FrameType;
        using SchemaType = typename SinkPolicy::SchemaType;

        struct Config {
            typename SinkPolicy::Config sink_config; 
            float default_fps = 60.0f;
            bool is_increasing = true;
            ChunkingPolicy chunker_config;
            std::string schema_json_path;
        };

        ReplayWriter(Config config)
            : sink_(config.sink_config),
              chunker_(config.chunker_config), registry_({}), sanitizer_(nullptr)
        {
            timer_.Setup(config.default_fps, config.is_increasing);
            registry_.LoadFromJson(config.schema_json_path);
            auto schema = Serializer::CreateSchema(registry_);
            sink_.OnSessionStart(schema);
        }

        void RecordFrame(VTX::Frame& native_frame, const VTX::GameTime::GameTimeRegister& game_time_register) 
        {
            timer_.CreateSnapshot();
            
            if (!timer_.AddTimeRegistry(game_time_register)) {
                timer_.Rollback();
                return;
            }

            int32_t prospectiveIndex = total_frames_ + 1;
            if (!timer_.ResolveGameTimes(prospectiveIndex)) {
                timer_.Rollback();
                return;
            }
            
            std::unique_ptr<FrameType> sink_frame = Serializer::FromNative(std::move(native_frame));
            size_t frameSize = Serializer::GetFrameSize(*sink_frame);
    
            if (!pending_frames_.empty() && 
                chunker_.ShouldFlush(pending_frames_.size(), current_chunk_bytes_, frameSize)) 
            {
                Flush();
            }

            total_frames_++;
            current_chunk_bytes_ += frameSize;
            pending_frames_.push_back(std::move(sink_frame));
        }

        void Flush() {
            if (pending_frames_.empty()) return;

            int32_t start_frame = total_frames_ - static_cast<int32_t>(pending_frames_.size());
            auto time_chunk = timer_.GetLastChunkCreatedUtc(); 
            sink_.SaveChunk(pending_frames_, time_chunk, start_frame, total_frames_);
            
            pending_frames_.clear(); 
            current_chunk_bytes_ = 0;
            timer_.UpdateChunkStartIndex();
        }

        void Stop() {
            Flush();
            VTX::SessionFooter footer_data;
            footer_data.total_frames = total_frames_;
            footer_data.duration_seconds = timer_.GetDuration();
            
            const auto& v_gametime = timer_.GetGameTime();
            const auto& v_utc      = timer_.GetCreatedUtc();
            const auto& v_gaps     = timer_.GetTimelineGaps();
            const auto& v_seg      = timer_.GetGameSegments();

            footer_data.game_times  = &v_gametime;
            footer_data.created_utc = &v_utc;
            footer_data.gaps        = &v_gaps;
            footer_data.segments    = &v_seg;
            
            sink_.Close(footer_data);
        }

        VTX::SchemaRegistry& GetRegistry()
        {
            return registry_;
        }
    private:
        SinkPolicy sink_;
        ChunkingPolicy chunker_;
        
        VTX::SchemaRegistry registry_;
        const SchemaSanitizerRegistry* sanitizer_;
        std::vector<std::unique_ptr<FrameType>> pending_frames_;
        VTX::GameTime::VTXGameTimes timer_; 
        size_t current_chunk_bytes_ = 0;
        int32_t total_frames_ = 0;
    };
}