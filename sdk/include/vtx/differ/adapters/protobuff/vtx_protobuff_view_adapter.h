#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <span>
#include <cstddef>
#include <optional>

#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>

#include "vtx/differ/core/vtx_diff_types.h"
#include "vtx/differ/core/interfaces/vtx_binary_view_node.h"


namespace VtxDiff::Protobuf {

    struct SubArrayNames {
        std::string FlatDataField;
        std::string OffsetsField;
    };

    class FProtobufViewAdapter {
    public:
        FProtobufViewAdapter() = default;

        FProtobufViewAdapter(const google::protobuf::Message* InMsg, bool bOwnsMessage);

        ~FProtobufViewAdapter();

        FProtobufViewAdapter(FProtobufViewAdapter&&) noexcept;
        FProtobufViewAdapter& operator=(FProtobufViewAdapter&&) noexcept;

        FProtobufViewAdapter(const FProtobufViewAdapter&) = delete;
        FProtobufViewAdapter& operator=(const FProtobufViewAdapter&) = delete;

        static std::optional<FProtobufViewAdapter> CreateRoot(const google::protobuf::DescriptorPool* Pool,
                                                              const std::string& RootType, const uint8_t* Buffer,
                                                              size_t Size);

        static std::optional<FProtobufViewAdapter> FromMessage(const google::protobuf::Message& Message);

        void SetSubArrayNames(std::unordered_map<std::string, SubArrayNames> Map);

        void Reset();
        bool IsValid() const;

        std::span<const FieldDesc> EnumerateFields() const;
        std::span<const std::byte> GetFieldBytes(const VtxDiff::FieldDesc& Field) const;
        std::string GetScalarFieldString(const std::string& FieldName) const;
        uint64_t GetUint64Field(const std::string& FieldName) const;

        size_t GetArraySize(const VtxDiff::FieldDesc& Field) const;
        std::span<const std::byte> GetArrayElementBytes(const VtxDiff::FieldDesc& Field, size_t Index) const;
        std::span<const std::byte> GetSubArrayBytes(const VtxDiff::FieldDesc& Field, size_t SubIndex) const;

        size_t GetMapSize(const VtxDiff::FieldDesc& Field) const;
        std::string GetMapKey(const VtxDiff::FieldDesc& Field, size_t Index) const;

        FProtobufViewAdapter GetNestedStruct(const VtxDiff::FieldDesc& Field) const;
        FProtobufViewAdapter GetArrayElementAsStruct(const VtxDiff::FieldDesc& Field, size_t Index) const;
        FProtobufViewAdapter GetMapValueAsStruct(const VtxDiff::FieldDesc& Field, size_t Index) const;
        FProtobufViewAdapter GetFieldByName(const std::string& FieldName) const;

    private:
        std::unique_ptr<google::protobuf::Message> Owned;
        const google::protobuf::Message* External = nullptr;

        mutable std::vector<std::vector<std::byte>> Arena;
        mutable std::vector<std::byte> Scratch;
        mutable std::string TempString;
        std::unordered_map<std::string, SubArrayNames> SubNames;

        mutable std::vector<FieldDesc> CachedFields;
        mutable bool bFieldsCached = false;

    private:
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

    static_assert(CBinaryNodeView<FProtobufViewAdapter>,
                  "FProtobufViewAdapter does not follow CBinaryNodeView concept!");

    class PbViewFactory {
    public:
        PbViewFactory() = default;
        ~PbViewFactory() = default;

        bool InitFromMemory(const uint8_t* Data, size_t Size, const std::string& RootType);
        bool InitFromFile(const std::string& Path, const std::string& RootType);
        void SetSubArrayNames(const std::unordered_map<std::string, SubArrayNames>& Map);
        std::optional<FProtobufViewAdapter> CreateRoot(std::span<const std::byte> Buffer) const;

        const std::string& GetRootType() const { return RootType; }

    private:
        std::string RootType;
        std::unordered_map<std::string, SubArrayNames> SubNames;
        const google::protobuf::DescriptorPool* UsePool = nullptr;
        std::unique_ptr<google::protobuf::DescriptorPool> OwnedPool;
    };

} // namespace VtxDiff::Protobuf
