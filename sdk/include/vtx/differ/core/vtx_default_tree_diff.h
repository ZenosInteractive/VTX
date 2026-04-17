#pragma once

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// xxh3.h defines XXH_INLINE_ALL internally, which emits all xxhash symbols
// as static inline.  Keep consistent with vtx_types_helpers.h so translation
// units that pull in both headers link cleanly.
#include <xxh3.h>

#include "vtx/differ/core/vtx_diff_types.h"
#include "vtx/differ/core/interfaces/vtx_binary_view_node.h"

namespace VtxDiff {

inline std::span<const std::byte> SafeSubspan(std::span<const std::byte> Data, size_t Start, size_t End) {
    const size_t Max = Data.size();
    Start = std::min(Start, Max); End = std::min(End, Max); End = std::max(End, Start);
    return Data.subspan(Start, End - Start);
}

inline bool SpanEqual(std::span<const std::byte> A, std::span<const std::byte> B) {
    if (A.size() != B.size()) return false;
    if (A.empty()) return true;
    return std::memcmp(A.data(), B.data(), A.size()) == 0;
}

inline bool ReadOffsetU32(std::span<const std::byte> Raw, size_t& Out) {
    if (Raw.size() != sizeof(uint32_t)) return false;
    uint32_t v = 0; std::memcpy(&v, Raw.data(), sizeof(uint32_t)); Out = (size_t)v; return true;
}

inline bool ReadOffsetAny(std::span<const std::byte> Raw, size_t& Out) {
    if (Raw.size() == sizeof(uint32_t)) { uint32_t v; std::memcpy(&v, Raw.data(), sizeof(v)); Out = v; return true; }
    if (Raw.size() == sizeof(uint64_t)) { uint64_t v; std::memcpy(&v, Raw.data(), sizeof(v)); Out = static_cast<size_t>(v); return true; }
    return false;
}

inline uint64_t StableHash64(std::string_view s) {
    uint64_t idHash64 = XXH3_64bits(s.data(), s.size());
    return idHash64;
}

inline bool IsAncestorPath(const VtxDiff::DiffIndexPath& A, const VtxDiff::DiffIndexPath& B) {
    if (A.size() >= B.size()) return false;
    for (size_t i = 0; i < A.size(); ++i) if (A[i] != B[i]) return false;
    return true;
}

template <CBinaryNodeView TNodeView>
class DefaultTreeDiff
{
private:
    struct ActorEntry { uint64_t Hash; size_t Index; };
    mutable std::vector<ActorEntry> ScratchActorsB;
    mutable std::vector<uint8_t> ScratchMatchedB;

    struct MapEntry { uint64_t Hash; std::string Key; size_t Index; };
    mutable std::vector<MapEntry> ScratchMapA;
    mutable std::vector<MapEntry> ScratchMapB;

    static int32_t HashToPathKey(uint64_t Hash) {
        return static_cast<int32_t>(Hash & 0x7FFFFFFF);
    }

    std::optional<uint64_t> TryGetContentHash(const TNodeView& Node) const
    {
        const std::string hash_string = Node.GetScalarFieldString("content_hash");
        if (hash_string.empty()) {
            return std::nullopt;
        }
        return Node.GetUint64Field("content_hash");
    }

    bool HaveMatchingContentHash(const TNodeView& NodeA, const TNodeView& NodeB) const
    {
        const auto hash_a = TryGetContentHash(NodeA);
        const auto hash_b = TryGetContentHash(NodeB);
        return hash_a.has_value() && hash_b.has_value() && (*hash_a == *hash_b);
    }

public:
    DefaultTreeDiff() = default;
    ~DefaultTreeDiff() = default;

    PatchIndex ComputeDiff(const TNodeView& NodeA, const TNodeView& NodeB, const DiffOptions& Opt) const
    {
        PatchIndex Out;
        Out.operations.reserve(512);
        DiffIndexPath root;

        if (!NodeA.IsValid() && !NodeB.IsValid()) return Out;
        if (!NodeA.IsValid() && NodeB.IsValid()) { Out.operations.push_back({ DiffOperation::Add, EVTXContainerType::Unknown, root }); return Out; }
        if (NodeA.IsValid() && !NodeB.IsValid()) { Out.operations.push_back({ DiffOperation::Remove, EVTXContainerType::Unknown, root }); return Out; }

        std::span<const FieldDesc> fields_a = NodeA.EnumerateFields();
        std::span<const FieldDesc> fields_b = NodeB.EnumerateFields();

        const FieldDesc* entities_a = nullptr;
        const FieldDesc* entities_b = nullptr;

        for (const auto& f : fields_a) { if (f.name == "entities") { entities_a = &f; break; } }
        for (const auto& f : fields_b) { if (f.name == "entities") { entities_b = &f; break; } }

        if (entities_a && entities_b)
        {
            root.push_back(static_cast<int32_t>(entities_a->type));
            DiffActorsArray(NodeA, NodeB, *entities_a, *entities_b, Opt, Out, root);
            return Out;
        }

        Diff(NodeA, NodeB, Opt, root, Out);
        return Out;
    }
    
