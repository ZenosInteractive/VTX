#pragma once
#include "vtx/common/vtx_types.h"
#include "vtx_schema.pb.h"
#include "vtx/common/vtx_types_helpers.h"
/**
 * @file vtx_to_proto.h
 * @brief Utility functions for serializing VTX native structures to Google Protocol Buffers.
 */

namespace VTX {
    /**
     * @namespace VTX::Serialization
     * @brief Contains helper functions for converting internal data structures to Protobuf messages.
     */
    namespace Serialization {

        /**
         * @brief Serializes a 3D Vector to its Protobuf representation.
         * @param src The source native Vector (x, y, z).
         * @param dst The destination Protobuf message pointer.
         */
        inline void ToProto(VTX::Vector& src, cppvtx::Vector* dst) {
            dst->set_x(src.x);
            dst->set_y(src.y);
            dst->set_z(src.z);
        }

        /**
         * @brief Serializes a Quaternion to its Protobuf representation.
         * @param src The source native Quat (x, y, z, w).
         * @param dst The destination Protobuf message pointer.
         */
        inline void ToProto(VTX::Quat& src, cppvtx::Quat* dst) {
            dst->set_x(src.x);
            dst->set_y(src.y);
            dst->set_z(src.z);
            dst->set_w(src.w);
        }

        /**
         * @brief Serializes a Transform (Translation, Rotation, Scale) to its Protobuf representation.
         * @param src The source native Transform structure.
         * @param dst The destination Protobuf message pointer.
         */
        inline void ToProto(VTX::Transform& src, cppvtx::Transform* dst) {
            ToProto(src.translation, dst->mutable_translation());
            ToProto(src.rotation, dst->mutable_rotation());
            ToProto(src.scale, dst->mutable_scale());
        }

        /**
         * @brief Serializes a FloatRange with its normalized value.
         * @param src The source native FloatRange.
         * @param dst The destination Protobuf message pointer.
         */
        inline void ToProto(VTX::FloatRange& src, cppvtx::FloatRange* dst) {
            dst->set_min(src.min);
            dst->set_max(src.max);
            dst->set_value_normalized(src.value_normalized);
        }

