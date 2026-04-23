#include <flatbuffers/util.h>
#include <cstring>
#include <flatbuffers/reflection_generated.h>
#include <algorithm>
#include "vtx/differ/adapters/flatbuffers/vtx_flatbuffer_view_adapter.h"
#include "vtx_schema_bfbs_generated.h"
#include "vtx/differ/core/vtx_diff_types.h"

namespace VtxDiff::Flatbuffers {


    namespace {
        const reflection::Field* FindFieldByName(const reflection::Object* Obj, std::string_view Name) {
            if (!Obj || !Obj->fields())
                return nullptr;
            for (auto* F : *Obj->fields()) {
                if (F && F->name() && F->name()->string_view() == Name)
                    return F;
            }
            return nullptr;
        }

        EVTXContainerType InferTypeFromName(std::string_view Name) {
            if (Name == "bool_properties")
                return EVTXContainerType::BoolProperties;
            if (Name == "int32_properties")
                return EVTXContainerType::Int32Properties;
            if (Name == "int64_properties")
                return EVTXContainerType::Int64Properties;
            if (Name == "float_properties")
                return EVTXContainerType::FloatProperties;
            if (Name == "double_properties")
                return EVTXContainerType::DoubleProperties;
            if (Name == "string_properties")
                return EVTXContainerType::StringProperties;
            if (Name == "transform_properties")
                return EVTXContainerType::TransformProperties;
            if (Name == "vector_properties")
                return EVTXContainerType::VectorProperties;
            if (Name == "quat_properties")
                return EVTXContainerType::QuatProperties;
            if (Name == "range_properties")
                return EVTXContainerType::RangeProperties;
            if (Name == "byte_array_properties")
                return EVTXContainerType::ByteArrayProperties;

            if (Name == "int32_arrays")
                return EVTXContainerType::Int32Arrays;
            if (Name == "int64_arrays")
                return EVTXContainerType::Int64Arrays;
            if (Name == "float_arrays")
                return EVTXContainerType::FloatArrays;
            if (Name == "double_arrays")
                return EVTXContainerType::DoubleArrays;
            if (Name == "string_arrays")
                return EVTXContainerType::StringArrays;
            if (Name == "transform_arrays")
                return EVTXContainerType::TransformArrays;
            if (Name == "vector_arrays")
                return EVTXContainerType::VectorArrays;
            if (Name == "quat_arrays")
                return EVTXContainerType::QuatArrays;
            if (Name == "range_arrays")
                return EVTXContainerType::RangeArrays;
            if (Name == "bool_arrays")
                return EVTXContainerType::BoolArrays;

            if (Name == "any_struct_properties")
                return EVTXContainerType::AnyStructProperties;
            if (Name == "any_struct_arrays" || Name == "entities")
                return EVTXContainerType::AnyStructArrays;
            if (Name == "map_properties")
                return EVTXContainerType::MapProperties;
            if (Name == "map_arrays")
                return EVTXContainerType::MapArrays;

            return EVTXContainerType::Unknown;

            return EVTXContainerType::Unknown;
        }

        bool IsFlatArrayType(EVTXContainerType Type) {
            switch (Type) {
            case EVTXContainerType::Int32Arrays:
            case EVTXContainerType::Int64Arrays:
            case EVTXContainerType::FloatArrays:
            case EVTXContainerType::DoubleArrays:
            case EVTXContainerType::StringArrays:
            case EVTXContainerType::TransformArrays:
            case EVTXContainerType::VectorArrays:
            case EVTXContainerType::QuatArrays:
            case EVTXContainerType::RangeArrays:
            case EVTXContainerType::BoolArrays:
            case EVTXContainerType::AnyStructArrays:
            case EVTXContainerType::MapArrays:
                return true;
            default:
                return false;
            }
        }
    } // namespace


