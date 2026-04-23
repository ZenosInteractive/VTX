#pragma once

#include "vtx/common/vtx_types.h"
namespace fbsvtx {
    struct Vector;
    struct Quat;
    struct Transform;
    struct FloatRange;
    struct MapContainer;
    struct PropertyContainer;
    struct Bucket;
    struct Frame;
    struct FileFooter;
    struct FileHeader;
    struct ChunkIndexEntry;
    struct ReplayTimeData;
    struct TimelineEvent;
} // namespace fbsvtx

namespace VTX {
    namespace Serialization {

        void FromFlat(const fbsvtx::Vector* src, VTX::Vector& dst);
        void FromFlat(const fbsvtx::Quat* src, VTX::Quat& dst);
        void FromFlat(const fbsvtx::Transform* src, VTX::Transform& dst);
        void FromFlat(const fbsvtx::FloatRange* src, VTX::FloatRange& dst);

        void FromFlat(const fbsvtx::MapContainer* src, VTX::MapContainer& dst);
        void FromFlat(const fbsvtx::PropertyContainer* src, VTX::PropertyContainer& dst);
        void FromFlat(const fbsvtx::Bucket* src, VTX::Bucket& dst);
        void FromFlat(const fbsvtx::Frame* src, VTX::Frame& dst);
        void FromFlat(const fbsvtx::FileFooter* src, VTX::FileFooter& dst);
        void FromFlat(const fbsvtx::FileHeader* src, VTX::FileHeader& dst);
        void FromFlat(const fbsvtx::ChunkIndexEntry* src, VTX::ChunkIndexEntry& dst);
        void FromFlat(const fbsvtx::ReplayTimeData* src, VTX::ReplayTimeData& dst);
        void FromFlat(const fbsvtx::TimelineEvent* src, VTX::TimelineEvent& dst);

    } // namespace Serialization
} // namespace VTX