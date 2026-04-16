#pragma once
#include "vtx/common/vtx_types.h"
namespace cppvtx {
    class Vector;
    class Quat;
    class Transform;
    class FloatRange;
    class PropertyContainer;
    class Bucket;
    class Frame;
}

namespace VTX {
    namespace Serialization {
        void FromProto(const cppvtx::Vector& src, VTX::Vector& dst);
        void FromProto(const cppvtx::Quat& src, VTX::Quat& dst);
        void FromProto(const cppvtx::Transform& src, VTX::Transform& dst);
        void FromProto(const cppvtx::FloatRange& src, VTX::FloatRange& dst);
        void FromProto(const cppvtx::PropertyContainer& proto, VTX::PropertyContainer& out);
        void FromProto(const cppvtx::Bucket& proto, VTX::Bucket& out);
        void FromProto(const cppvtx::Frame& proto, VTX::Frame& out);
    }
}