    void FbSchemaCache::Build(const reflection::Schema* Schema) {
        RawSchema = Schema;
        ObjectCache.clear();
        if (!Schema || !Schema->objects())
            return;

        for (auto* Obj : *Schema->objects()) {
            if (!Obj || !Obj->fields())
                continue;

            CachedObject CObj;
            CObj.EnumerableFields.reserve(Obj->fields()->size());

            for (auto* F : *Obj->fields()) {
                if (!F || !F->name())
                    continue;

                CachedField CF;
                CF.FbField = F;
                std::string_view Name = F->name()->string_view();
                CF.VtxType = InferTypeFromName(Name);
                CF.BaseType = F->type()->base_type();
                CF.ElementType = F->type()->element();

                if (CF.BaseType != reflection::Vector && CF.BaseType != reflection::Obj) {
                    CF.ByteSize = flatbuffers::GetTypeSize(
                        static_cast<reflection::BaseType>(static_cast<flatbuffers::ElementaryType>(CF.BaseType)));
                } else if (CF.BaseType == reflection::Obj) {
                    const auto* ObjSchema = Schema->objects()->Get(F->type()->index());
                    if (ObjSchema && ObjSchema->is_struct())
                        CF.ByteSize = static_cast<size_t>(ObjSchema->bytesize());
                }

                if (CF.BaseType == reflection::Vector) {
                    if (CF.ElementType == reflection::Obj) {
                        const auto* ObjSchema = Schema->objects()->Get(F->type()->index());
                        if (ObjSchema && ObjSchema->is_struct())
                            CF.ElementByteSize = static_cast<size_t>(ObjSchema->bytesize());
                    } else {
                        CF.ElementByteSize = flatbuffers::GetTypeSize(static_cast<reflection::BaseType>(
                            static_cast<flatbuffers::ElementaryType>(CF.ElementType)));
                    }
                }

                FieldDesc D;
                D.name = std::string(Name);
                D.type = CF.VtxType;
                D.is_array_like = IsFlatArrayType(CF.VtxType);
                D.is_map_like = (D.type == EVTXContainerType::MapProperties || D.type == EVTXContainerType::MapArrays);
                CObj.EnumerableFields.push_back(D);

                if (D.is_array_like && CF.BaseType == reflection::Obj) {
                    CF.NestedObj = Schema->objects()->Get(F->type()->index());
                    if (CF.NestedObj) {
                        CF.SoaDataField = FindFieldByName(CF.NestedObj, "data");
                        CF.SoaOffsetsField = FindFieldByName(CF.NestedObj, "offsets");
                    }
                }

                if (D.is_map_like && CF.BaseType == reflection::Vector) {
                    CF.NestedObj = Schema->objects()->Get(F->type()->index());
                    if (CF.NestedObj) {
                        CF.MapKeysField = FindFieldByName(CF.NestedObj, "keys");
                        CF.MapValuesField = FindFieldByName(CF.NestedObj, "values");
                        if (CF.MapValuesField) {
                            CF.MapValuesObj = Schema->objects()->Get(CF.MapValuesField->type()->index());
                        }
                    }
                }

                CObj.FieldsByName[Name] = CF;
            }
            ObjectCache[Obj] = std::move(CObj);
        }
    }

    const CachedField* FbSchemaCache::GetCachedField(const reflection::Object* Obj, std::string_view FieldName) const {
        auto It = ObjectCache.find(Obj);
        if (It != ObjectCache.end()) {
            auto FieldIt = It->second.FieldsByName.find(FieldName);
            if (FieldIt != It->second.FieldsByName.end()) {
                return &FieldIt->second;
            }
        }
        return nullptr;
    }

    const std::vector<FieldDesc>& FbSchemaCache::GetEnumerableFields(const reflection::Object* Obj) const {
        auto It = ObjectCache.find(Obj);
        if (It != ObjectCache.end())
            return It->second.EnumerableFields;
        static std::vector<FieldDesc> Empty;
        return Empty;
    }


    FlatbufferViewAdapter::FlatbufferViewAdapter(const FbSchemaCache* Cache, const reflection::Object* Object,
                                                 const flatbuffers::Table* TablePtr)
        : Cache(Cache)
        , Object(Object)
        , TablePtr(TablePtr) {}

    void FlatbufferViewAdapter::Reset() {}

    bool FlatbufferViewAdapter::IsValid() const {
        return TablePtr && Cache && Object;
    }

