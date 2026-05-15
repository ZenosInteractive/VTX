/**
 * @file binary_loader.h
 * @brief Generic raw-binary -> VTX::PropertyContainer loader, on top of GenericLoaderBase.
 *
 * @details Fifth sibling of GenericFlatBufferLoader / GenericProtobufLoader /
 *          GenericNativeLoader. Designed for the case "I have a const uint8_t* + size
 *          and I know the layout" -- the dev writes a BinaryBinding<Tag>::Transfer
 *          that walks a BinaryCursor with typed Read<T>() calls, and feeds each
 *          value into the right slot via the inherited LoadField.
 *
 *          No declarative schema for the binary layout: the order of Read<T>()
 *          calls IS the schema. Endianness, alignment, strings, sub-buffers and
 *          counted arrays are the binding's responsibility (BinaryCursor provides
 *          the tools).
 *
 * @note This loader is for byte-aligned binary blobs.
 *
 * @author Zenos Interactive
 */

#pragma once

#include "vtx/common/adapters/binary/binary_cursor.h"
#include "vtx/common/readers/frame_reader/loader_base.h"
#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"

#include <string>

namespace VTX {

    /**
     * @brief Format-specific binding the dev specializes to deserialize one entity
     *        (Transfer) or one frame (TransferToFrame) from a BinaryCursor.
     *
     * @details Same role as FlatBufferBinding<T> / ProtoBinding<T> /
     *          StructFrameBinding<T>, but the source is a BinaryCursor and @p Tag
     *          is an empty marker struct (there is no generated type to dispatch on).
     *
     * Example:
     * @code
     *   struct PlayerBin {};
     *
     *   template <>
     *   struct VTX::BinaryBinding<PlayerBin> {
     *       static void Transfer(VTX::BinaryCursor& cur, VTX::PropertyContainer& dest,
     *                            VTX::GenericBinaryLoader& loader, const std::string& schema_name) {
     *           loader.LoadField(dest, schema_name, "UniqueID", cur.ReadLenString<uint16_t>());
     *           loader.LoadField(dest, schema_name, "Health",   cur.Read<float>());
     *           VTX::Vector pos { cur.Read<double>(), cur.Read<double>(), cur.Read<double>() };
     *           loader.LoadField(dest, schema_name, "Position", pos);
     *       }
     *   };
     * @endcode
     *
     * @tparam Tag Empty marker struct identifying the binary layout.
     */
    template <typename Tag>
    struct BinaryBinding {
        static_assert(sizeof(Tag) == 0, "ERROR: Missing BinaryBinding<Tag> specialization.");
    };

    /**
     * @class GenericBinaryLoader
     * @brief Drives binary -> PropertyContainer deserialization through BinaryBinding<Tag>.
     *
     * @details Inherits LoadField / LoadBlob / AppendActorList / AppendSingleEntity /
     *          StoreValue / EnsureSize / PushToFlatArray / FillFlatArray from
     *          GenericLoaderBase via CRTP. Implements the two format-specific hooks:
     *          ResolveField (cache lookup) and Load<Tag> (binding dispatch).
     */
    class GenericBinaryLoader : public GenericLoaderBase<GenericBinaryLoader> {
    public:
        /**
         * @brief Construct a loader bound to a property-address cache.
         * @param cache Schema-derived O(1) field-address lookup table.
         * @param debug Enable verbose debug logging (currently unused).
         */
        explicit GenericBinaryLoader(const PropertyAddressCache& cache, bool debug = false)
            : cache_(&cache) {}


        /**
         * @brief Resolve a (struct, field) pair to a PropertyAddress via the cache.
         * @details Called automatically by GenericLoaderBase::LoadField /
         *          LoadBlob. The @p struct_name parameter is unused here (the
         *          cache is keyed by entity_type_id) -- kept in the signature
         *          for symmetry with the proto loader and for debug logging.
         * @param entity_type_id Numeric struct id, set on the destination
         *                       PropertyContainer by Load<Tag>().
         * @param struct_name Schema name of the parent struct (unused here).
         * @param field_name Schema name of the field to resolve.
         * @return Pointer to the cached address, or nullptr if not found.
         */
        const PropertyAddress* ResolveField(int32_t entity_type_id, const std::string& /*struct_name*/,
                                            const std::string& field_name) const {
            auto struct_it = cache_->structs.find(entity_type_id);
            if (struct_it == cache_->structs.end()) {
                return nullptr;
            }
            auto prop_it = struct_it->second.properties.find(field_name);
            if (prop_it == struct_it->second.properties.end()) {
                return nullptr;
            }
            return &prop_it->second;
        }


        /**
         * @brief Deserialize one entity from @p cursor into @p dest.
         * @details Sets dest.entity_type_id from @p struct_name if not yet
         *          resolved, dispatches to BinaryBinding<Tag>::Transfer, and
         *          recomputes the container content hash on exit.
         * @tparam Tag Empty marker struct identifying the binary layout.
         * @param cursor Source cursor; advances as the binding reads.
         * @param dest Target property container (cleared / populated in place).
         * @param struct_name Schema name of the destination struct.
         */
        template <typename Tag>
        void Load(BinaryCursor& cursor, PropertyContainer& dest, const std::string& struct_name) {
            if (dest.entity_type_id == -1) {
                auto it = cache_->name_to_id.find(struct_name);
                if (it != cache_->name_to_id.end()) {
                    dest.entity_type_id = it->second;
                }
            }
            BinaryBinding<Tag>::Transfer(cursor, dest, *this, struct_name);
            dest.content_hash = Helpers::CalculateContainerHash(dest);
        }

        /**
         * @brief Dispatch a top-level frame binding.
         * @details The binding owns the buckets / entities layout -- same
         *          convention as the FB / Proto / Native frame bindings.
         * @tparam FrameTag Empty marker struct identifying the frame layout.
         * @param cursor Source cursor positioned at the start of the frame.
         * @param dest Target VTX::Frame (overwritten by the binding).
         * @param schema_name Schema name passed through to the binding.
         */
        template <typename FrameTag>
        void LoadFrame(BinaryCursor& cursor, VTX::Frame& dest, const std::string& schema_name) {
            BinaryBinding<FrameTag>::TransferToFrame(cursor, dest, *this, schema_name);
        }

    private:
        const PropertyAddressCache* cache_;
    };

} // namespace VTX