    PatchIndex ComputeEntityDiff(const TNodeView& EntityA, const TNodeView& EntityB,const DiffOptions& Opt, DiffIndexPath BasePath = {}) const
    {
        PatchIndex Out;
        Out.operations.reserve(512);
        if (!EntityA.IsValid() && !EntityB.IsValid()) return Out;
        if (!EntityA.IsValid() && EntityB.IsValid()) { Out.operations.push_back({ DiffOperation::Add, EVTXContainerType::Unknown, BasePath }); return Out; }
        if (EntityA.IsValid() && !EntityB.IsValid()) { Out.operations.push_back({ DiffOperation::Remove, EVTXContainerType::Unknown, BasePath }); return Out; }

        if (HaveMatchingContentHash(EntityA, EntityB)) {
            return Out; 
        }
        
        Diff(EntityA, EntityB, Opt, BasePath, Out);
        return Out;
    }
    

private:
    void Diff(const TNodeView& NodeA, const TNodeView& NodeB, const DiffOptions& Opt, DiffIndexPath& Path, PatchIndex& Out) const
    {
        const bool validA = NodeA.IsValid();
        const bool validB = NodeB.IsValid();

        if (!validA && !validB) return;
        if (!validA && validB) { Out.operations.push_back({ DiffOperation::Add, EVTXContainerType::Unknown, Path }); return; }
        if (validA && !validB) { Out.operations.push_back({ DiffOperation::Remove, EVTXContainerType::Unknown, Path }); return; }

        auto FieldsA = NodeA.EnumerateFields();
        auto FieldsB = NodeB.EnumerateFields();
        
        constexpr uint32_t MAX_ENUM_TYPES = 64; 
        const FieldDesc* ArrA[MAX_ENUM_TYPES] = { nullptr };
        const FieldDesc* ArrB[MAX_ENUM_TYPES] = { nullptr };
        uint32_t maxTypeSeen = 0;

        for (const auto& f : FieldsA) {
            if (f.is_actors_field || f.type == EVTXContainerType::Unknown) continue;
            uint32_t t = static_cast<uint32_t>(f.type);
            if (t < MAX_ENUM_TYPES) { ArrA[t] = &f; if (t > maxTypeSeen) maxTypeSeen = t; }
        }
        for (const auto& f : FieldsB) {
            if (f.is_actors_field || f.type == EVTXContainerType::Unknown) continue;
            uint32_t t = static_cast<uint32_t>(f.type);
            if (t < MAX_ENUM_TYPES) { ArrB[t] = &f; if (t > maxTypeSeen) maxTypeSeen = t; }
        }

        for (uint32_t t = 0; t <= maxTypeSeen; ++t)
        {
            const FieldDesc* Fda = ArrA[t];
            const FieldDesc* Fdb = ArrB[t];
            if (!Fda && !Fdb) continue;

            EVTXContainerType T = static_cast<EVTXContainerType>(t);
            Path.push_back(t);
            
            if (!Fda && Fdb) {
                Out.operations.push_back({ DiffOperation::Add, T, Path });
            } else if (Fda && !Fdb) {
                Out.operations.push_back({ DiffOperation::Remove, T, Path });
            } else if (IsArraysType(T)) {
                DiffEncapsulatedArray(NodeA, NodeB, *Fda, *Fdb, Opt, Out, Path);
            } else {
                switch (T) {
                case EVTXContainerType::BoolProperties:
                case EVTXContainerType::Int32Properties:
                case EVTXContainerType::Int64Properties:
                case EVTXContainerType::FloatProperties:
                case EVTXContainerType::DoubleProperties:
                case EVTXContainerType::StringProperties:
                case EVTXContainerType::TransformProperties:
                case EVTXContainerType::VectorProperties:
                case EVTXContainerType::QuatProperties:
                case EVTXContainerType::RangeProperties:
                    DiffScalarArray(NodeA, NodeB, *Fda, *Fdb, Opt, Out, Path); break;
                case EVTXContainerType::AnyStructProperties:
                    DiffStructArray(NodeA, NodeB, *Fda, *Fdb, Opt, Out, Path); break;
                case EVTXContainerType::MapProperties:
                    DiffMapContainers(NodeA, NodeB, *Fda, *Fdb, Opt, Out, Path); break;
                default: break;
                }
            }
            Path.pop_back();
        }
    }
        