    std::optional<FlatbufferViewAdapter> FlatbufferViewAdapter::CreateRoot(const FbSchemaCache* Cache,
                                                                           const char* RootObjectName,
                                                                           const uint8_t* Buffer, size_t BufferSize) {
        if (!Cache || !Cache->RawSchema || !RootObjectName || !Buffer)
            return std::nullopt;

        const reflection::Object* RootObj = nullptr;
        for (auto* O : *Cache->RawSchema->objects()) {
            if (O && O->name() && std::strcmp(O->name()->c_str(), RootObjectName) == 0) {
                RootObj = O;
                break;
            }
        }
        if (!RootObj)
            return std::nullopt;

        auto* Tbl = flatbuffers::GetRoot<flatbuffers::Table>(Buffer);
        return FlatbufferViewAdapter(Cache, RootObj, Tbl);
    }

    std::span<const FieldDesc> FlatbufferViewAdapter::EnumerateFields() const {
        return Cache->GetEnumerableFields(Object);
    }

    const reflection::Field* FlatbufferViewAdapter::FindField(std::string_view FieldName) const {
        return FindFieldByName(Object, FieldName);
    }

    const uint8_t* FlatbufferViewAdapter::GetVectorData(const flatbuffers::Table* Tbl, const reflection::Field* F,
                                                        size_t& Count, size_t& ElemSize) const {
        Count = 0;
        ElemSize = 0;
        if (!Tbl || !F || F->type()->base_type() != reflection::Vector)
            return nullptr;

        auto VecU8 = Tbl->GetPointer<const flatbuffers::Vector<uint8_t>*>(F->offset());
        if (!VecU8)
            return nullptr;
        Count = VecU8->size();

        if (F->type()->element() == reflection::Obj) {
            const auto* ObjSchema = Cache->RawSchema->objects()->Get(F->type()->index());
            ElemSize = (ObjSchema && ObjSchema->is_struct()) ? static_cast<size_t>(ObjSchema->bytesize()) : 0;
        } else {
            ElemSize = flatbuffers::GetTypeSize(
                static_cast<reflection::BaseType>(static_cast<flatbuffers::ElementaryType>(F->type()->element())));
        }
        return VecU8->Data();
    }

    bool FlatbufferViewAdapter::IsFlatArrayType(EVTXContainerType Type) const {
        return (Type >= EVTXContainerType::ByteArrayProperties && Type <= EVTXContainerType::BoolArrays) ||
               Type == EVTXContainerType::AnyStructArrays || Type == EVTXContainerType::MapArrays;
    }

    size_t FlatbufferViewAdapter::GetArraySize(const FieldDesc& Fd) const {
        const auto* CF = Cache->GetCachedField(Object, Fd.name);
        if (!CF || !CF->FbField)
            return 0;

        if (CF->SoaOffsetsField) {
            const flatbuffers::Table* NestedTable =
                TablePtr->GetPointer<const flatbuffers::Table*>(CF->FbField->offset());
            if (!NestedTable)
                return 0;
            size_t count = 0, elemSize = 0;
            GetVectorData(NestedTable, CF->SoaOffsetsField, count, elemSize);
            return count;
        }

        size_t count = 0, elemSize = 0;
        GetVectorData(TablePtr, CF->FbField, count, elemSize);
        return count;
    }

    std::span<const std::byte> FlatbufferViewAdapter::GetFieldBytes(const FieldDesc& Fd) const {
        const auto* CF = Cache->GetCachedField(Object, Fd.name);
        if (!CF || !CF->FbField)
            return {};

        auto Voff = static_cast<flatbuffers::voffset_t>(CF->FbField->offset());

        if (CF->BaseType == reflection::String) {
            const flatbuffers::String* S = TablePtr->GetPointer<const flatbuffers::String*>(Voff);
            if (!S)
                return {};
            return {reinterpret_cast<const std::byte*>(S->c_str()), S->size()};
        }

        if (CF->BaseType != reflection::Vector && CF->BaseType != reflection::Obj) {
            const uint8_t* Ptr = TablePtr->GetAddressOf(Voff);
            if (!Ptr)
                return {};
            return {reinterpret_cast<const std::byte*>(Ptr), CF->ByteSize};
        }

        if (CF->BaseType == reflection::Obj && CF->ByteSize > 0) {
            const uint8_t* Ptr = TablePtr->GetStruct<const uint8_t*>(Voff);
            if (!Ptr)
                return {};
            return {reinterpret_cast<const std::byte*>(Ptr), CF->ByteSize};
        }
        return {};
    }

