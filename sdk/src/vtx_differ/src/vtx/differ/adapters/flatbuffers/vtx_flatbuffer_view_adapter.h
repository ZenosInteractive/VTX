#pragma once


/*
// FlatBuffers reflection
#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/reflection.h>

#include <unordered_map>

#include "V2/DiffEngine/IBinaryNodeView.h"

namespace VtxDiff::Flatbuffers {

struct SubArrayNames {
    std::string FlatDataField;
    std::string OffsetsField;
};

class FlatbufferViewAdapter final : public IBinaryNodeView {
public:
    FlatbufferViewAdapter(const reflection::Schema* Schema,
                          const reflection::Object* Object,
                          const flatbuffers::Table* TablePtr);

    virtual void Clear()override{}
    void SetSubArrayNames(const std::unordered_map<std::string, SubArrayNames>& Map) {
        SubArrayFields = Map;
    }

    // --- IBinaryNodeView ---
    virtual std::vector<FieldDesc> EnumerateFields() const override;
    virtual std::span<const std::byte> GetFieldBytes(const FieldDesc& Fd) const override;
 
    virtual size_t GetArraySize(const FieldDesc& Fd) const override;
    virtual std::span<const std::byte> GetArrayElementBytes(const FieldDesc& Fd, size_t Index) const override;
 
    virtual std::span<const std::byte> GetSubArrayBytes(const FieldDesc& Fd, size_t SubIndex) const override;
 
    virtual std::unique_ptr<IBinaryNodeView> GetNestedStruct(const FieldDesc& Fd) const override;
    virtual std::unique_ptr<IBinaryNodeView> GetArrayElementAsStruct(const FieldDesc& Fd, size_t Index) const override;
 
    virtual size_t GetMapSize(const FieldDesc& Fd) const override;
    virtual std::string GetMapKey(const FieldDesc& Fd, size_t I) const override;
    virtual std::unique_ptr<IBinaryNodeView> GetMapValueAsStruct(const FieldDesc& Fd, size_t I) const override;

    static std::unique_ptr<FlatbufferViewAdapter> CreateRoot(const reflection::Schema* Schema,
                                                             const char* RootObjectName,
                                                             const uint8_t* Buffer,size_t BufferSize);

private:
    const reflection::Field* FindField(std::string_view FieldName) const;
    const uint8_t* GetVectorData(const reflection::Field* F, size_t& Count, size_t& ElemSize) const;

    bool ResolveSubArrayFields(const std::string& Base,
                               const reflection::Field*& FlatDataF,
                               const reflection::Field*& OffsetsF) const;

    static size_t PodSizeFor(FieldType T);
    FieldType InferFieldType(const reflection::Field* F) const;
    bool IsSubArrayBase(const std::string& BaseName,
                        const reflection::Field*& FlatDataF,
                        const reflection::Field*& OffsetsF) const;

private:
    const reflection::Schema* Schema{};
    const reflection::Object* Object{};
    const flatbuffers::Table* TablePtr{};

    std::unordered_map<std::string, SubArrayNames> SubArrayFields;
};


class FbViewFactory final : public IBinaryViewFactory, public IDiffInitializer {
public:
    FbViewFactory() = default;
    FbViewFactory(const reflection::Schema* Schema, const char* RootObjectName)
        : Schema(Schema), RootName(RootObjectName) {}

    virtual void SetSubArrayNames(const std::unordered_map<std::string, SubArrayNames>& Map);

    virtual std::unique_ptr<IBinaryNodeView> CreateRoot(std::span<const std::byte> Buffer) const override;

    virtual bool InitFromFile(const std::string& Path, const std::string& RootType) override;
    virtual bool InitFromMemory(const uint8_t* Data, size_t Size, const std::string& RootType) override;

private:
    const reflection::Schema* Schema{};
    std::string RootName;
    std::unordered_map<std::string, SubArrayNames> SubNames;
    std::vector<uint8_t> SchemaBytes;
};

    
}*/