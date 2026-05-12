#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "vtx/common/vtx_concepts.h"
#include "vtx/common/vtx_frame_accessor.h"
#include "vtx/common/vtx_types.h"

namespace VTX {

    class EntityMutator {
    public:
        EntityMutator() = default;
        explicit EntityMutator(PropertyContainer& data)
            : data_(&data) {}

        template <VtxScalarType T>
        typename EntityView::template ScalarRetType<T> Get(PropertyKey<T> key) const {
            return AsView().Get(key);
        }

        template <VtxScalarType T>
        void Set(PropertyKey<T> key, T value) {
            if (!data_ || !key.IsValid())
                return;
            constexpr auto MemberPtr = EntityView::GetContainerMember<T>();
            auto& values = data_->*MemberPtr;
            if (static_cast<size_t>(key.index) >= values.size())
                return;
            values[key.index] = std::move(value);
        }

        EntityMutator GetMutableView(PropertyKey<EntityView> key) {
            if (!data_ || !key.IsValid())
                return {};
            if (static_cast<size_t>(key.index) >= data_->any_struct_properties.size())
                return {};
            return EntityMutator(data_->any_struct_properties[key.index]);
        }

        std::span<PropertyContainer> GetMutableViewArray(PropertyKey<std::span<const PropertyContainer>> key) {
            if (!data_ || !key.IsValid())
                return {};
            return data_->any_struct_arrays.GetMutableSubArray(key.index);
        }

        template <VtxArrayType T>
        auto GetMutableArray(PropertyKey<T> key) {
            constexpr auto MemberPtr = EntityView::GetArrayContainerMember<T>();
            using SpanType = decltype((data_->*MemberPtr).GetMutableSubArray(0));
            if (!data_ || !key.IsValid())
                return SpanType {};
            return (data_->*MemberPtr).GetMutableSubArray(key.index);
        }

        EntityView AsView() const { return data_ ? EntityView(*data_) : EntityView(); }

        PropertyContainer* raw() noexcept { return data_; }
        const PropertyContainer* raw() const noexcept { return data_; }
        bool valid() const noexcept { return data_ != nullptr; }

    private:
        PropertyContainer* data_ = nullptr;
    };

    class BucketMutator {
    public:
        BucketMutator() = default;
        explicit BucketMutator(Bucket& bucket)
            : bucket_(&bucket) {}

        size_t entity_count() const noexcept { return bucket_ ? bucket_->entities.size() : 0; }

        EntityMutator entity(uint32_t i) {
            if (!bucket_ || i >= bucket_->entities.size())
                return {};
            return EntityMutator(bucket_->entities[i]);
        }

        EntityView entity_view(uint32_t i) const {
            if (!bucket_ || i >= bucket_->entities.size())
                return {};
            return EntityView(bucket_->entities[i]);
        }

        class iterator {
        public:
            iterator() = default;
            iterator(Bucket* b, size_t i)
                : bucket_(b)
                , idx_(i) {}

            EntityMutator operator*() const { return EntityMutator(bucket_->entities[idx_]); }
            iterator& operator++() {
                ++idx_;
                return *this;
            }
            bool operator==(const iterator& o) const noexcept { return bucket_ == o.bucket_ && idx_ == o.idx_; }
            bool operator!=(const iterator& o) const noexcept { return !(*this == o); }

        private:
            Bucket* bucket_ = nullptr;
            size_t idx_ = 0;
        };

        iterator begin() { return bucket_ ? iterator {bucket_, 0} : iterator {}; }
        iterator end() { return bucket_ ? iterator {bucket_, bucket_->entities.size()} : iterator {}; }

        std::span<PropertyContainer> entities_of_type(int32_t type_id) {
            if (!bucket_)
                return {};
            if (type_id < 0 || static_cast<size_t>(type_id) >= bucket_->type_ranges.size())
                return {};
            const auto& range = bucket_->type_ranges[type_id];
            if (range.count <= 0)
                return {};
            return std::span<PropertyContainer>(bucket_->entities.data() + range.start_index,
                                                static_cast<size_t>(range.count));
        }

        EntityMutator AddEntity() {
            if (!bucket_)
                return {};
            bucket_->entities.emplace_back();
            if (bucket_->unique_ids.size() + 1 == bucket_->entities.size())
                bucket_->unique_ids.emplace_back();
            return EntityMutator(bucket_->entities.back());
        }