    std::span<const std::byte> FlatbufferViewAdapter::GetArrayElementBytes(const FieldDesc& Fd, size_t Index) const {
        const auto* CF = Cache->GetCachedField(Object, Fd.name);
        if (!CF)
            return {};

        if (CF->SoaDataField)
            return GetSubArrayBytes(Fd, Index);

        if (!CF->FbField || CF->BaseType != reflection::Vector)
            return {};

        if (CF->ElementType == reflection::String) {
            auto Vec = TablePtr->GetPointer<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>*>(
                CF->FbField->offset());
            if (!Vec || Index >= Vec->size())
                return {};
            const flatbuffers::String* S = Vec->Get(Index);
            return S ? std::span<const std::byte> {reinterpret_cast<const std::byte*>(S->c_str()), S->size()}
                     : std::span<const std::byte> {};
        }

        auto VecU8 = TablePtr->GetPointer<const flatbuffers::Vector<uint8_t>*>(CF->FbField->offset());
        if (!VecU8 || Index >= VecU8->size() || CF->ElementByteSize == 0)
            return {};

        return {reinterpret_cast<const std::byte*>(VecU8->Data() + Index * CF->ElementByteSize), CF->ElementByteSize};
    }

    std::span<const std::byte> FlatbufferViewAdapter::GetSubArrayBytes(const FieldDesc& Fd, size_t SubIndex) const {
        const auto* CF = Cache->GetCachedField(Object, Fd.name);
        if (!CF || !CF->SoaDataField || !CF->SoaOffsetsField)
            return {};

        const flatbuffers::Table* NestedTable = TablePtr->GetPointer<const flatbuffers::Table*>(CF->FbField->offset());
        if (!NestedTable)
            return {};

        auto VecOffsets = NestedTable->GetPointer<const flatbuffers::Vector<uint32_t>*>(CF->SoaOffsetsField->offset());
        if (!VecOffsets || SubIndex >= VecOffsets->size())
            return {};

        uint32_t StartElem = VecOffsets->Get(static_cast<flatbuffers::uoffset_t>(SubIndex));

        if (CF->SoaDataField->type()->element() == reflection::String) {
            auto Vec = NestedTable->GetPointer<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>*>(
                CF->SoaDataField->offset());
            if (!Vec)
                return {};

            uint32_t EndElem = (SubIndex + 1 < VecOffsets->size())
                                   ? VecOffsets->Get(static_cast<flatbuffers::uoffset_t>(SubIndex + 1))
                                   : static_cast<uint32_t>(Vec->size());
            if (EndElem < StartElem || EndElem > Vec->size())
                return {};

            size_t totalBytes = 0;
            for (uint32_t i = StartElem; i < EndElem; ++i) {
                const flatbuffers::String* S = Vec->Get(i);
                totalBytes += 4 + (S ? S->size() : 0);
            }

            static thread_local std::vector<std::byte> Scratch;
            Scratch.resize(totalBytes);

            std::byte* Dest = Scratch.data();
            for (uint32_t i = StartElem; i < EndElem; ++i) {
                const flatbuffers::String* S = Vec->Get(i);
                uint32_t Len = S ? static_cast<uint32_t>(S->size()) : 0u;
                std::memcpy(Dest, &Len, 4);
                Dest += 4;
                if (Len) {
                    std::memcpy(Dest, S->c_str(), Len);
                    Dest += Len;
                }
            }
            return {Scratch.data(), Scratch.size()};
        }

        auto VecData = NestedTable->GetPointer<const flatbuffers::Vector<uint8_t>*>(CF->SoaDataField->offset());
        if (!VecData)
            return {};

        size_t DataElemSize = flatbuffers::GetTypeSize(static_cast<reflection::BaseType>(
            static_cast<flatbuffers::ElementaryType>(CF->SoaDataField->type()->element())));

        uint32_t EndElem = (SubIndex + 1 < VecOffsets->size())
                               ? VecOffsets->Get(static_cast<flatbuffers::uoffset_t>(SubIndex + 1))
                               : static_cast<uint32_t>(VecData->size() / DataElemSize);
        if (EndElem < StartElem)
            return {};

        size_t ByteStart = StartElem * DataElemSize;
        size_t ByteLen = (EndElem - StartElem) * DataElemSize;

        return {reinterpret_cast<const std::byte*>(VecData->Data() + ByteStart), ByteLen};
    }

