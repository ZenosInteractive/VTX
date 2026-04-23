#pragma once

#include "vtx/common/vtx_types.h"
#include <flatbuffers/flatbuffer_builder.h>

namespace flatbuffers {
    template <typename T>
    struct Offset;
}

namespace fbsvtx {
    struct Vector;
    struct Quat;
    struct Transform;
    struct FloatRange;
    struct PropertyContainer;
    struct MapContainer;
    struct Bucket;
    struct Frame;
} // namespace fbsvtx

namespace VTX {
    namespace Serialization {

        fbsvtx::Vector ToFlat(VTX::Vector& v);
        fbsvtx::Quat ToFlat(VTX::Quat& q);
        fbsvtx::Transform ToFlat(VTX::Transform& t);
        fbsvtx::FloatRange ToFlat(VTX::FloatRange& r);

        flatbuffers::Offset<fbsvtx::MapContainer> ToFlat(flatbuffers::FlatBufferBuilder& builder,
                                                         VTX::MapContainer& src);
        flatbuffers::Offset<fbsvtx::PropertyContainer> ToFlat(flatbuffers::FlatBufferBuilder& builder,
                                                              VTX::PropertyContainer& src);
        flatbuffers::Offset<fbsvtx::Bucket> ToFlat(flatbuffers::FlatBufferBuilder& builder, VTX::Bucket& src);
        flatbuffers::Offset<fbsvtx::Frame> ToFlat(flatbuffers::FlatBufferBuilder& builder, VTX::Frame& src);

    } // namespace Serialization
} // namespace VTX