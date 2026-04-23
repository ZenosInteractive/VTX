#include "vtx/reader/serialization/proto_to_vtx.h"
#include <vector>
#include "vtx_schema.pb.h"
namespace VTX {
    namespace Serialization {

        template <typename TProtoArr, typename TNativeArr>
        void FromProtoVec(const TProtoArr& src, TNativeArr& dst) {
            dst.data.clear();
            dst.offsets.clear();

            dst.data.reserve(src.data_size());
            dst.offsets.reserve(src.offsets_size());

            for (const auto& v : src.data()) {
                dst.data.push_back(v);
            }
            for (auto o : src.offsets())
                dst.offsets.push_back(o);
        }

        template <typename TProtoArr, typename TNativeArr, typename TFunc>
        void FromProtoComplexVec(const TProtoArr& src, TNativeArr& dst, TFunc unpackFunc) {
            dst.data.resize(src.data_size());
            for (int i = 0; i < src.data_size(); ++i) {
                unpackFunc(src.data(i), dst.data[i]);
            }
            dst.offsets.assign(src.offsets().begin(), src.offsets().end());
        }


        void FromProto(const cppvtx::Vector& src, VTX::Vector& dst) {
            dst.x = src.x();
            dst.y = src.y();
            dst.z = src.z();
        }

        void FromProto(const cppvtx::Quat& src, VTX::Quat& dst) {
            dst.x = src.x();
            dst.y = src.y();
            dst.z = src.z();
            dst.w = src.w();
        }

        void FromProto(const cppvtx::Transform& src, VTX::Transform& dst) {
            FromProto(src.translation(), dst.translation);
            FromProto(src.rotation(), dst.rotation);
            FromProto(src.scale(), dst.scale);
        }

        void FromProto(const cppvtx::FloatRange& src, VTX::FloatRange& dst) {
            dst.min = src.min();
            dst.max = src.max();
            dst.value_normalized = src.value_normalized();
        }

        void FromProto(const cppvtx::PropertyContainer& proto, VTX::PropertyContainer& out) {
            auto Copy = [](auto& dest, const auto& src) {
                dest.assign(src.begin(), src.end());
            };

            out.entity_type_id = proto.type_id();
            out.content_hash = proto.content_hash();

            Copy(out.bool_properties, proto.bool_properties());
            Copy(out.int32_properties, proto.int32_properties());
            Copy(out.int64_properties, proto.int64_properties());
            Copy(out.float_properties, proto.float_properties());
            Copy(out.double_properties, proto.double_properties());
            Copy(out.string_properties, proto.string_properties());

            auto Unpack = [](const auto& p_list, auto& n_list) {
                n_list.resize(p_list.size());
                for (int i = 0; i < p_list.size(); ++i) {
                    FromProto(p_list.Get(i), n_list[i]);
                }
            };

            Unpack(proto.vector_properties(), out.vector_properties);
            Unpack(proto.quat_properties(), out.quat_properties);
            Unpack(proto.transform_properties(), out.transform_properties);
            Unpack(proto.range_properties(), out.range_properties);

            if (proto.has_byte_array_properties()) {
                const auto& proto_bytes = proto.byte_array_properties();
                auto& native_bytes = out.byte_array_properties;

                native_bytes.data.clear();

                for (int i = 0; i < proto_bytes.data_size(); ++i) {
                    const std::string& raw_data = proto_bytes.data(i);
                    native_bytes.data.insert(native_bytes.data.end(), raw_data.begin(), raw_data.end());
                }

                native_bytes.offsets.reserve(proto_bytes.offsets_size());
                for (int o : proto_bytes.offsets()) {
                    native_bytes.offsets.push_back(o);
                }
            }

            if (proto.has_int32_arrays())
                FromProtoVec(proto.int32_arrays(), out.int32_arrays);
            if (proto.has_int64_arrays())
                FromProtoVec(proto.int64_arrays(), out.int64_arrays);
            if (proto.has_float_arrays())
                FromProtoVec(proto.float_arrays(), out.float_arrays);
            if (proto.has_double_arrays())
                FromProtoVec(proto.double_arrays(), out.double_arrays);
            if (proto.has_bool_arrays())
                FromProtoVec(proto.bool_arrays(), out.bool_arrays);
            if (proto.has_string_arrays())
                FromProtoVec(proto.string_arrays(), out.string_arrays);

            if (proto.has_vector_arrays())
                FromProtoComplexVec(proto.vector_arrays(), out.vector_arrays,
                                    [](auto& p, auto& n) { FromProto(p, n); });
            if (proto.has_quat_arrays())
                FromProtoComplexVec(proto.quat_arrays(), out.quat_arrays, [](auto& p, auto& n) { FromProto(p, n); });
            if (proto.has_transform_arrays())
                FromProtoComplexVec(proto.transform_arrays(), out.transform_arrays,
                                    [](auto& p, auto& n) { FromProto(p, n); });
            if (proto.has_range_arrays())
                FromProtoComplexVec(proto.range_arrays(), out.range_arrays, [](auto& p, auto& n) { FromProto(p, n); });

            out.any_struct_properties.resize(proto.any_struct_properties_size());
            for (int i = 0; i < proto.any_struct_properties_size(); ++i) {
                FromProto(proto.any_struct_properties(i), out.any_struct_properties[i]);
            }

            if (proto.has_any_struct_arrays()) {
                FromProtoComplexVec(proto.any_struct_arrays(), out.any_struct_arrays,
                                    [](auto& p, auto& n) { FromProto(p, n); });
            }
        }

        void FromProto(const cppvtx::Bucket& proto, VTX::Bucket& out) {
            out.unique_ids.clear();
            out.unique_ids.reserve(proto.unique_ids_size());
            for (const auto& id : proto.unique_ids()) {
                out.unique_ids.push_back(id);
            }

            out.entities.clear();
            out.entities.resize(proto.entities_size());

            for (int i = 0; i < proto.entities_size(); ++i) {
                FromProto(proto.entities(i), out.entities[i]);
            }

            out.type_ranges.reserve(proto.type_ranges_size());
            for (int i = 0; i < proto.type_ranges_size(); ++i) {
                const auto& r = proto.type_ranges(i);
                out.type_ranges.push_back(VTX::EntityRange {r.start_index(), r.count()});
            }
        }

        void FromProto(const cppvtx::Frame& proto, VTX::Frame& out) {
            auto& buckets = out.GetMutableBuckets();
            buckets.resize(proto.data_size());
            for (int i = 0; i < proto.data_size(); ++i) {
                FromProto(proto.data(i), buckets[i]);
            }
        }
    } // namespace Serialization
} // namespace VTX