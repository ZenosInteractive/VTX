
#include "vtx_schema_generated.h"
#include "vtx/common/vtx_types.h"
#include "vtx/reader/serialization/flatbuffers_to_vtx.h"

// --- MATH TYPES ---
void VTX::Serialization::FromFlat(const fbsvtx::Vector* src, VTX::Vector& dst) {
    if (!src) return;
    dst.x = src->x(); 
    dst.y = src->y(); 
    dst.z = src->z();
}

void VTX::Serialization::FromFlat(const fbsvtx::Quat* src, VTX::Quat& dst) {
    if (!src) return;
    dst.x = src->x(); 
    dst.y = src->y(); 
    dst.z = src->z(); 
    dst.w = src->w();
}

void VTX::Serialization::FromFlat(const fbsvtx::Transform* src, VTX::Transform& dst) {
    if (!src) return;
    FromFlat(&src->translation(), dst.translation); 
    FromFlat(&src->rotation(), dst.rotation);
    FromFlat(&src->scale(), dst.scale);
}

void VTX::Serialization::FromFlat(const fbsvtx::FloatRange* src, VTX::FloatRange& dst) {
    if (!src) return;
    dst.min = src->min(); 
    dst.max = src->max(); 
    dst.value_normalized = src->value_normalized();
}

void VTX::Serialization::FromFlat(const fbsvtx::MapContainer* src, VTX::MapContainer& dst) {
    if (!src) return;
    if (src->keys()) {
        dst.keys.reserve(src->keys()->size());
        for (auto k : *src->keys()) dst.keys.push_back(k->str());
    }
    
    if (src->values()) {
        dst.values.resize(src->values()->size());
        for (size_t i = 0; i < src->values()->size(); ++i) {
            FromFlat(src->values()->Get(i), dst.values[i]);
        }
    }
}

// --- PROPERTY CONTAINER ---
void VTX::Serialization::FromFlat(const fbsvtx::PropertyContainer* src, VTX::PropertyContainer& dst) {
    if (!src) return;
    dst.entity_type_id = src->type_id();
    dst.content_hash = src->content_hash();
    
    // 1. Scalars
    if (auto v = src->bool_properties()) {
        dst.bool_properties.clear();
        dst.bool_properties.reserve(v->size());
        for (auto b : *v) {
            dst.bool_properties.push_back(b != 0);
        }
    }
    
    if (auto v = src->int32_properties())  dst.int32_properties.assign(v->begin(), v->end());
    if (auto v = src->int64_properties())  dst.int64_properties.assign(v->begin(), v->end());
    if (auto v = src->float_properties())  dst.float_properties.assign(v->begin(), v->end());
    if (auto v = src->double_properties()) dst.double_properties.assign(v->begin(), v->end());
    
    if (auto v = src->string_properties()) {
        dst.string_properties.reserve(v->size());
        for(auto s : *v) dst.string_properties.push_back(s->str());
    }

    // 2. Structs (Vector of Structs)
    auto unpackStructs = [](auto* fbsVec, auto& nativeVec) {
        if (!fbsVec) return;
        nativeVec.resize(fbsVec->size());
        for (size_t i = 0; i < fbsVec->size(); ++i) FromFlat(fbsVec->Get(i), nativeVec[i]);
    };

    unpackStructs(src->vector_properties(),    dst.vector_properties);
    unpackStructs(src->quat_properties(),      dst.quat_properties);
    unpackStructs(src->transform_properties(), dst.transform_properties);
    unpackStructs(src->range_properties(),     dst.range_properties);

    // 3. Flat Arrays (SoA)
    auto unpackArray = [](auto* fbsArr, auto& nativeArr) {
        if (!fbsArr) return;
        if (fbsArr->data()) nativeArr.data.assign(fbsArr->data()->begin(), fbsArr->data()->end());
        if (fbsArr->offsets()) nativeArr.offsets.assign(fbsArr->offsets()->begin(), fbsArr->offsets()->end());
    };

    // Special handling for BoolArray if it uses uint8 internally in FBS
    if (auto arr = src->bool_arrays()) {
        if (arr->data()) {
            // Conversion implicit uint8 -> bool
            dst.bool_arrays.data.assign(arr->data()->begin(), arr->data()->end()); 
        }
        if (arr->offsets()) dst.bool_arrays.offsets.assign(arr->offsets()->begin(), arr->offsets()->end());
    }

    unpackArray(src->byte_array_properties(), dst.byte_array_properties);
    unpackArray(src->int32_arrays(),    dst.int32_arrays);
    unpackArray(src->int64_arrays(),    dst.int64_arrays);
    unpackArray(src->float_arrays(),    dst.float_arrays);
    unpackArray(src->double_arrays(),   dst.double_arrays);
    
    // String Array
    if (auto arr = src->string_arrays()) {
        if (auto d = arr->data()) {
            dst.string_arrays.data.reserve(d->size());
            for(auto s : *d) dst.string_arrays.data.push_back(s->str());
        }
        if (auto o = arr->offsets()) dst.string_arrays.offsets.assign(o->begin(), o->end());
    }

    // Struct Arrays (Needs conversion per element)
    auto unpackStructArray = [](auto* fbsArr, auto& nativeArr) {
        if (!fbsArr) return;
        if (auto d = fbsArr->data()) {
            nativeArr.data.resize(d->size());
            for(size_t i = 0; i < d->size(); ++i) {
                FromFlat(d->Get(i), nativeArr.data[i]);
            }
        }
    };

    unpackStructArray(src->vector_arrays(), dst.vector_arrays);
    unpackStructArray(src->quat_arrays(), dst.quat_arrays);
    unpackStructArray(src->transform_arrays(), dst.transform_arrays);
    unpackStructArray(src->range_arrays(), dst.range_arrays);

    // 4. Recursive
    if (auto v = src->any_struct_properties()) {
        dst.any_struct_properties.resize(v->size());
        for(size_t i = 0; i < v->size(); ++i) FromFlat(v->Get(i), dst.any_struct_properties[i]);
    }

    if (auto arr = src->any_struct_arrays()) {
        if (auto d = arr->data()) {
            dst.any_struct_arrays.data.resize(d->size());
            for(size_t i = 0; i < d->size(); ++i) FromFlat(d->Get(i), dst.any_struct_arrays.data[i]);
        }
        if (auto o = arr->offsets()) dst.any_struct_arrays.offsets.assign(o->begin(), o->end());
    }

    // 5. Maps
    if (auto v = src->map_properties()) {
        dst.map_properties.resize(v->size());
        for(size_t i = 0; i < v->size(); ++i) FromFlat(v->Get(i), dst.map_properties[i]);
    }
    
    if (auto arr = src->map_arrays()) {
         if (auto d = arr->data()) {
            dst.map_arrays.data.resize(d->size());
            for(size_t i = 0; i < d->size(); ++i) FromFlat(d->Get(i), dst.map_arrays.data[i]);
        }
        if (auto o = arr->offsets()) dst.map_arrays.offsets.assign(o->begin(), o->end());
    }
}