    FlatbufferViewAdapter FlatbufferViewAdapter::GetArrayElementAsStruct(const FieldDesc& Fd, size_t Index) const {
        const auto* CF = Cache->GetCachedField(Object, Fd.name);
        if (!CF || !CF->FbField || CF->FbField->type()->base_type() != reflection::Vector ||
            CF->FbField->type()->element() != reflection::Obj)
            return {}; // Devuelve vacío

        const auto* NestedObj = Cache->RawSchema->objects()->Get(CF->FbField->type()->index());
        if (!NestedObj || NestedObj->is_struct())
            return {};

        auto Vec = TablePtr->GetPointer<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*>(
            CF->FbField->offset());
        if (!Vec || Index >= Vec->size())
            return {};
        return FlatbufferViewAdapter(Cache, NestedObj, Vec->Get(Index));
    }

    FlatbufferViewAdapter FlatbufferViewAdapter::GetNestedStruct(const FieldDesc& Fd) const {
        const auto* CF = Cache->GetCachedField(Object, Fd.name);
        if (!CF || !CF->FbField || CF->FbField->type()->base_type() != reflection::Obj)
            return {};

        const auto* NestedObj = Cache->RawSchema->objects()->Get(CF->FbField->type()->index());
        if (!NestedObj || NestedObj->is_struct())
            return {};

        const flatbuffers::Table* NestedTbl = TablePtr->GetPointer<const flatbuffers::Table*>(CF->FbField->offset());
        if (!NestedTbl)
            return {};

        return FlatbufferViewAdapter(Cache, NestedObj, NestedTbl);
    }

    FlatbufferViewAdapter FlatbufferViewAdapter::GetMapValueAsStruct(const FieldDesc& Fd, size_t I) const {
        const auto* CF = Cache->GetCachedField(Object, Fd.name);
        if (!CF || !CF->MapValuesField || !CF->MapValuesObj)
            return {};

        auto Vec = TablePtr->GetPointer<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*>(
            CF->FbField->offset());
        if (!Vec || I >= Vec->size())
            return {};

        const flatbuffers::Table* MC = Vec->Get(I);
        if (!MC)
            return {};

        auto Values = MC->GetPointer<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*>(
            CF->MapValuesField->offset());
        if (!Values || Values->size() == 0)
            return {};

        size_t ValIndex = std::min<size_t>(I, Values->size() - 1);
        return FlatbufferViewAdapter(Cache, CF->MapValuesObj,
                                     Values->Get(static_cast<flatbuffers::uoffset_t>(ValIndex)));
    }

    FlatbufferViewAdapter FlatbufferViewAdapter::GetFieldByName(const std::string& FieldName) const {
        FieldDesc fd;
        fd.name = FieldName;
        return GetNestedStruct(fd);
    }

    size_t FlatbufferViewAdapter::GetMapSize(const FieldDesc& Fd) const {
        const auto* CF = Cache->GetCachedField(Object, Fd.name);
        if (!CF || !CF->FbField)
            return 0;
        auto Vec = TablePtr->GetPointer<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*>(
            CF->FbField->offset());
        return Vec ? Vec->size() : 0;
    }

    std::string FlatbufferViewAdapter::GetMapKey(const FieldDesc& Fd, size_t I) const {
        const auto* CF = Cache->GetCachedField(Object, Fd.name);
        if (!CF || !CF->MapKeysField)
            return "";

        auto Vec = TablePtr->GetPointer<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*>(
            CF->FbField->offset());
        if (!Vec || I >= Vec->size())
            return "";

        const flatbuffers::Table* MC = Vec->Get(I);
        if (!MC)
            return "";

        auto Keys = MC->GetPointer<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>*>(
            CF->MapKeysField->offset());
        if (!Keys || Keys->size() == 0)
            return "";

        size_t KeyIndex = std::min<size_t>(I, Keys->size() - 1);
        return Keys->Get(static_cast<flatbuffers::uoffset_t>(KeyIndex))->str();
    }

