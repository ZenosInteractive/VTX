#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <span>
#include <cstddef>
#include <cstdint>
#include <sstream>

#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>

#include "V2/DiffEngine/Core/IBinaryViewFactory.h"

namespace VtxDiff::Protobuf {

    // ================================================================
    // Struct: SubArrayNames
    // ---------------------------------------------------------------
    // Describes paired fields for subarray handling, e.g.:
    //    "vertices_arrays" + "vertices_offsets"
    // ================================================================
    struct SubArrayNames {
        std::string FlatDataField;
        std::string OffsetsField;
    };

    // ================================================================
    // Class: ProtobufViewAdapter
    // ---------------------------------------------------------------
    // A reflection-based adapter that exposes a protobuf message as a
    // hierarchical binary tree for diff computation.
    // ================================================================
    class FProtobufViewAdapter final : public IBinaryNodeView {
    public:
        FProtobufViewAdapter(const google::protobuf::Message* Msg);
        virtual ~FProtobufViewAdapter() override;

        static std::unique_ptr<FProtobufViewAdapter> CreateRoot(const google::protobuf::DescriptorPool* Pool,
                                                                const std::string& RootType, const uint8_t* Buffer,
                                                                size_t Size);

        static std::unique_ptr<FProtobufViewAdapter> FromMessage(const google::protobuf::Message& Message);

        void SetSubArrayNames(std::unordered_map<std::string, SubArrayNames> Map);

        // ============================================================
        // IBinaryNodeView interface
        // ============================================================
        virtual std::vector<VtxDiff::FieldDesc> EnumerateFields() const override;
        virtual std::span<const std::byte> GetFieldBytes(const VtxDiff::FieldDesc& Field) const override;
        virtual size_t GetArraySize(const VtxDiff::FieldDesc& Field) const override;
        virtual std::span<const std::byte> GetArrayElementBytes(const VtxDiff::FieldDesc& Field,
                                                                size_t Index) const override;
        virtual std::span<const std::byte> GetSubArrayBytes(const VtxDiff::FieldDesc& Field,
                                                            size_t SubIndex) const override;
        virtual std::unique_ptr<IBinaryNodeView> GetNestedStruct(const VtxDiff::FieldDesc& Field) const override;
        virtual std::unique_ptr<IBinaryNodeView> GetArrayElementAsStruct(const VtxDiff::FieldDesc& Field,
                                                                         size_t Index) const override;
        virtual bool IsValid() const override;
        virtual size_t GetMapSize(const VtxDiff::FieldDesc& Field) const override;
        virtual std::string GetMapKey(const VtxDiff::FieldDesc& Field, size_t Index) const override;
        virtual std::unique_ptr<IBinaryNodeView> GetMapValueAsStruct(const VtxDiff::FieldDesc& Field,
                                                                     size_t Index) const override;
        virtual std::string GetScalarFieldString(const std::string& FieldName) const override;
        virtual std::unique_ptr<IBinaryNodeView> GetFieldByName(const std::string& FieldName) const override;

    private:
        std::unique_ptr<google::protobuf::Message> Owned;
        const google::protobuf::Message* External = nullptr;

        mutable std::vector<std::vector<std::byte>> Arena;
        mutable std::vector<std::byte> Scratch;
        mutable std::string TempString;
        std::unordered_map<std::string, SubArrayNames> SubNames;

    private:
        // Helpers internos
        const google::protobuf::FieldDescriptor* FindField(std::string_view Name) const;

        bool IsScalar(const google::protobuf::FieldDescriptor* Field) const;

        EVTXContainerType InferContainerType(const google::protobuf::FieldDescriptor* Field) const;
        std::span<const std::byte> Publish(const void* Data, size_t Size) const;
        std::span<const std::byte> Publish(const std::vector<std::byte>& Buffer) const;
        std::span<const std::byte> Publish(std::vector<std::byte>&& Buffer) const;
        void ResetArena() const;
        const google::protobuf::Descriptor* ActiveDesc() const;
        static std::unique_ptr<google::protobuf::Message> CloneMessage(const google::protobuf::Message& Src);

    public:
        virtual void Reset() override;

        inline const google::protobuf::Message* GetMsg() const noexcept { return Owned ? Owned.get() : External; }

        inline const google::protobuf::Descriptor* GetDesc() const noexcept {
            auto* m = GetMsg();
            return m ? m->GetDescriptor() : nullptr;
        }

        inline const google::protobuf::Reflection* GetRefl() const noexcept {
            auto* m = GetMsg();
            return m ? m->GetReflection() : nullptr;
        }
    };

    // ================================================================
    // Class: PbViewFactory
    // ---------------------------------------------------------------
    // Factory responsible for loading descriptor pools and creating
    // root-level ProtobufViewAdapters.
    // ================================================================
    class PbViewFactory final : public IBinaryViewFactory {
    public:
        PbViewFactory() = default;
        ~PbViewFactory() override = default;

        bool InitFromMemory(const uint8_t* Data, size_t Size, const std::string& RootType);
        bool InitFromFile(const std::string& Path, const std::string& RootType);

        void SetSubArrayNames(const std::unordered_map<std::string, SubArrayNames>& Map);
        std::unique_ptr<IBinaryNodeView> CreateRoot(std::span<const std::byte> Buffer) const override;

        const std::string& GetRootType() const { return RootType; }

    private:
        std::string RootType;
        std::unordered_map<std::string, SubArrayNames> SubNames;
        const google::protobuf::DescriptorPool* UsePool = nullptr;
        std::unique_ptr<google::protobuf::DescriptorPool> OwnedPool;
    };

} // namespace VtxDiff::Protobuf