// --- DATA & FRAME ---
void VTX::Serialization::FromFlat(const fbsvtx::Bucket* src, VTX::Bucket& dst) {
    if(!src) return;
    if(src->unique_ids()) {
        dst.unique_ids.reserve(src->unique_ids()->size());
        for(auto s : *src->unique_ids()) dst.unique_ids.push_back(s->str());
    }
    if(src->entities()) {
        dst.entities.resize(src->entities()->size());
        for(size_t i = 0; i < src->entities()->size(); ++i) FromFlat(src->entities()->Get(i), dst.entities[i]);
    }
    
    if (src->type_ranges()) {
        const auto* ranges = src->type_ranges();
        dst.type_ranges.reserve(ranges->size());

        for (size_t i = 0; i < ranges->size(); ++i) {
            const auto* r = ranges->Get(i);
            dst.type_ranges.push_back(VTX::EntityRange{
                r->start_index(), 
                r->count()
            });
        }
    }
}

void VTX::Serialization::FromFlat(const fbsvtx::Frame* src, VTX::Frame& dst) {
    if(!src || !src->data()) return;
    auto& buckets = dst.GetMutableBuckets();
    buckets.resize(src->data()->size());
    for(size_t i = 0; i < src->data()->size(); ++i) {
        FromFlat(src->data()->Get(i), buckets[i]);
    }
}

void VTX::Serialization::FromFlat(const fbsvtx::FileFooter* src, VTX::FileFooter& dst)
{
    dst.total_frames = src->total_frames();
    dst.duration_seconds = src->duration_seconds();
    
    FromFlat(src->times(),dst.times);
    
    dst.chunk_index.reserve(src->chunk_index()->size());
    for(size_t i = 0; i < src->chunk_index()->size(); ++i) {
        FromFlat(src->chunk_index()->Get(i),  dst.chunk_index[i]);
    }
    
    dst.events.reserve(src->events()->size());
    for(size_t i = 0; i < src->events()->size(); ++i) {
        FromFlat(src->events()->Get(i),  dst.events[i]);
    }
    
    dst.payload_checksum = src->payload_checksum();
}

void VTX::Serialization::FromFlat(const fbsvtx::FileHeader* src, VTX::FileHeader& dst)
{
}

void VTX::Serialization::FromFlat(const fbsvtx::ChunkIndexEntry* src, VTX::ChunkIndexEntry& dst)
{
    dst.chunk_index = src->chunk_index();
    dst.start_frame = src->start_frame();
    dst.end_frame = src->end_frame();
    dst.file_offset = src->file_offset();
    dst.chunk_size_bytes = src->chunk_size_bytes();
    
}

void VTX::Serialization::FromFlat(const fbsvtx::ReplayTimeData* src, VTX::ReplayTimeData& dst)
{
    for(size_t i = 0; i < src->game_time()->size(); ++i) {
        dst.game_time.push_back(src->game_time()->Get(i));
    }
    
    for(size_t i = 0; i < src->created_utc()->size(); ++i) {
        dst.created_utc.push_back(src->created_utc()->Get(i));
    }
    
    for(size_t i = 0; i < src->gaps()->size(); ++i) {
        dst.gaps.push_back(src->gaps()->Get(i));
    }
    
    for(size_t i = 0; i < src->segments()->size(); ++i) {
        dst.segments.push_back(src->segments()->Get(i));
    }
}

void VTX::Serialization::FromFlat(const fbsvtx::TimelineEvent* src, VTX::TimelineEvent& dst)
{
    dst.game_time = src->game_time();
    dst.event_type = *src->event_type();
    dst.label = *src->label();
    FromFlat(src->location(),dst.location);
    dst.entity_unique_id = *src->entity_unique_id();
}
