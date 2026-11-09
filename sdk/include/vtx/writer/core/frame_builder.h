#pragma once
// FrameBuilder: one VTX::Frame a writer refills every frame, in place.
//
// The frame's buckets are the schema's, in the schema's order (the layout ReplayWriter records), so recording
// it moves nothing. Each frame: Begin, then Add one entity at a time -- its unique id and a container prepared
// for its struct (PrepareForOverwrite), which the caller fills through slots (vtx_property_slots.h), setting
// every scalar and struct slot -- then Frame() for TryRecordFrame. The containers, their slot vectors and the
// buckets keep their memory from frame to frame; a container added at a position that held another struct's
// entity last frame is reset in full, so an entity is always exactly what a fresh prepared container filled the
// same way would be.
//
// A writer whose policy encodes frames directly (ReplayWriter's direct path) leaves the frame intact, and the
// builder refills the same memory. One that consumes the frame leaves it emptied, and Begin rebuilds it.

#include "vtx/common/readers/schema_reader/game_schema_types.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VTX {

    class FrameBuilder {
    public:
        /// The buckets of the schema, in its order: SchemaRegistry::GetBucketNames, as ReplayWriter records them.
        explicit FrameBuilder(std::vector<std::string> bucket_names)
            : names_(std::move(bucket_names))
            , counts_(names_.size(), 0) {}

        const std::vector<std::string>& BucketNames() const { return names_; }

        /// The index of bucket @p name, for Add; none when the schema declares no such bucket.
        std::optional<size_t> BucketIndex(std::string_view name) const {
            for (size_t i = 0; i < names_.size(); ++i) {
                if (names_[i] == name)
                    return i;
            }
            return std::nullopt;
        }

        /// Starts a frame: every bucket empty, its memory kept.
        void Begin() {
            if (!Laid()) {
                frame_ = Frame {};
                for (const std::string& name : names_)
                    frame_.CreateBucket(name);
            }
            for (size_t& count : counts_)
                count = 0;
            for (Bucket& bucket : frame_.GetMutableBuckets())
                bucket.type_ranges.clear();
        }

        /// A new entity at the end of bucket @p bucket: its container, prepared for @p schema's struct, to be filled
        /// with every scalar and struct slot set.
        PropertyContainer& Add(size_t bucket, std::string unique_id, const SchemaStruct& schema, int32_t type_id) {
            Bucket& b = frame_.GetMutableBuckets()[bucket];
            const size_t at = counts_[bucket]++;
            if (at < b.entities.size()) {
                b.unique_ids[at] = std::move(unique_id);
            } else {
                b.unique_ids.push_back(std::move(unique_id));
                b.entities.emplace_back();
            }
            PropertyContainer& container = b.entities[at];
            Helpers::PrepareForOverwrite(container, schema, type_id);
            return container;
        }

        /// The frame holding exactly the entities added since Begin, for TryRecordFrame.
        Frame& Finish() {
            auto& buckets = frame_.GetMutableBuckets();
            for (size_t i = 0; i < buckets.size(); ++i) {
                buckets[i].entities.resize(counts_[i]);
                buckets[i].unique_ids.resize(counts_[i]);
            }
            return frame_;
        }

    private:
        // The frame still has the schema's buckets in the schema's order.
        bool Laid() const {
            const auto& buckets = frame_.GetBuckets();
            if (buckets.size() != names_.size() || frame_.bucket_map.size() != names_.size())
                return false;
            for (size_t i = 0; i < names_.size(); ++i) {
                const auto at = frame_.bucket_map.find(names_[i]);
                if (at == frame_.bucket_map.end() || at->second != i)
                    return false;
            }
            return true;
        }

        std::vector<std::string> names_;
        std::vector<size_t> counts_;
        Frame frame_;
    };

} // namespace VTX
