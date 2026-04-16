
#include "vtx_schema_generated.h"
#include "vtx/common/vtx_types.h"
#include "vtx/writer/serialization/vtx_to_flatbuffer.h"

#include "vtx/common/vtx_types_helpers.h"

namespace VTX
{
    namespace Serialization
    {
        template <typename T, typename CreateFunc>
        auto CreateScalarArray(flatbuffers::FlatBufferBuilder& builder, const std::vector<T>& data, const std::vector<uint32_t>& offsets, CreateFunc func) 
        {
            if (data.empty()) return decltype(func(builder, 0, 0))(0);
        
            auto data_vec = builder.CreateVector(data);
            auto offs_vec = builder.CreateVector(offsets);
            return func(builder, data_vec, offs_vec);
        }

        template <typename NativeT, typename FbsT, typename CreateFunc>
        auto CreateStructArray(flatbuffers::FlatBufferBuilder& builder, std::vector<NativeT>& data, const std::vector<uint32_t>& offsets, CreateFunc func) 
        {
            if (data.empty()) return decltype(func(builder, 0, 0))(0);

            std::vector<FbsT> temp; 
            temp.reserve(data.size());
            for(auto& x : data) temp.push_back(ToFlat(x)); 

            auto data_vec = builder.CreateVectorOfStructs(temp);
            auto offs_vec = builder.CreateVector(offsets);
            return func(builder, data_vec, offs_vec);
        }

        template <typename CreateFunc>
        static auto CreateStringArray(flatbuffers::FlatBufferBuilder& builder, const std::vector<std::string>& data, const std::vector<uint32_t>& offsets, CreateFunc func) 
        {
            if (data.empty()) return decltype(func(builder, 0, 0))(0);

            std::vector<flatbuffers::Offset<flatbuffers::String>> str_offsets;
            str_offsets.reserve(data.size());
            for(const auto& s : data) str_offsets.push_back(builder.CreateString(s));

            auto data_vec = builder.CreateVector(str_offsets);
            auto offs_vec = builder.CreateVector(offsets);
            return func(builder, data_vec, offs_vec);
        }

        template <typename CreateFunc>
        static auto CreateBoolFromBitArray(flatbuffers::FlatBufferBuilder& builder, const std::vector<bool>& data, const std::vector<uint32_t>& offsets, CreateFunc func) 
        {
            if (data.empty()) return decltype(func(builder, 0, 0))(0);
        
            std::vector<uint8_t> temp_bool(data.begin(), data.end());
            auto dataVec = builder.CreateVector(temp_bool);
            auto offsVec = builder.CreateVector(offsets);
            return func(builder, dataVec, offsVec);
        }
    }
};
// =========================================================================
// IMPLEMENTACIÓN DE LA API (Sin inlines)
// =========================================================================

fbsvtx::Vector VTX::Serialization::ToFlat(VTX::Vector& v) {
    return {v.x, v.y, v.z};
}

fbsvtx::Quat VTX::Serialization::ToFlat(VTX::Quat& q) {
    return {q.x, q.y, q.z, q.w};
}

fbsvtx::Transform VTX::Serialization::ToFlat(VTX::Transform& t) {
    return {ToFlat(t.translation), ToFlat(t.rotation), ToFlat(t.scale)};
}

fbsvtx::FloatRange VTX::Serialization::ToFlat(VTX::FloatRange& r) {
    return {r.min, r.max, r.value_normalized};
}

flatbuffers::Offset<fbsvtx::MapContainer> VTX::Serialization::ToFlat(flatbuffers::FlatBufferBuilder& builder, VTX::MapContainer& src) {
    if (src.keys.empty()) return 0;
    
    std::vector<flatbuffers::Offset<flatbuffers::String>> k;
    k.reserve(src.keys.size());
    for(const auto& s : src.keys) k.push_back(builder.CreateString(s));
    
    std::vector<flatbuffers::Offset<fbsvtx::PropertyContainer>> v;
    v.reserve(src.values.size());
    for(auto& p : src.values) v.push_back(ToFlat(builder, p)); 

    return fbsvtx::CreateMapContainer(builder, builder.CreateVector(k), builder.CreateVector(v));
}