    std::string FlatbufferViewAdapter::GetScalarFieldString(const std::string& FieldName) const {
        if (!TablePtr || !Object)
            return {};

        const reflection::Field* FbField = FindField(FieldName);
        if (!FbField)
            return {};

        auto voffset = static_cast<flatbuffers::voffset_t>(FbField->offset());
        auto base_type = FbField->type()->base_type();

        switch (base_type) {
        case reflection::String: {
            const auto* s = TablePtr->GetPointer<const flatbuffers::String*>(voffset);
            return s ? s->str() : std::string {};
        }
        case reflection::ULong:
            return std::to_string(
                TablePtr->GetField<uint64_t>(voffset, static_cast<uint64_t>(FbField->default_integer())));
        case reflection::Long:
            return std::to_string(
                TablePtr->GetField<int64_t>(voffset, static_cast<int64_t>(FbField->default_integer())));
        case reflection::UInt:
            return std::to_string(
                TablePtr->GetField<uint32_t>(voffset, static_cast<uint32_t>(FbField->default_integer())));
        case reflection::Int:
            return std::to_string(
                TablePtr->GetField<int32_t>(voffset, static_cast<int32_t>(FbField->default_integer())));
        case reflection::Float:
            return std::to_string(TablePtr->GetField<float>(voffset, static_cast<float>(FbField->default_real())));
        case reflection::Double:
            return std::to_string(TablePtr->GetField<double>(voffset, static_cast<double>(FbField->default_real())));
        case reflection::Bool:
            return TablePtr->GetField<uint8_t>(voffset, static_cast<uint8_t>(FbField->default_integer())) ? "true"
                                                                                                          : "false";
        case reflection::UByte:
            return std::to_string(
                TablePtr->GetField<uint8_t>(voffset, static_cast<uint8_t>(FbField->default_integer())));
        case reflection::Byte:
            return std::to_string(TablePtr->GetField<int8_t>(voffset, static_cast<int8_t>(FbField->default_integer())));
        case reflection::UShort:
            return std::to_string(
                TablePtr->GetField<uint16_t>(voffset, static_cast<uint16_t>(FbField->default_integer())));
        case reflection::Short:
            return std::to_string(
                TablePtr->GetField<int16_t>(voffset, static_cast<int16_t>(FbField->default_integer())));
        case reflection::None:
        case reflection::UType:
        case reflection::Vector:
        case reflection::Obj:
        case reflection::Union:
        case reflection::Array:
        case reflection::Vector64:
        case reflection::MaxBaseType:
        default:
            return {};
        }
    }

    uint64_t FlatbufferViewAdapter::GetUint64Field(const std::string& FieldName) const {
        const reflection::Field* FbField = FindField(FieldName);
        if (!FbField)
            return 0;

        auto voffset = static_cast<flatbuffers::voffset_t>(FbField->offset());
        auto base_type = FbField->type()->base_type();
        if (base_type == reflection::ULong) {
            return TablePtr->GetField<uint64_t>(voffset, static_cast<uint64_t>(FbField->default_integer()));
        }
        return 0;
    }


    std::optional<FlatbufferViewAdapter> FbViewFactory::CreateRoot(std::span<const std::byte> Buffer) {
        if (RootName.empty() || Buffer.empty() || !GlobalCache.RawSchema)
            return std::nullopt;
        return FlatbufferViewAdapter::CreateRoot(&GlobalCache, RootName.c_str(),
                                                 reinterpret_cast<const uint8_t*>(Buffer.data()), Buffer.size());
    }

    bool FbViewFactory::InitFromFile(const std::string& Path, const std::string& RootType) {
        return false;
    }

    bool FbViewFactory::InitFromMemory(const uint8_t* Data, size_t Size, const std::string& RootType) {
        const uint8_t* schemaData = fbsvtx::FileHeaderBinarySchema::data();
        size_t schemaSize = fbsvtx::FileHeaderBinarySchema::size();

        if (!schemaData || schemaSize == 0)
            return false;
        const reflection::Schema* loadedSchema = reflection::GetSchema(schemaData);
        if (!loadedSchema)
            return false;

        GlobalCache.Build(loadedSchema);
        RootName = RootType;
        return true;
    }

} // namespace VtxDiff::Flatbuffers