    void DiffActorsArray(const TNodeView& NodeA, const TNodeView& NodeB, const FieldDesc& Fda, const FieldDesc& Fdb, const DiffOptions& Opt, PatchIndex& Out, DiffIndexPath& Path) const
    {
        const size_t CountA = NodeA.GetArraySize(Fda);
        const size_t CountB = NodeB.GetArraySize(Fdb); 
        if (CountA == 0 && CountB == 0) return;

        FieldDesc IdFieldDesc; IdFieldDesc.name = "unique_ids";

        ScratchActorsB.clear();
        ScratchMatchedB.assign(CountB, 0); 

        //check if  actors order has changed, actor order is different in frames a and b
        for (size_t i = 0; i < CountB; ++i) {
            auto spanId = NodeB.GetArrayElementBytes(IdFieldDesc, i);
            if (!spanId.empty()) {
                std::string_view IdView(reinterpret_cast<const char*>(spanId.data()), spanId.size());
                ScratchActorsB.push_back({StableHash64(IdView), i});
            }
        }

        std::sort(ScratchActorsB.begin(), ScratchActorsB.end(), [](const ActorEntry& a, const ActorEntry& b) {
            return a.Hash < b.Hash;
        });

        for (size_t i = 0; i < CountA; ++i) {
            auto spanIdA = NodeA.GetArrayElementBytes(IdFieldDesc, i);
            if (spanIdA.empty()) continue;

            std::string_view IdViewA(reinterpret_cast<const char*>(spanIdA.data()), spanIdA.size());
            uint64_t TargetHash = StableHash64(IdViewA);

            size_t foundIndexB = static_cast<size_t>(-1);
            
            //fast-path, direct position, 99% of the cases  entity 'i' is in the same postion 'i'
            if (i < CountB) {
                auto spanIdB_Direct = NodeB.GetArrayElementBytes(IdFieldDesc, i);
                std::string_view IdViewB_Direct(reinterpret_cast<const char*>(spanIdB_Direct.data()), spanIdB_Direct.size());
                if (IdViewA == IdViewB_Direct) {
                    foundIndexB = i; //found we dont search
                }
            }
            
            //entities are out of order, do a binary search
            if (foundIndexB == static_cast<size_t>(-1)) {
                auto It = std::lower_bound(ScratchActorsB.begin(), ScratchActorsB.end(), TargetHash, 
                    [](const ActorEntry& elem, uint64_t val) { return elem.Hash < val; });
                
                if (It != ScratchActorsB.end() && It->Hash == TargetHash) {
                    auto spanIdB = NodeB.GetArrayElementBytes(IdFieldDesc, It->Index);
                    std::string_view IdB(reinterpret_cast<const char*>(spanIdB.data()), spanIdB.size());
                    if (IdViewA == IdB) {
                        foundIndexB = It->Index;
                    }
                }
            }
            
            //not found in binary search, removed or destroyed
            if (foundIndexB == static_cast<size_t>(-1))
            {
                std::string IdStr(IdViewA);
                Path.push_back(HashToPathKey(TargetHash));
                Out.operations.push_back({ DiffOperation::Remove, EVTXContainerType::AnyStructProperties, Path, IdStr });
                Path.pop_back();
                continue;
            }

            //mark entity B as processed
            ScratchMatchedB[foundIndexB] = 1;

            auto ActorA = NodeA.GetArrayElementAsStruct(Fda, i);   
            auto ActorB = NodeB.GetArrayElementAsStruct(Fdb, foundIndexB);
            if (!ActorA.IsValid() || !ActorB.IsValid()) continue;
            
            //check now content hash

            if (HaveMatchingContentHash(ActorA, ActorB)) {
                continue; //entity is edentical, no need to process all vectors.
            }
            
            const int32_t Key = HashToPathKey(TargetHash);
            Path.push_back(Key);

            size_t OpCountBefore = Out.operations.size();
            Diff(ActorA, ActorB, Opt, Path, Out); 

            if (Out.operations.size() > OpCountBefore) {
                std::string IdStr(IdViewA); 
                Out.actor_id_by_key.try_emplace(Key, IdStr);
                for (size_t opIdx = OpCountBefore; opIdx < Out.operations.size(); ++opIdx) {
                    Out.operations[opIdx].ActorId = IdStr;
                }
            }
            Path.pop_back();
        }

        //process entities added in frame B
        for (size_t i = 0; i < CountB; ++i) {
            if (ScratchMatchedB[i] == 0) {
                auto spanIdB = NodeB.GetArrayElementBytes(IdFieldDesc, i);
                if (spanIdB.empty()) continue;
                std::string_view IdView(reinterpret_cast<const char*>(spanIdB.data()), spanIdB.size());
                std::string IdStr(IdView);
                
                Path.push_back(HashToPathKey(StableHash64(IdView)));
                Out.operations.push_back({ DiffOperation::Add, EVTXContainerType::AnyStructProperties, Path, IdStr });
                Path.pop_back();
            }
        }
    }