        /**
         * @brief Serializes a PropertyContainer containing various data types.
         * * This function handles:
         * - Scalar properties (Int32, Int64, Float, Double, Bool, String).
         * - Complex object properties (Vector, Quat, Transform, Range).
         * - Flattened arrays using a Data/Offset pattern for dynamic sizing.
         * - Nested generic structures (AnyStruct).
         * * @param src The source native PropertyContainer.
         * @param dst The destination Protobuf message pointer.
         */
        inline void ToProto(VTX::PropertyContainer& src, cppvtx::PropertyContainer* dst) {
            src.content_hash = VTX::Helpers::CalculateContainerHash(src);

            dst->set_type_id(src.entity_type_id);
            dst->set_content_hash(src.content_hash);
            // --- Scalar Properties ---
            for (auto val : src.int32_properties)
                dst->add_int32_properties(val);
            for (auto val : src.int64_properties)
                dst->add_int64_properties(val);
            for (auto val : src.float_properties)
                dst->add_float_properties(val);
            for (auto val : src.double_properties)
                dst->add_double_properties(val);
            for (auto val : src.bool_properties)
                dst->add_bool_properties(val);
            for (auto& val : src.string_properties)
                dst->add_string_properties(val);

            // --- Object Properties ---
            for (auto& val : src.vector_properties)
                ToProto(val, dst->add_vector_properties());
            for (auto& val : src.quat_properties)
                ToProto(val, dst->add_quat_properties());
            for (auto& val : src.transform_properties)
                ToProto(val, dst->add_transform_properties());
            for (auto& val : src.range_properties)
                ToProto(val, dst->add_range_properties());

            // --- Array Properties (Flattened Bucket + offsets) ---
            // Each block checks if the source data is non-empty before mutating the destination.
            if (!src.byte_array_properties.data.empty()) {
                auto* dest_arr = dst->mutable_byte_array_properties();

                dest_arr->add_data(src.byte_array_properties.data.data(), src.byte_array_properties.data.size());
                for (auto o : src.byte_array_properties.offsets) {
                    dest_arr->add_offsets(o);
                }
            }

            if (!src.int32_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_int32_arrays();
                for (auto v : src.int32_arrays.data)
                    dest_arr->add_data(v);
                for (auto o : src.int32_arrays.offsets)
                    dest_arr->add_offsets(o);
            }

            if (!src.int64_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_int64_arrays();
                for (auto v : src.int64_arrays.data)
                    dest_arr->add_data(v);
                for (auto o : src.int64_arrays.offsets)
                    dest_arr->add_offsets(o);
            }

            if (!src.float_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_float_arrays();
                for (auto v : src.float_arrays.data)
                    dest_arr->add_data(v);
                for (auto o : src.float_arrays.offsets)
                    dest_arr->add_offsets(o);
            }

            if (!src.double_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_double_arrays();
                for (auto v : src.double_arrays.data)
                    dest_arr->add_data(v);
                for (auto o : src.double_arrays.offsets)
                    dest_arr->add_offsets(o);
            }

            if (!src.bool_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_bool_arrays();
                for (auto v : src.bool_arrays.data)
                    dest_arr->add_data(v);
                for (auto o : src.bool_arrays.offsets)
                    dest_arr->add_offsets(o);
            }

            if (!src.string_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_string_arrays();
                for (auto& v : src.string_arrays.data)
                    dest_arr->add_data(v);
                for (auto o : src.string_arrays.offsets)
                    dest_arr->add_offsets(o);
            }

            if (!src.vector_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_vector_arrays();
                for (auto& v : src.vector_arrays.data)
                    ToProto(v, dest_arr->add_data());
                for (auto o : src.vector_arrays.offsets)
                    dest_arr->add_offsets(o);
            }

            if (!src.quat_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_quat_arrays();
                for (auto& v : src.quat_arrays.data)
                    ToProto(v, dest_arr->add_data());
                for (auto o : src.quat_arrays.offsets)
                    dest_arr->add_offsets(o);
            }

            if (!src.transform_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_transform_arrays();
                for (auto& v : src.transform_arrays.data)
                    ToProto(v, dest_arr->add_data());
                for (auto o : src.transform_arrays.offsets)
                    dest_arr->add_offsets(o);
            }

            if (!src.range_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_range_arrays();
                for (auto& v : src.range_arrays.data)
                    ToProto(v, dest_arr->add_data());
                for (auto o : src.range_arrays.offsets)
                    dest_arr->add_offsets(o);
            }

            // --- AnyStruct Properties & Arrays ---
            for (auto& child : src.any_struct_properties) {
                ToProto(child, dst->add_any_struct_properties());
            }

            if (!src.any_struct_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_any_struct_arrays();
                for (auto& child : src.any_struct_arrays.data) {
                    ToProto(child, dest_arr->add_data());
                }
                for (auto o : src.any_struct_arrays.offsets) {
                    dest_arr->add_offsets(o);
                }
            }

            for (auto& map_item : src.map_properties) {
                auto* dstMap = dst->add_map_properties();
                for (auto& key : map_item.keys)
                    dstMap->add_keys(key);
                for (auto& val : map_item.values)
                    ToProto(val, dstMap->add_values());
            }

            if (!src.map_arrays.data.empty()) {
                auto* dst_arr = dst->mutable_map_arrays();
                for (auto& map_item : src.map_arrays.data) {
                    auto* dst_map = dst_arr->add_data();
                    for (auto& key : map_item.keys)
                        dst_map->add_keys(key);
                    for (auto& val : map_item.values)
                        ToProto(val, dst_map->add_values());
                }
                for (auto o : src.map_arrays.offsets)
                    dst_arr->add_offsets(o);
            }
        }

        /**
         * @brief Serializes a Data block containing unique IDs and entities.
         * @param src The source native Data structure.
         * @param dst The destination Protobuf message pointer.
         */
        inline void ToProto(VTX::Bucket& src, cppvtx::Bucket* dst) {
            dst->mutable_unique_ids()->Reserve(static_cast<int>(src.unique_ids.size()));
            for (auto& id : src.unique_ids)
                dst->add_unique_ids(id);

            dst->mutable_entities()->Reserve(static_cast<int>(src.entities.size()));
            for (auto& entity : src.entities) {
                ToProto(entity, dst->add_entities());
            }

            for (auto& native_range : src.type_ranges) {
                cppvtx::EntityRange* proto_range = dst->add_type_ranges();
                proto_range->set_start_index(native_range.start_index);
                proto_range->set_count(native_range.count);
            }
        }

        /**
         * @brief Serializes a complete Frame by processing its constituent data buckets.
         * @param src The source native Frame object.
         * @param dst The destination Protobuf message pointer.
         */
        inline void ToProto(VTX::Frame& src, cppvtx::Frame* dst) {
            for (auto& dataBlock : src.GetMutableBuckets()) {
                ToProto(dataBlock, dst->add_data());
            }
        }
    } // namespace Serialization
} // namespace VTX