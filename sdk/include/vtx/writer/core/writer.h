#pragma once
#include <memory>
#include <vector>
#include <optional>
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"
#include "vtx/common/vtx_frame_accessor.h"
#include "vtx/writer/core/vtx_frame_mutation_view.h"
#include "vtx/writer/core/vtx_frame_post_processor.h"
#include "vtx/writer/core/vtx_writer_result.h"

#include <algorithm>
#include <string>


namespace VTX {
    namespace ChunkingPolicy {
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
    } // namespace ChunkingPolicy


    template <typename SinkPolicy, typename ChunkingPolicy = ChunkingPolicy::ThresholdChunkPolicy>
    class ReplayWriter {
    public:
        using Serializer = typename SinkPolicy::SerializerPolicy;
        using FrameType = typename SinkPolicy::FrameType;
        using SchemaType = typename SinkPolicy::SchemaType;

        struct Config {
            typename SinkPolicy::Config sink_config;
            float default_fps = 60.0f;
            bool is_increasing = true;
            ChunkingPolicy chunker_config;
            std::string schema_json_path;
            std::string schema_json_content;                      // in-memory schema JSON (wins over schema_json_path)
            std::shared_ptr<VTX::SchemaRegistry> schema_registry; // pre-built registry (wins over content/path)
            bool retain_finalized_snapshot = false;
        };

        ReplayWriter(Config config)
            : sink_(config.sink_config)
            , chunker_(config.chunker_config)
            , registry_({})
            , sanitizer_(nullptr) {
            retain_snapshot_ = config.retain_finalized_snapshot;
            timer_.Setup(config.default_fps, config.is_increasing);
            // Schema source precedence: injected registry > in-memory JSON content > file path.
            if (config.schema_registry) {
                registry_ = *config.schema_registry;
            } else if (!config.schema_json_content.empty()) {
                registry_.LoadFromRawString(config.schema_json_content);
            } else {
                registry_.LoadFromJson(config.schema_json_path);
            }
            auto schema = Serializer::CreateSchema(registry_);
            sink_.OnSessionStart(schema);
            frame_accessor_.InitializeFromCache(registry_.GetPropertyCache());
        }

        ~ReplayWriter() {
            if (post_processor_) {
                try {
                    post_processor_->Clear();
                } catch (...) {}
            }
        }

        RecordResult TryRecordFrame(VTX::Frame& native_frame,
                                    const VTX::GameTime::GameTimeRegister& game_time_register) {
            timer_.CreateSnapshot();

            if (!timer_.AddTimeRegistry(game_time_register)) {
                timer_.Rollback();
                return RecordResult::MadeRejected(VtxErrorCode::GameTimeRejected,
                                                  "game-time registry rejected by the timer");
            }

            const int32_t prospective_index = total_frames_ + 1;
            if (!timer_.ResolveGameTimes(prospective_index)) {
                timer_.Rollback();
                return RecordResult::MadeRejected(VtxErrorCode::GameTimeRejected,
                                                  "game-time could not be resolved (non-monotonic / invalid)");
            }

            if (post_processor_) {
                FrameMutationView view(native_frame, frame_accessor_);
                FramePostProcessContext ctx;
                ctx.global_frame_index = total_frames_;
                ctx.chunk_local_frame_index = static_cast<int32_t>(pending_frames_.size());
                ctx.chunk_index = chunks_flushed_;
                ctx.schema_version = 0;
                ctx.frame_accessor = &frame_accessor_;
                ctx.previous_frame = nullptr;
                try {
                    post_processor_->Process(view, ctx);
                } catch (const std::exception& e) {
                    (void)e;
                } catch (...) {}
                view.Freeze();
            }

            std::string validation_detail;
            if (!FinalizeFrame(native_frame, validation_detail)) {
                timer_.Rollback();
                return RecordResult::MadeRejected(VtxErrorCode::EntityTypeUnresolved, std::move(validation_detail));
            }

            if (retain_snapshot_) {
                last_finalized_ = native_frame;
                has_last_finalized_ = true;
            }

            std::unique_ptr<FrameType> sink_frame = Serializer::FromNative(std::move(native_frame));
            size_t frame_size = Serializer::GetFrameSize(*sink_frame);

            if (!pending_frames_.empty() &&
                chunker_.ShouldFlush(pending_frames_.size(), current_chunk_bytes_, frame_size)) {
                Flush();
            }

            const int32_t assigned_index = total_frames_;
            total_frames_++;
            current_chunk_bytes_ += frame_size;
            pending_frames_.push_back(std::move(sink_frame));
            return RecordResult::MadeWritten(assigned_index);
        }