    inline bool FastSpanEqual(std::span<const std::byte> A, std::span<const std::byte> B) const {
        if (A.size() != B.size()) return false;
        const size_t sz = A.size();
        if (sz == 0) return true;

        const void* pa = A.data();
        const void* pb = B.data();

        // 4 bytes (Int32, Float)
        if (sz == 4) {
            uint32_t va, vb; 
            std::memcpy(&va, pa, 4); std::memcpy(&vb, pb, 4);
            return va == vb;
        }
        // 8 bytes (Int64, Double, 2x Float)
        if (sz == 8) {
            uint64_t va, vb; 
            std::memcpy(&va, pa, 8); std::memcpy(&vb, pb, 8);
            return va == vb;
        }
        // 12 bytes (Vector: 3x Float) - 64 bits + 32 bits
        if (sz == 12) {
            uint64_t va8, vb8; uint32_t va4, vb4;
            std::memcpy(&va8, pa, 8); std::memcpy(&vb8, pb, 8);
            std::memcpy(&va4, static_cast<const uint8_t*>(pa) + 8, 4); 
            std::memcpy(&vb4, static_cast<const uint8_t*>(pb) + 8, 4);
            return (va8 == vb8) && (va4 == vb4);
        }
        // 16 bytes (Quat, Range, 4x Float) - 128 bits (2x 64 bits)
        if (sz == 16) {
            uint64_t va1, vb1, va2, vb2;
            std::memcpy(&va1, pa, 8); std::memcpy(&vb1, pb, 8);
            std::memcpy(&va2, static_cast<const uint8_t*>(pa) + 8, 8); 
            std::memcpy(&vb2, static_cast<const uint8_t*>(pb) + 8, 8);
            return (va1 == vb1) && (va2 == vb2);
        }

        // Fallback para strings o arrays gigantes
        return std::memcmp(pa, pb, sz) == 0;
    }
    
