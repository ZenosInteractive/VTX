#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "vtx/common/vtx_frame_accessor.h"
#include "vtx/writer/core/vtx_frame_mutation_view.h"

namespace VTX {

    struct FramePostProcessorInitContext {
        const FrameAccessor* frame_accessor = nullptr;
        int32_t total_frames = 0;
        uint32_t schema_version = 0;
        uint32_t format_major = 0;
        uint32_t format_minor = 0;
    };

    struct FramePostProcessContext {
        int32_t global_frame_index = 0;
        int32_t chunk_local_frame_index = 0;
        int32_t chunk_index = 0;
        uint32_t schema_version = 0;

        const FrameAccessor* frame_accessor = nullptr;
        const Frame* previous_frame = nullptr;
    };

    class IFramePostProcessor {
    public:
        virtual ~IFramePostProcessor() = default;

        virtual void Init(const FramePostProcessorInitContext& context) {}
        virtual void Clear() {}
        virtual void PrintInfo() const {}

        virtual void Process(FrameMutationView& view, const FramePostProcessContext& ctx) = 0;
    };

    class FramePostProcessorChain final : public IFramePostProcessor {
    public:
        void Add(std::shared_ptr<IFramePostProcessor> p) {
            if (p)
                processors_.push_back(std::move(p));
        }

        bool Remove(const std::shared_ptr<IFramePostProcessor>& p) {
            for (auto it = processors_.begin(); it != processors_.end(); ++it) {
                if (*it == p) {
                    processors_.erase(it);
                    return true;
                }
            }
            return false;
        }

        size_t size() const noexcept { return processors_.size(); }
        bool empty() const noexcept { return processors_.empty(); }

        void Init(const FramePostProcessorInitContext& ctx) override {
            for (auto& p : processors_) {
                if (p)
                    p->Init(ctx);
            }
        }

        void Process(FrameMutationView& view, const FramePostProcessContext& ctx) override {
            for (auto& p : processors_) {
                if (p)
                    p->Process(view, ctx);
            }
        }

        void PrintInfo() const override {
            for (const auto& p : processors_) {
                if (p)
                    p->PrintInfo();
            }
        }

        void Clear() override {
            for (auto it = processors_.rbegin(); it != processors_.rend(); ++it) {
                if (*it)
                    (*it)->Clear();
            }
        }

        void ClearChain() noexcept { processors_.clear(); }

        void ClearMembers() {
            Clear();
            ClearChain();
        }

    private:
        std::vector<std::shared_ptr<IFramePostProcessor>> processors_;
    };

} // namespace VTX