flatbuffers::Offset<fbsvtx::PropertyContainer> VTX::Serialization::ToFlat(flatbuffers::FlatBufferBuilder& builder, VTX::PropertyContainer& src) {
    
    src.content_hash = VTX::Helpers::CalculateContainerHash(src);
    // --- SCALARS ---
    auto int32Off   = src.int32_properties.empty() ? 0 : builder.CreateVector(src.int32_properties);
    auto int64Off   = src.int64_properties.empty() ? 0 : builder.CreateVector(src.int64_properties);
    auto floatOff   = src.float_properties.empty() ? 0 : builder.CreateVector(src.float_properties);
    auto doubleOff  = src.double_properties.empty() ? 0 : builder.CreateVector(src.double_properties);
    
    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> boolOff = 0;
    if (!src.bool_properties.empty()) {
        std::vector<uint8_t> temp(src.bool_properties.begin(), src.bool_properties.end());
        boolOff = builder.CreateVector(temp);
    }

    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>> stringOff = 0;
    if (!src.string_properties.empty()) {
        std::vector<flatbuffers::Offset<flatbuffers::String>> strs;
        for(const auto& s : src.string_properties) strs.push_back(builder.CreateString(s));
        stringOff = builder.CreateVector(strs);
    }

    // --- MATH PROPS ---
    flatbuffers::Offset<flatbuffers::Vector<const fbsvtx::Vector*>> vecPropOff = 0;
    if(!src.vector_properties.empty()){
        std::vector<fbsvtx::Vector> temp; temp.reserve(src.vector_properties.size());
        for(auto& v : src.vector_properties) temp.push_back(ToFlat(v));
        vecPropOff = builder.CreateVectorOfStructs(temp);
    }
    
    flatbuffers::Offset<flatbuffers::Vector<const fbsvtx::Quat*>> quatPropOff = 0;
    if(!src.quat_properties.empty()){
        std::vector<fbsvtx::Quat> temp; temp.reserve(src.quat_properties.size());
        for(auto& v : src.quat_properties) temp.push_back(ToFlat(v));
        quatPropOff = builder.CreateVectorOfStructs(temp);
    }

    flatbuffers::Offset<flatbuffers::Vector<const fbsvtx::Transform*>> transPropOff = 0;
    if(!src.transform_properties.empty()){
        std::vector<fbsvtx::Transform> temp; temp.reserve(src.transform_properties.size());
        for(auto& v : src.transform_properties) temp.push_back(ToFlat(v));
        transPropOff = builder.CreateVectorOfStructs(temp);
    }

    flatbuffers::Offset<flatbuffers::Vector<const fbsvtx::FloatRange*>> rangePropOff = 0;
    if(!src.range_properties.empty()){
        std::vector<fbsvtx::FloatRange> temp; temp.reserve(src.range_properties.size());
        for(auto& v : src.range_properties) temp.push_back(ToFlat(v));
        rangePropOff = builder.CreateVectorOfStructs(temp);
    }

    // --- FLAT ARRAYS (SoA) ---
    auto flatBytes = CreateScalarArray(builder, src.byte_array_properties.data, src.byte_array_properties.offsets, 
        [](auto& b, auto d, auto o){ return fbsvtx::CreateFlatBytesArray(b, d, o); });

    auto flatInt32 = CreateScalarArray(builder, src.int32_arrays.data, src.int32_arrays.offsets, 
        [](auto& b, auto d, auto o){ return fbsvtx::CreateFlatIntArray(b, d, o); });

    auto flatInt64 = CreateScalarArray(builder, src.int64_arrays.data, src.int64_arrays.offsets, 
        [](auto& b, auto d, auto o){ return fbsvtx::CreateFlatInt64Array(b, d, o); });

    auto flatFloat = CreateScalarArray(builder, src.float_arrays.data, src.float_arrays.offsets, 
        [](auto& b, auto d, auto o){ return fbsvtx::CreateFlatFloatArray(b, d, o); });

    auto flatDouble = CreateScalarArray(builder, src.double_arrays.data, src.double_arrays.offsets, 
        [](auto& b, auto d, auto o){ return fbsvtx::CreateFlatDoubleArray(b, d, o); });
    
    auto flatString = CreateStringArray(builder, src.string_arrays.data, src.string_arrays.offsets, 
        [](auto& b, auto d, auto o){ return fbsvtx::CreateFlatStringArray(b, d, o); });
    
    auto flatBool = CreateScalarArray(builder, src.bool_arrays.data, src.bool_arrays.offsets, 
        [](auto& b, auto d, auto o){ return fbsvtx::CreateFlatBoolArray(b, d, o); });

    // Struct Arrays
    auto flatVec = CreateStructArray<VTX::Vector, fbsvtx::Vector>(builder, src.vector_arrays.data, src.vector_arrays.offsets, 
        [](auto& b, auto d, auto o){ return fbsvtx::CreateFlatVectorArray(b, d, o); });

    auto flatQuat = CreateStructArray<VTX::Quat, fbsvtx::Quat>(builder, src.quat_arrays.data, src.quat_arrays.offsets, 
        [](auto& b, auto d, auto o){ return fbsvtx::CreateFlatQuatArray(b, d, o); });

    auto flatTrans = CreateStructArray<VTX::Transform, fbsvtx::Transform>(builder, src.transform_arrays.data, src.transform_arrays.offsets, 
        [](auto& b, auto d, auto o){ return fbsvtx::CreateFlatTransformArray(b, d, o); });

    auto flatRange = CreateStructArray<VTX::FloatRange, fbsvtx::FloatRange>(builder, src.range_arrays.data, src.range_arrays.offsets, 
        [](auto& b, auto d, auto o){ return fbsvtx::CreateFlatRangeArray(b, d, o); });

    // --- RECURSIVE ---
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<fbsvtx::PropertyContainer>>> anyStructOff = 0;
    if (!src.any_struct_properties.empty()) {
        std::vector<flatbuffers::Offset<fbsvtx::PropertyContainer>> offs;
        for (auto& child : src.any_struct_properties) offs.push_back(ToFlat(builder, child));
        anyStructOff = builder.CreateVector(offs);
    }

    flatbuffers::Offset<fbsvtx::FlatPropertyContainerArray> anyStructArrOff = 0;
    if(!src.any_struct_arrays.data.empty()) {
        std::vector<flatbuffers::Offset<fbsvtx::PropertyContainer>> d;
        for(auto& item : src.any_struct_arrays.data) d.push_back(ToFlat(builder, item));
        
        anyStructArrOff = fbsvtx::CreateFlatPropertyContainerArray(
            builder, builder.CreateVector(d), builder.CreateVector(src.any_struct_arrays.offsets));
    }

    // --- MAPS ---
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<fbsvtx::MapContainer>>> mapPropOff = 0;
    if(!src.map_properties.empty()) {
        std::vector<flatbuffers::Offset<fbsvtx::MapContainer>> m;
        for(auto& item : src.map_properties) m.push_back(ToFlat(builder, item));
        mapPropOff = builder.CreateVector(m);
    }

    flatbuffers::Offset<fbsvtx::FlatMapArray> mapArrOff = 0;
    if(!src.map_arrays.data.empty()) {
        std::vector<flatbuffers::Offset<fbsvtx::MapContainer>> d;
        for(auto& item : src.map_arrays.data) d.push_back(ToFlat(builder, item));
        mapArrOff = fbsvtx::CreateFlatMapArray(
            builder, builder.CreateVector(d), builder.CreateVector(src.map_arrays.offsets));
    }

    // --- FINAL BUILD ---
    fbsvtx::PropertyContainerBuilder containerBuilder(builder);
    
    containerBuilder.add_type_id(src.entity_type_id);
    containerBuilder.add_content_hash(src.content_hash);
    
    if(!boolOff.IsNull())   containerBuilder.add_bool_properties(boolOff);
    if(!int32Off.IsNull())  containerBuilder.add_int32_properties(int32Off);
    if(!int64Off.IsNull())  containerBuilder.add_int64_properties(int64Off);
    if(!floatOff.IsNull())  containerBuilder.add_float_properties(floatOff);
    if(!doubleOff.IsNull()) containerBuilder.add_double_properties(doubleOff);
    if(!stringOff.IsNull()) containerBuilder.add_string_properties(stringOff);
    
    if(!vecPropOff.IsNull())   containerBuilder.add_vector_properties(vecPropOff);
    if(!quatPropOff.IsNull())  containerBuilder.add_quat_properties(quatPropOff);
    if(!transPropOff.IsNull()) containerBuilder.add_transform_properties(transPropOff);
    if(!rangePropOff.IsNull()) containerBuilder.add_range_properties(rangePropOff);

    if(!flatBytes.IsNull())  containerBuilder.add_byte_array_properties(flatBytes);
    if(!flatInt32.IsNull())  containerBuilder.add_int32_arrays(flatInt32);
    if(!flatInt64.IsNull())  containerBuilder.add_int64_arrays(flatInt64);
    if(!flatFloat.IsNull())  containerBuilder.add_float_arrays(flatFloat);
    if(!flatDouble.IsNull()) containerBuilder.add_double_arrays(flatDouble);
    if(!flatString.IsNull()) containerBuilder.add_string_arrays(flatString);
    if(!flatBool.IsNull())   containerBuilder.add_bool_arrays(flatBool);
    
    if(!flatVec.IsNull())    containerBuilder.add_vector_arrays(flatVec);
    if(!flatQuat.IsNull())   containerBuilder.add_quat_arrays(flatQuat);
    if(!flatTrans.IsNull())  containerBuilder.add_transform_arrays(flatTrans);
    if(!flatRange.IsNull())  containerBuilder.add_range_arrays(flatRange);

    if(!anyStructOff.IsNull())    containerBuilder.add_any_struct_properties(anyStructOff);
    if(!anyStructArrOff.IsNull()) containerBuilder.add_any_struct_arrays(anyStructArrOff);
    if(!mapPropOff.IsNull())      containerBuilder.add_map_properties(mapPropOff);
    if(!mapArrOff.IsNull())       containerBuilder.add_map_arrays(mapArrOff);

    return containerBuilder.Finish();
}

