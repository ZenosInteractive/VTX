#pragma once

#include <limits>

#include "vtx/common/vtx_concepts.h"
#include "vtx/common/vtx_logger.h"
#include "vtx/differ/core/interfaces/vtx_binary_view_node.h"
#include "vtx/differ/core/vtx_default_tree_diff.h"
#include "vtx/differ/core/vtx_diff_types.h"

namespace DiffUtils
{
    template<typename TNodeView>
    VtxDiff::PatchIndex DiffSingleActorByIndex(const TNodeView& FrameA, const TNodeView& FrameB, uint32_t ActorIndexA, uint32_t ActorIndexB, const VtxDiff::DiffOptions& Opt)
    {
        VtxDiff::FieldDesc entities_field;
        entities_field.name = "entities";
        entities_field.type = VtxDiff::EVTXContainerType::AnyStructArrays;

        auto entity_a = FrameA.GetArrayElementAsStruct(entities_field, ActorIndexA);
        auto entity_b = FrameB.GetArrayElementAsStruct(entities_field, ActorIndexB);

        VtxDiff::DefaultTreeDiff<TNodeView> differ;
        return differ.ComputeEntityDiff(entity_a, entity_b, Opt);
    }
}

namespace VtxDiff {

    template <typename TNodeView>
    requires CBinaryNodeView<TNodeView>
    class PatchAccessor {
    public:
        static TNodeView GetTargetNode(const TNodeView& Root, const DiffIndexPath& Path) {
            return ResolvePath(Root, Path, {}).Node;
        }

        static bool Exists(const TNodeView& Root, const DiffIndexOp& Op) {
            return ResolvePath(Root, Op.Path, Op.MapKey).Exists;
        }

        static std::span<const std::byte> GetRawBytes(const TNodeView& Root, const DiffIndexOp& Op) {
            return ResolvePath(Root, Op.Path, Op.MapKey).Bytes;
        }

    private:
        static constexpr size_t InvalidIndex() {
            return std::numeric_limits<size_t>::max();
        }

        struct ResolvedPath {
            bool Exists = false;
            std::span<const std::byte> Bytes{};
            TNodeView Node{};
        };

        static FieldDesc MakeField(EVTXContainerType Type) {
            FieldDesc fd;
            fd.name = TypeToFieldName(Type);
            fd.type = Type;
            fd.is_array_like = IsArraysType(Type);
            fd.is_map_like = (Type == EVTXContainerType::MapProperties || Type == EVTXContainerType::MapArrays);
            return fd;
        }

        static bool HasField(const TNodeView& Node, const FieldDesc& Field) {
            for (const auto& Candidate : Node.EnumerateFields()) {
                if (Candidate.name == Field.name && Candidate.type == Field.type) {
                    return true;
                }
            }
            return false;
        }

        static size_t FindActorIndexByHash(const TNodeView& Node, int32_t TargetHash) {
            FieldDesc entities_field;
            entities_field.name = "entities";
            entities_field.type = EVTXContainerType::AnyStructArrays;

            FieldDesc id_field;
            id_field.name = "unique_ids";

            const size_t count = Node.GetArraySize(entities_field);
            for (size_t i = 0; i < count; ++i) {
                auto actor = Node.GetArrayElementAsStruct(entities_field, i);
                if (!actor.IsValid()) continue;

                auto span_id = actor.GetFieldBytes(id_field);
                if (span_id.empty()) continue;

                const int32_t current_hash = static_cast<int32_t>(VtxDiff::StableHash64(
                    { reinterpret_cast<const char*>(span_id.data()), span_id.size() }
                ) & 0x7FFFFFFF);

                if (current_hash == TargetHash) {
                    return i;
                }
            }
            return InvalidIndex();
        }