    void DiffScalarArray(const TNodeView& A, const TNodeView& B, const FieldDesc& Fda, const FieldDesc& Fdb, const DiffOptions& Opt, PatchIndex& Out, const DiffIndexPath& Path) const
    {
        auto full_bytes_a = A.GetFieldBytes(Fda);
        auto full_bytes_b = B.GetFieldBytes(Fdb);
        
        // Fast-path global (Idénticos al 100%)
        if (!full_bytes_a.empty() && full_bytes_a.size() == full_bytes_b.size()) {
            if (std::memcmp(full_bytes_a.data(), full_bytes_b.data(), full_bytes_a.size()) == 0) return; 
        }

        const size_t size_a = A.GetArraySize(Fda);
        const size_t size_b = B.GetArraySize(Fdb);
        const size_t size = std::min(size_a, size_b);

        int32_t current_range_start = -1;
        int32_t current_range_count = 0;
        
        auto FlushRange = [&]() {
            if (current_range_count > 0) {
                if (current_range_count == 1) {
                    Out.operations.push_back({DiffOperation::Replace, Fda.type, Path.Append(current_range_start)});
                } else {
                    DiffIndexOp op = {DiffOperation::ReplaceRange, Fda.type, Path.Append(current_range_start)};
                    op.ReplaceRangeCount = current_range_count;
                    Out.operations.push_back(op);
                }
                current_range_start = -1;
                current_range_count = 0;
            }
        };
        
        bool is_contiguous = !full_bytes_a.empty() && !full_bytes_b.empty() && size_a > 0 && size_b > 0;
        
        if (is_contiguous) {
            const size_t stride_a = full_bytes_a.size() / size_a;
            const size_t stride_b = full_bytes_b.size() / size_b;
            
            if (stride_a == stride_b && stride_a > 0) {
                const uint8_t* ptr_a = reinterpret_cast<const uint8_t*>(full_bytes_a.data());
                const uint8_t* ptr_b = reinterpret_cast<const uint8_t*>(full_bytes_b.data());

                for (size_t i = 0; i < size; ++i) {
                    bool bDifferent = false;

                    if (Opt.compare_floats_with_epsilon) {
                        if (stride_a == sizeof(float)) {
                            float fa, fb;
                            std::memcpy(&fa, ptr_a + (i * stride_a), sizeof(float));
                            std::memcpy(&fb, ptr_b + (i * stride_b), sizeof(float));
                            bDifferent = std::abs(fa - fb) > Opt.float_epsilon;
                        } else if (stride_a == sizeof(double)) {
                            double da, db;
                            std::memcpy(&da, ptr_a + (i * stride_a), sizeof(double));
                            std::memcpy(&db, ptr_b + (i * stride_b), sizeof(double));
                            bDifferent = std::abs(da - db) > static_cast<double>(Opt.float_epsilon);
                        } else {
                            bDifferent = (std::memcmp(ptr_a + (i * stride_a), ptr_b + (i * stride_b), stride_a) != 0);
                        }
                    } else {
                        auto span_a = std::span<const std::byte>(reinterpret_cast<const std::byte*>(ptr_a + (i * stride_a)), stride_a);
                        auto span_b = std::span<const std::byte>(reinterpret_cast<const std::byte*>(ptr_b + (i * stride_b)), stride_a);
                        bDifferent = !FastSpanEqual(span_a, span_b);
                    }

                    if (bDifferent) {
                        if (current_range_start == -1) current_range_start = static_cast<int32_t>(i);
                        ++current_range_count;
                    } else {
                        FlushRange();
                    }
                }
                FlushRange();
                goto EMIT_RESIZES; // Saltamos el bucle lento
            }
        }
        
        for (size_t i = 0; i < size; ++i) {
           auto bytes_a = A.GetArrayElementBytes(Fda, i);
           auto bytes_b = B.GetArrayElementBytes(Fdb, i);

           if (bytes_a.empty() && bytes_b.empty()) { FlushRange(); continue; }

           bool bDifferent = false;
           if (Opt.compare_floats_with_epsilon && bytes_a.size() == bytes_b.size()) {
              if (bytes_a.size() == sizeof(float)) {
                 float fa, fb; std::memcpy(&fa, bytes_a.data(), sizeof(float)); std::memcpy(&fb, bytes_b.data(), sizeof(float));
                 bDifferent = std::abs(fa - fb) > Opt.float_epsilon;
              } else if (bytes_a.size() == sizeof(double)) {
                 double da, db; std::memcpy(&da, bytes_a.data(), sizeof(double)); std::memcpy(&db, bytes_b.data(), sizeof(double));
                 bDifferent = std::abs(da - db) > static_cast<double>(Opt.float_epsilon);
              } else {
                 bDifferent = !FastSpanEqual(bytes_a, bytes_b);
              }
           } else {
              bDifferent = !FastSpanEqual(bytes_a, bytes_b);
           }

           if (bDifferent) {
               if (current_range_start == -1) current_range_start = static_cast<int32_t>(i);
               ++current_range_count;
           } else {
              FlushRange();
           }
        }
        FlushRange();

EMIT_RESIZES:
        for (size_t i = size; i < size_b; ++i) {
            Out.operations.push_back({ DiffOperation::Add, Fda.type, Path.Append(static_cast<int32_t>(i)) });
        }
        
        for (size_t i = size; i < size_a; ++i) {
            Out.operations.push_back({ DiffOperation::Remove, Fda.type, Path.Append(static_cast<int32_t>(i)) });
        }
    }

