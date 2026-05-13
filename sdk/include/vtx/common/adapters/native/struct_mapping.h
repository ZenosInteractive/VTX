/**
* @file struct_mapping.h
 * @brief Compile-time reflection for "C++ struct -> VTX slots" mappings.
 *
 * Twin of JsonMapping<T> but the key is the VTX slot name (not a JSON key).
 * Consumed by GenericNativeLoader to walk a C++ instance and push each member
 * into the right PropertyContainer slot via the shared base's LoadField.
 *
 * Usage:
 *
 *   template<>
 *   struct VTX::StructMapping<ArenaPlayer> {
 *       static constexpr auto GetFields() {
 *           return std::make_tuple(
 *               MakeStructField(ArenaSchema::Player::UniqueID, &ArenaPlayer::unique_id),
 *               MakeStructField(ArenaSchema::Player::Health,   &ArenaPlayer::health),
 *               MakeStructField(ArenaSchema::Player::Position, &ArenaPlayer::position),
 *               ...);
 *       }
 *   };
 *
 * @author Zenos Interactive
 */

#pragma once
#include <type_traits>

namespace VTX {

    /**
     * @brief A single mapped field linking a VTX slot name to a C++ member.
     * @tparam Class The struct that owns the member.
     * @tparam Type  The member's type.
     */
    template <typename Class, typename Type>
    struct StructField {
        const char* name;
        Type Class::*member_ptr;
    };


    /**
     * @brief Helper to build a StructField with template deduction.
     * Usage: MakeStructField("Health", &Player::health)
     */
    template <typename Class, typename Type>
    constexpr auto MakeStructField(const char* name, Type Class::*member_ptr) {
        return StructField<Class, Type> {name, member_ptr};
    }

    /**
   * @brief Mapping trait. Users specialize for their types.
   * @details The specialization must provide a static `GetFields()` method
   * returning a tuple of StructField objects.
   */
    template <typename T>
    struct StructMapping;

    /**
     * @brief Frame-level binding trait. Users specialize with a `TransferToFrame`
     * static method that builds VTX::Frame buckets from the source struct.
     *
     * Equivalent to FlatBufferBinding<FrameT>::TransferToFrame and
     * ProtoBinding<FrameT>::TransferToFrame.
     *
     * Usage:
     *   template<>
     *   struct VTX::StructFrameBinding<ArenaFrame> {
     *       static void TransferToFrame(const ArenaFrame& src, VTX::Frame& dest,
     *                                   GenericNativeLoader& loader,
     *                                   const std::string& schema_name) {
     *           auto& bucket = dest.GetBucket("entity");
     *           loader.AppendActorList(bucket, ArenaSchema::Player::StructName,
     *                                  src.players,
     *                                  [](const ArenaPlayer& p){ return p.unique_id; });
     *           ...
     *       }
     *   };
     */
    template <typename T>
    struct StructFrameBinding;

    template <typename T, typename = void>
    struct HasStructMapping : std::false_type {};

    template <typename T>
    struct HasStructMapping<T, std::void_t<decltype(StructMapping<T>::GetFields())>> : std::true_type {};

    /**
     * @brief True if T has a registered StructMapping<T> specialization.
     * Used by GenericNativeLoader to decide between recursive Load (struct mapped)
     * and direct LoadField / FillFlatArray (primitive or VTX-native type).
     */
    template <typename T>
    inline constexpr bool has_struct_mapping_v = HasStructMapping<T>::value;


} // namespace VTX