        static size_t FindMapIndexByHash(const TNodeView& Node, const FieldDesc& Field, int32_t TargetHash, const std::string& ExpectedKey) {
            const size_t count = Node.GetMapSize(Field);
            for (size_t i = 0; i < count; ++i) {
                const std::string key = Node.GetMapKey(Field, i);
                if (key.empty()) continue;

                const int32_t key_hash = static_cast<int32_t>(VtxDiff::StableHash64(key) & 0x7FFFFFFF);
                if (key_hash != TargetHash) continue;
                if (!ExpectedKey.empty() && key != ExpectedKey) continue;
                return i;
            }
            return InvalidIndex();
        }

        static ResolvedPath ResolvePath(const TNodeView& Root, const DiffIndexPath& Path, const std::string& MapKey) {
            if (!Root.IsValid()) {
                return {};
            }

            if (Path.size() == 0) {
                return { true, {}, Root };
            }

            if (Path[0] == static_cast<int32_t>(EVTXContainerType::AnyStructArrays) && Path.size() >= 2) {
                FieldDesc entities_field;
                entities_field.name = "entities";
                entities_field.type = EVTXContainerType::AnyStructArrays;

                const size_t actor_index = FindActorIndexByHash(Root, Path[1]);
                if (actor_index == InvalidIndex()) {
                    return {};
                }

                auto actor_node = Root.GetArrayElementAsStruct(entities_field, actor_index);
                if (!actor_node.IsValid()) {
                    return {};
                }

                if (Path.size() == 2) {
                    return { true, Root.GetArrayElementBytes(entities_field, actor_index), actor_node };
                }

                return ResolveNode(actor_node, Path, 2, MapKey);
            }

            return ResolveNode(Root, Path, 0, MapKey);
        }

        static ResolvedPath ResolveNode(const TNodeView& Node, const DiffIndexPath& Path, size_t PathIndex, const std::string& MapKey) {
            if (!Node.IsValid() || PathIndex >= Path.size()) {
                return {};
            }

            const auto ContainerType = static_cast<EVTXContainerType>(Path[PathIndex]);
            const FieldDesc field = MakeField(ContainerType);
            const bool field_exists = HasField(Node, field);

            if (PathIndex + 1 >= Path.size()) {
                return { field_exists, Node.GetFieldBytes(field), Node.GetNestedStruct(field) };
            }

            const int32_t RawIndex = Path[PathIndex + 1];
            if (RawIndex < 0) {
                return {};
            }

            if (ContainerType == EVTXContainerType::AnyStructProperties) {
                const size_t struct_index = static_cast<size_t>(RawIndex);
                if (struct_index >= Node.GetArraySize(field)) {
                    return {};
                }

                auto child = Node.GetArrayElementAsStruct(field, struct_index);
                if (!child.IsValid()) {
                    return {};
                }

                if (PathIndex + 2 >= Path.size()) {
                    return { true, Node.GetArrayElementBytes(field, struct_index), child };
                }

                return ResolveNode(child, Path, PathIndex + 2, MapKey);
            }

            if (ContainerType == EVTXContainerType::MapProperties || ContainerType == EVTXContainerType::MapArrays) {
                const size_t map_index = FindMapIndexByHash(Node, field, RawIndex, MapKey);
                if (map_index == InvalidIndex()) {
                    return {};
                }

                auto child = Node.GetMapValueAsStruct(field, map_index);
                if (!child.IsValid()) {
                    return { true, {}, {} };
                }

                if (PathIndex + 2 >= Path.size()) {
                    return { true, {}, child };
                }

                return ResolveNode(child, Path, PathIndex + 2, MapKey);
            }

            if (IsArraysType(ContainerType)) {
                const size_t array_index = static_cast<size_t>(RawIndex);
                if (array_index >= Node.GetArraySize(field)) {
                    return {};
                }

                return { true, Node.GetSubArrayBytes(field, array_index), {} };
            }

            const size_t element_index = static_cast<size_t>(RawIndex);
            if (element_index >= Node.GetArraySize(field)) {
                return {};
            }

            return { true, Node.GetArrayElementBytes(field, element_index), {} };
        }
    };
}