    void DiffStructArray(const TNodeView& A, const TNodeView& B, const FieldDesc& Fda, const FieldDesc& Fdb, const DiffOptions& Opt, PatchIndex& Out, DiffIndexPath& Path) const
    {
        auto full_bytes_a = A.GetFieldBytes(Fda);
        auto full_bytes_b = B.GetFieldBytes(Fdb);
        
        if (!full_bytes_a.empty() && full_bytes_a.size() == full_bytes_b.size()) {
            if (std::memcmp(full_bytes_a.data(), full_bytes_b.data(), full_bytes_a.size()) == 0) return; 
        }

        const size_t size_a = A.GetArraySize(Fda);
        const size_t size_b = B.GetArraySize(Fdb);

        if (size_a != size_b) {
            Out.operations.push_back({ DiffOperation::Replace, Fda.type, Path });
            return;
        }

        for (size_t i = 0; i < size_a; ++i) {
            auto node_a = A.GetArrayElementAsStruct(Fda, i);
            auto node_b = B.GetArrayElementAsStruct(Fdb, i);
            Path.push_back(static_cast<int32_t>(i));
            
            if (!node_a.IsValid() || !node_b.IsValid()) {
                Out.operations.push_back({ DiffOperation::Replace, Fda.type, Path });
            } else
            {
                //use fast-path with content hash to omit identical structs
                if (HaveMatchingContentHash(node_a, node_b)) {
                    //structs are identical, omit
                } else {
                    Diff(node_a, node_b, Opt, Path, Out);
                }
            }
            Path.pop_back();
        }
    }

    void DiffFlatRepeatedByOffsets(const TNodeView& A, const TNodeView& B, const FieldDesc& FlatFda, const FieldDesc& FlatFdb, const FieldDesc& OffFda, const FieldDesc& OffFdb, const DiffOptions& Opt, PatchIndex& Out, DiffIndexPath& Path) const
    {
        const size_t OffCountA = A.GetArraySize(OffFda);
        const size_t OffCountB = B.GetArraySize(OffFdb);
        const size_t N = std::min(OffCountA, OffCountB);

        for (size_t i = 0; i < N; ++i) {
            size_t StartA = 0, StartB = 0;
            bool bValidOffsets = true;
            bValidOffsets &= ReadOffsetU32(A.GetArrayElementBytes(OffFda, i), StartA);
            bValidOffsets &= ReadOffsetU32(B.GetArrayElementBytes(OffFdb, i), StartB);

            if (!bValidOffsets) {
                Out.operations.push_back({ DiffOperation::Replace, FlatFda.type, Path.Append(static_cast<int32_t>(i)) });
                continue;
            }

            size_t EndA = A.GetArraySize(FlatFda);
            size_t EndB = B.GetArraySize(FlatFdb);
            if (i + 1 < OffCountA) { size_t Next = 0; if (ReadOffsetU32(A.GetArrayElementBytes(OffFda, i + 1), Next)) EndA = Next; }
            if (i + 1 < OffCountB) { size_t Next = 0; if (ReadOffsetU32(B.GetArrayElementBytes(OffFdb, i + 1), Next)) EndB = Next; }

            if (EndA < StartA) EndA = StartA;
            if (EndB < StartB) EndB = StartB;

            const size_t LenA = EndA - StartA;
            const size_t LenB = EndB - StartB;

            if (LenA != LenB) {
                Out.operations.push_back({ DiffOperation::Replace, FlatFda.type, Path.Append(static_cast<int32_t>(i)) });
                continue;
            }

            bool bSubArrayChanged = false;
            if (FlatFda.type == EVTXContainerType::AnyStructArrays) {
                for (size_t j = 0; j < LenA; ++j) {
                    auto NodeElemA = A.GetArrayElementAsStruct(FlatFda, StartA + j);
                    auto NodeElemB = B.GetArrayElementAsStruct(FlatFdb, StartB + j);
                    if (!NodeElemA.IsValid() || !NodeElemB.IsValid()) { bSubArrayChanged = true; break; }
                    
                    size_t before = Out.operations.size();
                    Diff(NodeElemA, NodeElemB, Opt, Path, Out);
                    if (Out.operations.size() > before) {
                        Out.operations.resize(before);
                        bSubArrayChanged = true; break;
                    }
                }
            } else {
                for (size_t j = 0; j < LenA; ++j) {
                    auto EA = A.GetArrayElementBytes(FlatFda, StartA + j);
                    auto EB = B.GetArrayElementBytes(FlatFdb, StartB + j);
                    if (!AreScalarsEqual(FlatFda.type, EA, EB, Opt)) { bSubArrayChanged = true; break; }
                }
            }

            if (bSubArrayChanged) {
                Out.operations.push_back({ DiffOperation::Replace, FlatFda.type, Path.Append(static_cast<int32_t>(i)) });
            }
        }

        for (size_t i = N; i < OffCountA; ++i) { Out.operations.push_back({ DiffOperation::Remove, FlatFda.type, Path.Append(static_cast<int32_t>(i)) }); }
        for (size_t i = N; i < OffCountB; ++i) { Out.operations.push_back({ DiffOperation::Replace, FlatFda.type, Path.Append(static_cast<int32_t>(i)) }); }
    }