        void RemoveEntity(uint32_t entity_index) {
            if (!bucket_ || entity_index >= bucket_->entities.size())
                return;
            bucket_->entities.erase(bucket_->entities.begin() + entity_index);
            if (entity_index < bucket_->unique_ids.size())
                bucket_->unique_ids.erase(bucket_->unique_ids.begin() + entity_index);
            for (auto& r : bucket_->type_ranges) {
                if (r.count <= 0)
                    continue;
                const int32_t end = r.start_index + r.count;
                if (static_cast<int32_t>(entity_index) < r.start_index) {
                    --r.start_index;
                } else if (static_cast<int32_t>(entity_index) < end) {
                    --r.count;
                }
            }
        }

        template <class Predicate>
        size_t RemoveIf(Predicate pred) {
            if (!bucket_)
                return 0;
            const size_t before = bucket_->entities.size();
            std::vector<uint8_t> keep(before, 1);
            for (size_t i = 0; i < before; ++i) {
                if (pred(EntityView(bucket_->entities[i])))
                    keep[i] = 0;
            }
            size_t write = 0;
            for (size_t read = 0; read < before; ++read) {
                if (!keep[read])
                    continue;
                if (write != read) {
                    bucket_->entities[write] = std::move(bucket_->entities[read]);
                    if (read < bucket_->unique_ids.size() && write < bucket_->unique_ids.size())
                        bucket_->unique_ids[write] = std::move(bucket_->unique_ids[read]);
                }
                ++write;
            }
            bucket_->entities.resize(write);
            if (bucket_->unique_ids.size() > write)
                bucket_->unique_ids.resize(write);
            for (auto& r : bucket_->type_ranges) {
                r.start_index = 0;
                r.count = 0;
            }
            return before - write;
        }

        void Clear() {
            if (!bucket_)
                return;
            bucket_->entities.clear();
            bucket_->unique_ids.clear();
            for (auto& r : bucket_->type_ranges) {
                r.start_index = 0;
                r.count = 0;
            }
        }

        Bucket* raw() noexcept { return bucket_; }
        const Bucket* raw() const noexcept { return bucket_; }
        bool valid() const noexcept { return bucket_ != nullptr; }

    private:
        Bucket* bucket_ = nullptr;
    };

    class FrameMutationView {
    public:
        FrameMutationView() = default;
        FrameMutationView(Frame& frame, const FrameAccessor& accessor)
            : frame_(&frame)
            , accessor_(&accessor) {}

        bool HasBucket(const std::string& name) const {
            if (!frame_)
                return false;
            return frame_->bucket_map.contains(name);
        }

        BucketMutator GetBucket(const std::string& name) {
            if (!frame_)
                return {};
            auto it = frame_->bucket_map.find(name);
            if (it == frame_->bucket_map.end())
                return {};
            return BucketMutator(frame_->buckets[it->second]);
        }

        BucketMutator GetBucket(int32_t bucket_index) {
            if (!frame_ || bucket_index < 0 || static_cast<size_t>(bucket_index) >= frame_->buckets.size())
                return {};
            return BucketMutator(frame_->buckets[static_cast<size_t>(bucket_index)]);
        }

        size_t bucket_count() const noexcept { return frame_ ? frame_->buckets.size() : 0; }

        template <VtxScalarType T>
        PropertyKey<T> ResolveKey(const std::string& struct_name, const std::string& prop_name) const {
            if (!accessor_)
                return PropertyKey<T> {-1};
            return accessor_->Get<T>(struct_name, prop_name);
        }

        template <VtxArrayType T>
        PropertyKey<T> ResolveArrayKey(const std::string& struct_name, const std::string& prop_name) const {
            if (!accessor_)
                return PropertyKey<T> {-1};
            return accessor_->GetArray<T>(struct_name, prop_name);
        }

        PropertyKey<EntityView> ResolveViewKey(const std::string& struct_name, const std::string& prop_name) const {
            if (!accessor_)
                return PropertyKey<EntityView> {-1};
            return accessor_->GetViewKey(struct_name, prop_name);
        }

        const Frame* AsConstFrame() const noexcept { return frame_; }
        Frame* raw() noexcept { return frame_; }
        const FrameAccessor* accessor() const noexcept { return accessor_; }
        bool valid() const noexcept { return frame_ != nullptr && accessor_ != nullptr; }

    private:
        Frame* frame_ = nullptr;
        const FrameAccessor* accessor_ = nullptr;
    };

} // namespace VTX