flatbuffers::Offset<fbsvtx::Bucket> VTX::Serialization::ToFlat(flatbuffers::FlatBufferBuilder& builder, VTX::Bucket& src) {
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>> idsOff = 0;
    if(!src.unique_ids.empty()) {
        std::vector<flatbuffers::Offset<flatbuffers::String>> offsets;
        for(const auto& s : src.unique_ids) offsets.push_back(builder.CreateString(s));
        idsOff = builder.CreateVector(offsets);
    }

    std::vector<flatbuffers::Offset<fbsvtx::PropertyContainer>> entOffsets;
    for(auto& e : src.entities) entOffsets.push_back(ToFlat(builder, e));
    auto entitiesOff = builder.CreateVector(entOffsets);
    
    flatbuffers::Offset<flatbuffers::Vector<const fbsvtx::EntityRange*>> rangesOff = 0;
    if (!src.type_ranges.empty()) {
        std::vector<fbsvtx::EntityRange> fbRanges;
        fbRanges.reserve(src.type_ranges.size());
        for (const auto& r : src.type_ranges) {
            fbRanges.emplace_back(r.start_index, r.count); 
        }
        rangesOff = builder.CreateVectorOfStructs(fbRanges);
    }

    return fbsvtx::CreateBucket(builder, idsOff, entitiesOff, rangesOff);
}

flatbuffers::Offset<fbsvtx::Frame> VTX::Serialization::ToFlat(flatbuffers::FlatBufferBuilder& builder, VTX::Frame& src) {
    auto& src_buckets = src.GetMutableBuckets();
    
    std::vector<flatbuffers::Offset<fbsvtx::Bucket>> fbs_buckets;
    fbs_buckets.reserve(src_buckets.size());

    for (size_t i = 0; i < src_buckets.size(); ++i) {
        fbs_buckets.push_back(ToFlat(builder, src_buckets[i]));
    }

    auto bucketsVecOff = builder.CreateVector(fbs_buckets);

    return fbsvtx::CreateFrame(builder, bucketsVecOff);
}