    void DiffEncapsulatedArray(const TNodeView& ParentA, const TNodeView& ParentB, const FieldDesc& WrapperFda, const FieldDesc& WrapperFdb, const DiffOptions& Opt, PatchIndex& Out, DiffIndexPath& Path) const
    {
        auto wrap_a = ParentA.GetNestedStruct(WrapperFda);
        auto wrap_b = ParentB.GetNestedStruct(WrapperFdb);

        if (!wrap_a.IsValid() && !wrap_b.IsValid()) return;
        if (!wrap_a.IsValid() && wrap_b.IsValid()) { Out.operations.push_back({ DiffOperation::Add, WrapperFda.type, Path }); return; }
        if (wrap_a.IsValid() && !wrap_b.IsValid()) { Out.operations.push_back({ DiffOperation::Remove, WrapperFda.type, Path }); return; }

        FieldDesc field_desc; field_desc.name = "offsets";
        FieldDesc data_desc; data_desc.name = "data"; 

        bool b_is_recursive = (WrapperFda.type == EVTXContainerType::AnyStructArrays);

        const size_t off_count_a = wrap_a.GetArraySize(field_desc);
        const size_t off_count_b = wrap_b.GetArraySize(field_desc);
        const size_t size = std::min(off_count_a, off_count_b);

        for (size_t i = 0; i < size; ++i) {
            size_t start_a = 0, start_b = 0;
            bool b_valid = true;
            b_valid &= ReadOffsetAny(wrap_a.GetArrayElementBytes(field_desc, i), start_a);
            b_valid &= ReadOffsetAny(wrap_b.GetArrayElementBytes(field_desc, i), start_b);

            if (!b_valid) {
                Out.operations.push_back({ DiffOperation::Replace, WrapperFda.type, Path.Append(static_cast<int32_t>(i)) });
                continue;
            }

            size_t end_a = wrap_a.GetArraySize(data_desc);
            size_t end_b = wrap_b.GetArraySize(data_desc);

            if (i + 1 < off_count_a) { size_t Next = 0; if (ReadOffsetAny(wrap_a.GetArrayElementBytes(field_desc, i + 1), Next)) end_a = Next; }
            if (i + 1 < off_count_b) { size_t Next = 0; if (ReadOffsetAny(wrap_b.GetArrayElementBytes(field_desc, i + 1), Next)) end_b = Next; }

            if (end_a < start_a) end_a = start_a;
            if (end_b < start_b) end_b = start_b;

            const size_t len_a = end_a - start_a;
            const size_t len_b = end_b - start_b;

            if (len_a != len_b) {
                Out.operations.push_back({ DiffOperation::Replace, WrapperFda.type, Path.Append(static_cast<int32_t>(i)) });
                continue;
            }

            bool b_changed = false;
            if (b_is_recursive) {
                for (size_t j = 0; j < len_a; ++j) {
                    auto node_a = wrap_a.GetArrayElementAsStruct(data_desc, start_a + j);
                    auto node_b = wrap_b.GetArrayElementAsStruct(data_desc, start_b + j);
                    if (!node_a.IsValid() || !node_b.IsValid()) { b_changed = true; break; }

                    //use fast-path with content hash to omit identical structs
                    if (HaveMatchingContentHash(node_a, node_b)) {
                        //structs are identical, omit
                    } else {
                        //we surely know something has changed, we can omit the full diff
                        b_changed = true; 
                        break;
                    }
                }
            } else {
                auto block_a = wrap_a.GetSubArrayBytes(WrapperFda, i);
                auto block_b = wrap_b.GetSubArrayBytes(WrapperFdb, i);

                if (!block_a.empty() && block_a.size() == block_b.size()) {
                    if (std::memcmp(block_a.data(), block_b.data(), block_a.size()) == 0) continue;
                }

                for (size_t j = 0; j < len_a; ++j) {
                    auto bytes_a = wrap_a.GetArrayElementBytes(data_desc, start_a + j);
                    auto bytes_b = wrap_b.GetArrayElementBytes(data_desc, start_b + j);
                    if (!AreScalarsEqual(WrapperFda.type, bytes_a, bytes_b, Opt)) { b_changed = true; break; }
                }
            }

            if (b_changed) {
                Out.operations.push_back({ DiffOperation::Replace, WrapperFda.type, Path.Append(static_cast<int32_t>(i)) });
            }
        }

        for (size_t i = size; i < off_count_a; ++i) { Out.operations.push_back({ DiffOperation::Remove, WrapperFda.type, Path.Append(static_cast<int32_t>(i)) }); }
        for (size_t i = size; i < off_count_b; ++i) { Out.operations.push_back({ DiffOperation::Replace, WrapperFda.type, Path.Append(static_cast<int32_t>(i)) }); }
    }

