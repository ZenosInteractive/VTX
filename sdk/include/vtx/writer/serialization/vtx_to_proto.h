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

        namespace detail {
            template <typename RepeatedField, typename Source>
            inline void AppendAll(RepeatedField* dst, const Source& src) {
                if (!src.empty())
                    dst->Add(src.begin(), src.end());
            }

            // The body of ToProto(PropertyContainer): every content hash in src's tree is already stored
            // (Helpers::StoreContainerHashes), so nothing here hashes.
            inline void ToProtoHashed(VTX::PropertyContainer& src, cppvtx::PropertyContainer* dst);
        } // namespace detail

        /**
         * @brief Serializes a PropertyContainer containing various data types.
         * * This function handles:
         * - Scalar properties (Int32, Int64, Float, Double, Bool, String).
         * - Complex object properties (Vector, Quat, Transform, Range).
         * - Flattened arrays using a Data/Offset pattern for dynamic sizing.
         * - Nested generic structures (AnyStruct).
         * * @param src The source native PropertyContainer.
         * @param dst The destination Protobuf message pointer.
         * @details Stores the content hash of src and of every container nested in it.
         */
        inline void ToProto(VTX::PropertyContainer& src, cppvtx::PropertyContainer* dst) {
            // Hash the whole tree once, nested containers first, then copy it out.
            VTX::Helpers::StoreContainerHashes(src);
            detail::ToProtoHashed(src, dst);
        }

        inline void detail::ToProtoHashed(VTX::PropertyContainer& src, cppvtx::PropertyContainer* dst) {
            dst->set_type_id(src.entity_type_id);
            dst->set_content_hash(src.content_hash);
            // --- Scalar Properties ---
            AppendAll(dst->mutable_int32_properties(), src.int32_properties);
            AppendAll(dst->mutable_int64_properties(), src.int64_properties);
            AppendAll(dst->mutable_float_properties(), src.float_properties);
            AppendAll(dst->mutable_double_properties(), src.double_properties);
            if (!src.bool_properties.empty()) {
                auto* bools = dst->mutable_bool_properties();
                bools->Reserve(static_cast<int>(src.bool_properties.size()));
                for (bool val : src.bool_properties)
                    bools->AddAlreadyReserved(val);
            }
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
                AppendAll(dest_arr->mutable_offsets(), src.byte_array_properties.offsets);
            }

            if (!src.int32_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_int32_arrays();
                AppendAll(dest_arr->mutable_data(), src.int32_arrays.data);
                AppendAll(dest_arr->mutable_offsets(), src.int32_arrays.offsets);
            }

            if (!src.int64_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_int64_arrays();
                AppendAll(dest_arr->mutable_data(), src.int64_arrays.data);
                AppendAll(dest_arr->mutable_offsets(), src.int64_arrays.offsets);
            }

            if (!src.float_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_float_arrays();
                AppendAll(dest_arr->mutable_data(), src.float_arrays.data);
                AppendAll(dest_arr->mutable_offsets(), src.float_arrays.offsets);
            }

            if (!src.double_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_double_arrays();
                AppendAll(dest_arr->mutable_data(), src.double_arrays.data);
                AppendAll(dest_arr->mutable_offsets(), src.double_arrays.offsets);
            }

            if (!src.bool_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_bool_arrays();
                AppendAll(dest_arr->mutable_data(), src.bool_arrays.data);
                AppendAll(dest_arr->mutable_offsets(), src.bool_arrays.offsets);
            }

            if (!src.string_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_string_arrays();
                for (auto& v : src.string_arrays.data)
                    dest_arr->add_data(v);
                AppendAll(dest_arr->mutable_offsets(), src.string_arrays.offsets);
            }

            if (!src.vector_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_vector_arrays();
                for (auto& v : src.vector_arrays.data)
                    ToProto(v, dest_arr->add_data());
                AppendAll(dest_arr->mutable_offsets(), src.vector_arrays.offsets);
            }

            if (!src.quat_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_quat_arrays();
                for (auto& v : src.quat_arrays.data)
                    ToProto(v, dest_arr->add_data());
                AppendAll(dest_arr->mutable_offsets(), src.quat_arrays.offsets);
            }

            if (!src.transform_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_transform_arrays();
                for (auto& v : src.transform_arrays.data)
                    ToProto(v, dest_arr->add_data());
                AppendAll(dest_arr->mutable_offsets(), src.transform_arrays.offsets);
            }

            if (!src.range_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_range_arrays();
                for (auto& v : src.range_arrays.data)
                    ToProto(v, dest_arr->add_data());
                AppendAll(dest_arr->mutable_offsets(), src.range_arrays.offsets);
            }

            // --- AnyStruct Properties & Arrays ---
            for (auto& child : src.any_struct_properties) {
                ToProtoHashed(child, dst->add_any_struct_properties());
            }

            if (!src.any_struct_arrays.data.empty()) {
                auto* dest_arr = dst->mutable_any_struct_arrays();
                for (auto& child : src.any_struct_arrays.data) {
                    ToProtoHashed(child, dest_arr->add_data());
                }
                AppendAll(dest_arr->mutable_offsets(), src.any_struct_arrays.offsets);
            }

            for (auto& map_item : src.map_properties) {
                auto* dstMap = dst->add_map_properties();
                for (auto& key : map_item.keys)
                    dstMap->add_keys(key);
                for (auto& val : map_item.values)
                    ToProtoHashed(val, dstMap->add_values());
            }

            if (!src.map_arrays.data.empty()) {
                auto* dst_arr = dst->mutable_map_arrays();
                for (auto& map_item : src.map_arrays.data) {
                    auto* dst_map = dst_arr->add_data();
                    for (auto& key : map_item.keys)
                        dst_map->add_keys(key);
                    for (auto& val : map_item.values)
                        ToProtoHashed(val, dst_map->add_values());
                }
                AppendAll(dst_arr->mutable_offsets(), src.map_arrays.offsets);
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