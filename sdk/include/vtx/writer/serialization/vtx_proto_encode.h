#pragma once
/**
 * @file vtx_proto_encode.h
 * @brief A native frame straight to its Protobuf wire bytes, without building the cppvtx messages.
 *
 * @details EncodeFrame writes, byte for byte, what
 *          `ProtobufVtxPolicy::FromNative(std::move(frame))->SerializeToString()` writes: every bucket in
 *          type order (SortBucketByTypeId's order and type_ranges), every container's content hash
 *          (Helpers::StoreContainerHashes), and each field emitted exactly when the generated serializer
 *          emits it -- fields in number order; a scalar only when non-zero (a float or double by its bits,
 *          so -0.0 is written); repeated scalars packed; a sub-message exactly when ToProto creates it.
 *          The frame is read in place and left as it was, but for the content hashes it stores, so a
 *          writer can refill it for its next frame.
 *
 *          Nested messages are written into one reusable buffer per depth and copied into their parent
 *          once, prefixed by their length: no message object, and no allocation once the buffers have
 *          grown to a frame's size.
 */
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"

#include <algorithm>

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace VTX {
    namespace Serialization {
        namespace wire {

            enum : uint32_t { kVarint = 0, kFixed64 = 1, kLengthDelimited = 2, kFixed32 = 5 };

            inline void Varint(std::string& out, uint64_t value) {
                char buffer[10];
                size_t n = 0;
                while (value >= 0x80) {
                    buffer[n++] = static_cast<char>((value & 0x7F) | 0x80);
                    value >>= 7;
                }
                buffer[n++] = static_cast<char>(value);
                out.append(buffer, n);
            }

            inline void Tag(std::string& out, uint32_t field, uint32_t type) {
                Varint(out, (field << 3) | type);
            }

            inline uint64_t ZigZag32(int32_t v) {
                return static_cast<uint32_t>((static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31));
            }
            inline uint64_t ZigZag64(int64_t v) {
                return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
            }

            template <typename T>
            void Fixed(std::string& out, T value) {
                char bytes[sizeof(T)];
                std::memcpy(bytes, &value, sizeof(T)); // little-endian, as protobuf's fixed encodings
                out.append(bytes, sizeof(T));
            }

            // A singular int32 / uint64 field, omitted at zero (proto3).
            inline void Int32Field(std::string& out, uint32_t field, int32_t v) {
                if (v != 0) {
                    Tag(out, field, kVarint);
                    Varint(out, static_cast<uint64_t>(static_cast<int64_t>(v))); // negative: sign-extended
                }
            }
            inline void UInt64Field(std::string& out, uint32_t field, uint64_t v) {
                if (v != 0) {
                    Tag(out, field, kVarint);
                    Varint(out, v);
                }
            }
            // A singular float / double, omitted only when its bits are zero (the generated serializer's test).
            inline void FloatField(std::string& out, uint32_t field, float v) {
                uint32_t raw;
                std::memcpy(&raw, &v, sizeof(raw));
                if (raw != 0) {
                    Tag(out, field, kFixed32);
                    Fixed(out, v);
                }
            }
            inline void DoubleField(std::string& out, uint32_t field, double v) {
                uint64_t raw;
                std::memcpy(&raw, &v, sizeof(raw));
                if (raw != 0) {
                    Tag(out, field, kFixed64);
                    Fixed(out, v);
                }
            }

            inline void BytesField(std::string& out, uint32_t field, const void* data, size_t size) {
                Tag(out, field, kLengthDelimited);
                Varint(out, size);
                out.append(static_cast<const char*>(data), size);
            }

            // A length-delimited sub-message whose bytes are already in `body`.
            inline void Embed(std::string& out, uint32_t field, const std::string& body) {
                BytesField(out, field, body.data(), body.size());
            }

            // Packed repeated fields, omitted when empty.
            template <typename T>
            void PackedFixed(std::string& out, uint32_t field, const std::vector<T>& values) {
                if (values.empty())
                    return;
                Tag(out, field, kLengthDelimited);
                Varint(out, values.size() * sizeof(T));
                out.append(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(T));
            }

            template <typename Values, typename Encode>
            void PackedVarint(std::string& out, uint32_t field, const Values& values, Encode encode,
                              std::string& scratch) {
                if (values.empty())
                    return;
                scratch.clear();
                for (const auto& v : values)
                    Varint(scratch, encode(v));
                Embed(out, field, scratch);
            }

        } // namespace wire

        /// @brief Encodes native frames as Protobuf bytes; one per writer, its buffers reused frame after frame.
        class ProtoFrameEncoder {
        public:
            /// @brief `frame`'s cppvtx::Frame bytes into `out` (replaced); stores every container's content hash.
            size_t EncodeFrame(VTX::Frame& frame, std::string& out) {
                out.clear();
                for (VTX::Bucket& bucket : frame.GetMutableBuckets()) {
                    std::string& body = Buffer(0);
                    EncodeBucket(bucket, body);
                    wire::Embed(out, 1, body);
                }
                return out.size();
            }

        private:
            // One per nesting depth. A deque, so growing it never moves a buffer a caller is still writing.
            std::deque<std::string> buffers_;
            std::vector<size_t> order_;       // a bucket's entities in type order
            std::vector<EntityRange> ranges_; // and its type ranges
            std::vector<std::vector<size_t>> by_type_;

            std::string& Buffer(size_t depth) {
                if (buffers_.size() <= depth)
                    buffers_.resize(depth + 1);
                buffers_[depth].clear();
                return buffers_[depth];
            }

            // The order and type ranges SortBucketByTypeId gives the bucket; false when it copies the bucket
            // as it is (no entities, or none with a type), its own ranges then kept.
            bool TypeOrder(const VTX::Bucket& bucket) {
                order_.clear();
                ranges_.clear();
                const auto& entities = bucket.entities;
                int32_t max_type = -1;
                for (const auto& e : entities)
                    max_type = std::max(e.entity_type_id, max_type);
                if (entities.empty() || max_type < 0)
                    return false;
                if (by_type_.size() < static_cast<size_t>(max_type) + 1)
                    by_type_.resize(static_cast<size_t>(max_type) + 1);
                for (int32_t t = 0; t <= max_type; ++t)
                    by_type_[static_cast<size_t>(t)].clear();
                for (size_t i = 0; i < entities.size(); ++i) {
                    const int32_t t = entities[i].entity_type_id;
                    if (t >= 0)
                        by_type_[static_cast<size_t>(t)].push_back(i);
                }
                ranges_.assign(static_cast<size_t>(max_type) + 1, {0, 0});
                int32_t at = 0;
                for (int32_t t = 0; t <= max_type; ++t) {
                    const auto& indices = by_type_[static_cast<size_t>(t)];
                    ranges_[static_cast<size_t>(t)] = {at, static_cast<int32_t>(indices.size())};
                    order_.insert(order_.end(), indices.begin(), indices.end());
                    at += static_cast<int32_t>(indices.size());
                }
                return true;
            }

            void EncodeBucket(VTX::Bucket& bucket, std::string& out) {
                const bool sorted = TypeOrder(bucket);
                // 1: unique_ids, in the entities' order; an entity past the ids has none.
                if (sorted) {
                    for (size_t i : order_)
                        if (i < bucket.unique_ids.size())
                            wire::BytesField(out, 1, bucket.unique_ids[i].data(), bucket.unique_ids[i].size());
                } else {
                    for (const auto& id : bucket.unique_ids)
                        wire::BytesField(out, 1, id.data(), id.size());
                }
                // 2: entities.
                if (sorted) {
                    for (size_t i : order_)
                        EncodeEntity(bucket.entities[i], out);
                } else {
                    for (auto& entity : bucket.entities)
                        EncodeEntity(entity, out);
                }
                // 3: type_ranges.
                const auto& ranges = sorted ? ranges_ : bucket.type_ranges;
                for (const EntityRange& range : ranges) {
                    std::string& body = Buffer(1);
                    wire::Int32Field(body, 1, range.start_index);
                    wire::Int32Field(body, 2, range.count);
                    wire::Embed(out, 3, body);
                }
            }

            void EncodeEntity(VTX::PropertyContainer& entity, std::string& out) {
                VTX::Helpers::StoreContainerHashes(entity);
                std::string& body = Buffer(1);
                EncodeContainer(entity, body, 1);
                wire::Embed(out, 2, body);
            }

            static void EncodeVector(const VTX::Vector& v, std::string& out) {
                wire::DoubleField(out, 1, v.x);
                wire::DoubleField(out, 2, v.y);
                wire::DoubleField(out, 3, v.z);
            }
            static void EncodeQuat(const VTX::Quat& q, std::string& out) {
                wire::FloatField(out, 1, q.x);
                wire::FloatField(out, 2, q.y);
                wire::FloatField(out, 3, q.z);
                wire::FloatField(out, 4, q.w);
            }
            static void EncodeRange(const VTX::FloatRange& r, std::string& out) {
                wire::FloatField(out, 1, r.min);
                wire::FloatField(out, 2, r.max);
                wire::FloatField(out, 3, r.value_normalized);
            }
            void EncodeTransform(const VTX::Transform& t, std::string& out, size_t depth) {
                // ToProto always creates all three sub-messages, so each is written even when empty.
                std::string& part = Buffer(depth);
                EncodeVector(t.translation, part);
                wire::Embed(out, 1, part);
                std::string& rotation = Buffer(depth);
                EncodeQuat(t.rotation, rotation);
                wire::Embed(out, 2, rotation);
                std::string& scale = Buffer(depth);
                EncodeVector(t.scale, scale);
                wire::Embed(out, 3, scale);
            }

            // One element message per value, written with `encode` into the next depth's buffer.
            template <typename T, typename Encode>
            void Messages(std::string& out, uint32_t field, const std::vector<T>& values, size_t depth, Encode encode) {
                for (const T& value : values) {
                    std::string& body = Buffer(depth);
                    encode(value, body);
                    wire::Embed(out, field, body);
                }
            }

            // A Flat*Array sub-message: present exactly when its data is not empty (ToProto's test).
            template <typename WriteData>
            void FlatArray(std::string& out, uint32_t field, bool has_data, const std::vector<uint32_t>& offsets,
                           size_t depth, WriteData write_data) {
                if (!has_data)
                    return;
                std::string& body = Buffer(depth);
                write_data(body, depth + 1);
                wire::PackedVarint(
                    body, 2, offsets, [](uint32_t o) { return static_cast<uint64_t>(o); }, Buffer(depth + 1));
                wire::Embed(out, field, body);
            }

            void EncodeMap(const VTX::MapContainer& map, std::string& out, size_t depth) {
                for (const auto& key : map.keys)
                    wire::BytesField(out, 1, key.data(), key.size());
                for (const auto& value : map.values) {
                    std::string& body = Buffer(depth);
                    EncodeContainer(value, body, depth + 1);
                    wire::Embed(out, 2, body);
                }
            }

            // A container whose content hashes are stored (StoreContainerHashes), as ToProtoHashed writes it.
            void EncodeContainer(const VTX::PropertyContainer& c, std::string& out, size_t depth) {
                using namespace wire;
                const size_t next = depth + 1;
                Int32Field(out, 1, c.entity_type_id);
                if (!c.bool_properties.empty()) {
                    Tag(out, 2, kLengthDelimited);
                    Varint(out, c.bool_properties.size());
                    for (bool b : c.bool_properties)
                        out.push_back(b ? 1 : 0);
                }
                PackedVarint(
                    out, 3, c.int32_properties, [](int32_t v) { return ZigZag32(v); }, Buffer(next));
                PackedVarint(
                    out, 4, c.int64_properties, [](int64_t v) { return ZigZag64(v); }, Buffer(next));
                PackedFixed(out, 5, c.float_properties);
                PackedFixed(out, 6, c.double_properties);
                for (const auto& s : c.string_properties)
                    BytesField(out, 7, s.data(), s.size());
                Messages(out, 8, c.transform_properties, next,
                         [this, next](const VTX::Transform& t, std::string& b) { EncodeTransform(t, b, next + 1); });
                Messages(out, 9, c.vector_properties, next,
                         [](const VTX::Vector& v, std::string& b) { EncodeVector(v, b); });
                Messages(out, 10, c.quat_properties, next,
                         [](const VTX::Quat& q, std::string& b) { EncodeQuat(q, b); });
                Messages(out, 11, c.range_properties, next,
                         [](const VTX::FloatRange& r, std::string& b) { EncodeRange(r, b); });

                // 12: byte_array_properties -- its whole data as ONE bytes element, as ToProto adds it.
                FlatArray(out, 12, !c.byte_array_properties.data.empty(), c.byte_array_properties.offsets, next,
                          [&c](std::string& b, size_t) {
                              BytesField(b, 1, c.byte_array_properties.data.data(),
                                         c.byte_array_properties.data.size());
                          });
                FlatArray(out, 13, !c.int32_arrays.data.empty(), c.int32_arrays.offsets, next,
                          [this, &c](std::string& b, size_t d) {
                              PackedVarint(
                                  b, 1, c.int32_arrays.data, [](int32_t v) { return ZigZag32(v); }, Buffer(d));
                          });
                FlatArray(out, 14, !c.int64_arrays.data.empty(), c.int64_arrays.offsets, next,
                          [this, &c](std::string& b, size_t d) {
                              PackedVarint(
                                  b, 1, c.int64_arrays.data, [](int64_t v) { return ZigZag64(v); }, Buffer(d));
                          });
                FlatArray(out, 15, !c.float_arrays.data.empty(), c.float_arrays.offsets, next,
                          [&c](std::string& b, size_t) { PackedFixed(b, 1, c.float_arrays.data); });
                FlatArray(out, 16, !c.double_arrays.data.empty(), c.double_arrays.offsets, next,
                          [&c](std::string& b, size_t) { PackedFixed(b, 1, c.double_arrays.data); });
                FlatArray(out, 17, !c.vector_arrays.data.empty(), c.vector_arrays.offsets, next,
                          [this, &c](std::string& b, size_t d) {
                              Messages(b, 1, c.vector_arrays.data, d,
                                       [](const VTX::Vector& v, std::string& m) { EncodeVector(v, m); });
                          });
                FlatArray(out, 18, !c.quat_arrays.data.empty(), c.quat_arrays.offsets, next,
                          [this, &c](std::string& b, size_t d) {
                              Messages(b, 1, c.quat_arrays.data, d,
                                       [](const VTX::Quat& q, std::string& m) { EncodeQuat(q, m); });
                          });
                FlatArray(out, 19, !c.transform_arrays.data.empty(), c.transform_arrays.offsets, next,
                          [this, &c](std::string& b, size_t d) {
                              Messages(
                                  b, 1, c.transform_arrays.data, d,
                                  [this, d](const VTX::Transform& t, std::string& m) { EncodeTransform(t, m, d + 1); });
                          });
                FlatArray(out, 20, !c.range_arrays.data.empty(), c.range_arrays.offsets, next,
                          [this, &c](std::string& b, size_t d) {
                              Messages(b, 1, c.range_arrays.data, d,
                                       [](const VTX::FloatRange& r, std::string& m) { EncodeRange(r, m); });
                          });
                // 21: bool_arrays -- packed bools, each stored byte as 0 or 1.
                FlatArray(out, 21, !c.bool_arrays.data.empty(), c.bool_arrays.offsets, next,
                          [&c](std::string& b, size_t) {
                              Tag(b, 1, kLengthDelimited);
                              Varint(b, c.bool_arrays.data.size());
                              for (uint8_t v : c.bool_arrays.data)
                                  b.push_back(v != 0 ? 1 : 0);
                          });
                FlatArray(out, 22, !c.string_arrays.data.empty(), c.string_arrays.offsets, next,
                          [&c](std::string& b, size_t) {
                              for (const auto& s : c.string_arrays.data)
                                  BytesField(b, 1, s.data(), s.size());
                          });
                for (const auto& child : c.any_struct_properties) {
                    std::string& body = Buffer(next);
                    EncodeContainer(child, body, next + 1);
                    Embed(out, 23, body);
                }
                FlatArray(out, 24, !c.any_struct_arrays.data.empty(), c.any_struct_arrays.offsets, next,
                          [this, &c](std::string& b, size_t d) {
                              for (const auto& child : c.any_struct_arrays.data) {
                                  std::string& body = Buffer(d);
                                  EncodeContainer(child, body, d + 1);
                                  Embed(b, 1, body);
                              }
                          });
                for (const auto& map : c.map_properties) {
                    std::string& body = Buffer(next);
                    EncodeMap(map, body, next + 1);
                    Embed(out, 25, body);
                }
                FlatArray(out, 26, !c.map_arrays.data.empty(), c.map_arrays.offsets, next,
                          [this, &c](std::string& b, size_t d) {
                              for (const auto& map : c.map_arrays.data) {
                                  std::string& body = Buffer(d);
                                  EncodeMap(map, body, d + 1);
                                  Embed(b, 1, body);
                              }
                          });
                UInt64Field(out, 27, c.content_hash);
            }
        };

    } // namespace Serialization
} // namespace VTX