        void RecordFrame(VTX::Frame& native_frame, const VTX::GameTime::GameTimeRegister& game_time_register) {
            (void)TryRecordFrame(native_frame, game_time_register);
        }

        void Flush() {
            if (pending_frames_.empty())
                return;

            int32_t start_frame = total_frames_ - static_cast<int32_t>(pending_frames_.size());
            auto time_chunk = timer_.GetLastChunkCreatedUtc();
            sink_.SaveChunk(pending_frames_, time_chunk, start_frame, total_frames_);

            pending_frames_.clear();
            current_chunk_bytes_ = 0;
            ++chunks_flushed_;
            timer_.UpdateChunkStartIndex();
        }

        void Stop() {
            Flush();
            VTX::SessionFooter footer_data;
            footer_data.total_frames = total_frames_;
            footer_data.duration_seconds = timer_.GetDuration();

            const auto& v_gametime = timer_.GetGameTime();
            const auto& v_utc = timer_.GetCreatedUtc();
            const auto& v_gaps = timer_.GetTimelineGaps();
            const auto& v_seg = timer_.GetGameSegments();

            footer_data.game_times = &v_gametime;
            footer_data.created_utc = &v_utc;
            footer_data.gaps = &v_gaps;
            footer_data.segments = &v_seg;

            sink_.Close(footer_data);
        }

        VTX::SchemaRegistry& GetRegistry() { return registry_; }

        const VTX::Frame* GetLastFinalizedFrame() const { return has_last_finalized_ ? &last_finalized_ : nullptr; }

        const VTX::PropertyContainer* FindEntity(const std::string& bucket_name, const std::string& unique_id) const {
            if (!has_last_finalized_) {
                return nullptr;
            }
            const VTX::Bucket& bucket = last_finalized_.GetBucket(bucket_name);
            const size_t count = std::min(bucket.unique_ids.size(), bucket.entities.size());
            for (size_t i = 0; i < count; ++i) {
                if (bucket.unique_ids[i] == unique_id) {
                    return &bucket.entities[i];
                }
            }
            return nullptr;
        }

        void SetPostProcessor(std::shared_ptr<IFramePostProcessor> processor) {
            if (processor) {
                FramePostProcessorInitContext init_ctx;
                init_ctx.frame_accessor = &frame_accessor_;
                init_ctx.total_frames = 0;
                init_ctx.schema_version = 0;
                init_ctx.format_major = 0;
                init_ctx.format_minor = 0;
                try {
                    processor->Init(init_ctx);
                } catch (...) {
                    throw;
                }
            }
            post_processor_ = std::move(processor);
        }

        std::shared_ptr<IFramePostProcessor> GetPostProcessor() const { return post_processor_; }

        void ClearPostProcessor() {
            auto outgoing = std::move(post_processor_);
            post_processor_.reset();
            if (outgoing) {
                try {
                    outgoing->Clear();
                } catch (...) {}
            }
        }

    private:
        bool FinalizeFrame(VTX::Frame& frame, std::string& out_detail) {
            for (auto& bucket : frame.GetMutableBuckets()) {
                for (auto& entity : bucket.entities) {
                    if (entity.entity_type_id < 0) {
                        out_detail = "frame contains an entity whose type does not resolve to a schema struct";
                        return false;
                    }
                    entity.content_hash = Helpers::CalculateContainerHash(entity);
                }
            }
            return true;
        }

        SinkPolicy sink_;
        ChunkingPolicy chunker_;

        VTX::SchemaRegistry registry_;
        const SchemaSanitizerRegistry* sanitizer_;
        std::vector<std::unique_ptr<FrameType>> pending_frames_;
        VTX::GameTime::VTXGameTimes timer_;
        size_t current_chunk_bytes_ = 0;
        int32_t total_frames_ = 0;
        int32_t chunks_flushed_ = 0;

        FrameAccessor frame_accessor_ = {};
        std::shared_ptr<IFramePostProcessor> post_processor_;

        bool retain_snapshot_ = false;
        VTX::Frame last_finalized_;
        bool has_last_finalized_ = false;
    };
} // namespace VTX