    void DiffMapContainers(const TNodeView& A, const TNodeView& B, const FieldDesc& Fda, const FieldDesc& Fdb, const DiffOptions& Opt, PatchIndex& Out, DiffIndexPath& Path) const
    {
        const size_t size_a = A.GetMapSize(Fda);
        const size_t size_b = B.GetMapSize(Fdb);
        if (size_a == 0 && size_b == 0) return;
        
        ScratchMapA.clear(); ScratchMapB.clear();
        for (size_t i = 0; i < size_a; ++i) {
            std::string k = A.GetMapKey(Fda, i);
            ScratchMapA.push_back({StableHash64(k), std::move(k), i});
        }
        for (size_t i = 0; i < size_b; ++i) {
            std::string k = B.GetMapKey(Fdb, i);
            ScratchMapB.push_back({StableHash64(k), std::move(k), i});
        }

        std::sort(ScratchMapA.begin(), ScratchMapA.end(), [](const auto& a, const auto& b){ return a.Hash < b.Hash; });
        std::sort(ScratchMapB.begin(), ScratchMapB.end(), [](const auto& a, const auto& b){ return a.Hash < b.Hash; });

        size_t idx_a = 0, idx_b = 0;
        while(idx_a < ScratchMapA.size() || idx_b < ScratchMapB.size()) {
            if (idx_a < ScratchMapA.size() && (idx_b == ScratchMapB.size() || ScratchMapA[idx_a].Hash < ScratchMapB[idx_b].Hash)) {
                Path.push_back(HashToPathKey(ScratchMapA[idx_a].Hash));
                Out.operations.push_back({ DiffOperation::Remove, Fda.type, Path, {}, ScratchMapA[idx_a].Key });
                Path.pop_back();
                idx_a++;
            }
            else if (idx_b < ScratchMapB.size() && (idx_a == ScratchMapA.size() || ScratchMapB[idx_b].Hash < ScratchMapA[idx_a].Hash)) {
                Path.push_back(HashToPathKey(ScratchMapB[idx_b].Hash));
                Out.operations.push_back({ DiffOperation::Add, Fda.type, Path, {}, ScratchMapB[idx_b].Key });
                Path.pop_back();
                idx_b++;
            }
            else {
                if (ScratchMapA[idx_a].Key == ScratchMapB[idx_b].Key) {
                    auto node_a = A.GetMapValueAsStruct(Fda, ScratchMapA[idx_a].Index);
                    auto node_b = B.GetMapValueAsStruct(Fdb, ScratchMapB[idx_b].Index);
                    if (node_a.IsValid() && node_b.IsValid()) {
                        if (HaveMatchingContentHash(node_a, node_b)) {
                            //structs are identical, omit
                        } else {
                            Path.push_back(HashToPathKey(ScratchMapA[idx_a].Hash));
                            const size_t OpCountBefore = Out.operations.size();
                            Diff(node_a, node_b, Opt, Path, Out);
                            for (size_t OpIdx = OpCountBefore; OpIdx < Out.operations.size(); ++OpIdx) {
                                Out.operations[OpIdx].MapKey = ScratchMapA[idx_a].Key;
                            }
                            Path.pop_back();
                        }
                    }
                } else {
                    Path.push_back(HashToPathKey(ScratchMapA[idx_a].Hash));
                    Out.operations.push_back({ DiffOperation::Remove, Fda.type, Path, {}, ScratchMapA[idx_a].Key });
                    Path.pop_back();
                    
                    Path.push_back(HashToPathKey(ScratchMapB[idx_b].Hash));
                    Out.operations.push_back({ DiffOperation::Add, Fda.type, Path, {}, ScratchMapB[idx_b].Key });
                    Path.pop_back();
                }
                idx_a++; idx_b++;
            }
        }
    }
};

} // namespace VtxDiff
