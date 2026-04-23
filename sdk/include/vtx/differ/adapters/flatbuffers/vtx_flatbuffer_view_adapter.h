#pragma once
#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/reflection.h>
#include <unordered_map>
#include <span>

#include "vtx/differ/core/vtx_diff_types.h"
#include "vtx/differ/core/interfaces/vtx_binary_view_node.h"


namespace VtxDiff::Flatbuffers {

    struct SubArrayNames {
        std::string FlatDataField;
        std::string OffsetsField;
    };

    struct CachedField {
        const reflection::Field* FbField = nullptr;
        EVTXContainerType VtxType = EVTXContainerType::Unknown;

        reflection::BaseType BaseType = reflection::None;
        reflection::BaseType ElementType = reflection::None;
        size_t ByteSize = 0;
        size_t ElementByteSize = 0;

        const reflection::Object* NestedObj = nullptr;
        const reflection::Field* SoaDataField = nullptr;
        const reflection::Field* SoaOffsetsField = nullptr;

        const reflection::Field* MapKeysField = nullptr;
        const reflection::Field* MapValuesField = nullptr;
        const reflection::Object* MapValuesObj = nullptr;
    };

    struct CachedObject {
        std::vector<FieldDesc> EnumerableFields;
        std::unordered_map<std::string_view, CachedField> FieldsByName;
    };

    class FbSchemaCache {
    public:
        void Build(const reflection::Schema* Schema);
        const CachedField* GetCachedField(const reflection::Object* Obj, std::string_view FieldName) const;
        const std::vector<FieldDesc>& GetEnumerableFields(const reflection::Object* Obj) const;

        const reflection::Schema* RawSchema = nullptr;

    private:
        std::unordered_map<const reflection::Object*, CachedObject> ObjectCache;
    };

    class FlatbufferViewAdapter {
    public:
        FlatbufferViewAdapter() = default;
        FlatbufferViewAdapter(const FbSchemaCache* Cache, const reflection::Object* Object,
                              const flatbuffers::Table* TablePtr);

        void Clear() {}
        void Reset();
        bool IsValid() const;

        std::span<const FieldDesc> EnumerateFields() const;
        std::span<const std::byte> GetFieldBytes(const FieldDesc& Fd) const;

        size_t GetArraySize(const FieldDesc& Fd) const;
        std::span<const std::byte> GetArrayElementBytes(const FieldDesc& Fd, size_t Index) const;
        std::span<const std::byte> GetSubArrayBytes(const FieldDesc& Fd, size_t SubIndex) const;

        size_t GetMapSize(const FieldDesc& Fd) const;
        std::string GetMapKey(const FieldDesc& Fd, size_t I) const;
        std::string GetScalarFieldString(const std::string& FieldName) const;
        uint64_t GetUint64Field(const std::string& FieldName) const;

        FlatbufferViewAdapter GetNestedStruct(const FieldDesc& Fd) const;
        FlatbufferViewAdapter GetArrayElementAsStruct(const FieldDesc& Fd, size_t Index) const;
        FlatbufferViewAdapter GetMapValueAsStruct(const FieldDesc& Fd, size_t I) const;
        FlatbufferViewAdapter GetFieldByName(const std::string& FieldName) const;

        static std::optional<FlatbufferViewAdapter> CreateRoot(const FbSchemaCache* Cache, const char* RootObjectName,
                                                               const uint8_t* Buffer, size_t BufferSize);

    private:
        const reflection::Field* FindField(std::string_view FieldName) const;
        const uint8_t* GetVectorData(const flatbuffers::Table* Tbl, const reflection::Field* F, size_t& Count,
                                     size_t& ElemSize) const;
        bool IsFlatArrayType(EVTXContainerType Type) const;

    private:
        const FbSchemaCache* Cache {nullptr};
        const reflection::Object* Object {nullptr};
        const flatbuffers::Table* TablePtr {nullptr};
    };

    static_assert(CBinaryNodeView<FlatbufferViewAdapter>,
                  "FlatbufferViewAdapter does not follow CBinaryNodeView concept!");

    class FbViewFactory {
    public:
        FbViewFactory() = default;

        std::optional<FlatbufferViewAdapter> CreateRoot(std::span<const std::byte> Buffer);

        bool InitFromFile(const std::string& Path, const std::string& RootType);
        bool InitFromMemory(const uint8_t* Data, size_t Size, const std::string& RootType);

    private:
        const reflection::Schema* Schema {};
        std::string RootName;
        std::unordered_map<std::string, SubArrayNames> SubNames;
        std::vector<uint8_t> SchemaBytes;
        FbSchemaCache GlobalCache;
    };


} // namespace VtxDiff::Flatbuffers
