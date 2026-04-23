// ============================================================================
// ProtobufViewAdapter.cpp
// ----------------------------------------------------------------------------
// Complete implementation of ProtobufViewAdapter and PbViewFactory in C++20.
// Based on original architecture, with full field traversal, array/subarray
// handling, and map/nested structure support. Unreal dependencies removed.
// ============================================================================

#include "vtx/differ/adapters/protobuff/vtx_protobuff_view_adapter.h"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/descriptor_database.h>
#include <google/protobuf/dynamic_message.h>

#include <algorithm>
#include <mutex>
#include <array>


#include "vtx/common/vtx_logger.h"
#include "vtx/differ/core/vtx_diff_types.h"
#include "vtx/differ/core/interfaces/vtx_binary_view_node.h"

using namespace google::protobuf;


namespace VtxDiff::Protobuf {
    // ============================================================================
    // Persistent factory per descriptor pool
    // ============================================================================
    namespace {
        using FactoryPtr = std::unique_ptr<DynamicMessageFactory>;
        std::mutex GlobalFactoryMutex;
        std::unordered_map<const DescriptorPool*, FactoryPtr> GlobalFactories;

        DynamicMessageFactory* GetFactoryForPool(const DescriptorPool* Pool) {
            std::lock_guard<std::mutex> Lock(GlobalFactoryMutex);
            auto& Entry = GlobalFactories[Pool];
            if (!Entry)
                Entry = std::make_unique<DynamicMessageFactory>(Pool);
            return Entry.get();
        }

        std::unique_ptr<Message> NewOwnedMessage(const DescriptorPool* Pool, const Descriptor* Desc) {
            DynamicMessageFactory* Factory = GetFactoryForPool(Pool);
            const Message* Proto = Factory->GetPrototype(Desc);
            if (!Proto)
                return {};
            return std::unique_ptr<Message>(Proto->New());
        }

        std::unique_ptr<Message> CloneMessage(const Message& Src) {
            const Descriptor* Desc = Src.GetDescriptor();
            const DescriptorPool* Pool = Desc->file()->pool();
            auto Copy = NewOwnedMessage(Pool, Desc);
            if (Copy)
                Copy->CopyFrom(Src);
            return Copy;
        }
    } // namespace

    // ============================================================================
    // Byte utilities
    // ============================================================================

    template <typename T>
    static void PutLE(std::vector<std::byte>& Dst, T Value) {
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
        std::array<unsigned char, sizeof(T)> buf {};
        std::memcpy(buf.data(), &Value, sizeof(T));
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        std::reverse(buf.begin(), buf.end());
#elif defined(_BIG_ENDIAN) && _BIG_ENDIAN
        std::reverse(buf.begin(), buf.end());
#endif
        for (unsigned char b : buf)
            Dst.push_back(static_cast<std::byte>(b));
    }

    static inline void PutF(std::vector<std::byte>& dst, float v) {
        PutLE(dst, v);
    }

    // ============================================================================
    // Vector/Quat/Transform packing helpers
    // ============================================================================

    static float SafeGetFloat(const Message& m, const Reflection* r, const FieldDescriptor* f) {
        if (!f)
            return 0.0f;

        if (f->cpp_type() == FieldDescriptor::CPPTYPE_DOUBLE) {
            return static_cast<float>(r->GetDouble(m, f));
        } else if (f->cpp_type() == FieldDescriptor::CPPTYPE_FLOAT) {
            return r->GetFloat(m, f);
        }
        return 0.0f;
    }

    static bool PackVector(const Message& m, std::vector<std::byte>& out) {
        auto* r = m.GetReflection();
        const Descriptor* d = m.GetDescriptor();
        const FieldDescriptor* fx = d->FindFieldByName("x");
        if (!fx)
            fx = d->FindFieldByName("r");
        const FieldDescriptor* fy = d->FindFieldByName("y");
        if (!fy)
            fy = d->FindFieldByName("g");
        const FieldDescriptor* fz = d->FindFieldByName("z");
        if (!fz)
            fz = d->FindFieldByName("b");
        const FieldDescriptor* fw = d->FindFieldByName("w");

        if (!fx || !fy || !fz)
            return false;

        PutF(out, SafeGetFloat(m, r, fx));
        PutF(out, SafeGetFloat(m, r, fy));
        PutF(out, SafeGetFloat(m, r, fz));
        if (fw)
            PutF(out, SafeGetFloat(m, r, fw));

        return true;
    }

    static bool PackQuat(const Message& m, std::vector<std::byte>& out) {
        auto* d = m.GetDescriptor();
        auto* r = m.GetReflection();
        const char* names[4] = {"x", "y", "z", "w"};
        for (int i = 0; i < 4; ++i) {
            auto* f = d->FindFieldByName(names[i]);
            if (!f)
                return false;
            PutF(out, SafeGetFloat(m, r, f));
        }
        return true;
    }

    static bool PackFloatRange(const Message& m, std::vector<std::byte>& out) {
        auto* r = m.GetReflection();
        const Descriptor* d = m.GetDescriptor();
        auto fmin = d->FindFieldByName("min");
        auto fmax = d->FindFieldByName("max");

        PutF(out, SafeGetFloat(m, r, fmin));
        PutF(out, SafeGetFloat(m, r, fmax));
        return true;
    }

    static bool PackTransform(const Message& m, std::vector<std::byte>& out) {
        auto* d = m.GetDescriptor();
        auto* r = m.GetReflection();

        auto packVec = [&](const FieldDescriptor* f) -> bool {
            if (!f || f->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE)
                return false;
            const Message& sub = r->GetMessage(m, f);
            return PackVector(sub, out); // x,y,z
        };

        auto* fT = d->FindFieldByName("translation");
        auto* fR = d->FindFieldByName("rotation");
        auto* fS = d->FindFieldByName("scale");
        if (!fT || !fR || !fS)
            return false;

        if (!packVec(fT))
            return false;

        // quat (x,y,z,w)
        {
            const Message& q = r->GetMessage(m, fR);
            if (!PackQuat(q, out))
                return false;
        }

        if (!packVec(fS))
            return false;
        return true;
    }

    static inline bool IsKnownStructMsg(const Descriptor* md) {
        if (!md)
            return false;
        const std::string& n = md->name();
        return n == "Vector" || n == "Quat" || n == "Transform" || n == "FloatRange";
    }

    static bool PackKnownStruct(const Message& m, std::vector<std::byte>& out) {
        const std::string& n = m.GetDescriptor()->name();
        if (n == "Vector")
            return PackVector(m, out);
        if (n == "Quat")
            return PackQuat(m, out);
        if (n == "Transform")
            return PackTransform(m, out);
        if (n == "FloatRange")
            return PackFloatRange(m, out);
        return false;
    }

    // ============================================================
    // Constructor / Destructor
    // ============================================================

    FProtobufViewAdapter::FProtobufViewAdapter(const google::protobuf::Message* InMsg, bool bOwnsMessage)
        : External(InMsg) {
        if (!bOwnsMessage) {}
    }

    FProtobufViewAdapter::~FProtobufViewAdapter() = default;

    FProtobufViewAdapter::FProtobufViewAdapter(FProtobufViewAdapter&&) noexcept = default;
    FProtobufViewAdapter& FProtobufViewAdapter::operator=(FProtobufViewAdapter&&) noexcept = default;


    // ============================================================
    // CloneMessage
    // ------------------------------------------------------------
    // Creates a deep copy of a protobuf message using the same
    // DescriptorPool and DynamicMessageFactory.
    // This is used to produce isolated views for diffing without
    // modifying the original protobuf instance.
    // ============================================================

    std::unique_ptr<Message> FProtobufViewAdapter::CloneMessage(const Message& Src) {
        const google::protobuf::Descriptor* Desc = Src.GetDescriptor();
        const google::protobuf::DescriptorPool* Pool =
            Desc ? Desc->file()->pool() : google::protobuf::DescriptorPool::generated_pool();

        // Always use the same pool as the source
        auto* Factory = GetFactoryForPool(Pool);
        const google::protobuf::Message* Proto = Factory->GetPrototype(Desc);
        if (!Proto)
            return nullptr;

        auto Copy = std::unique_ptr<google::protobuf::Message>(Proto->New());
        Copy->CopyFrom(Src);
        return Copy;
    }

    void FProtobufViewAdapter::Reset() {
        ResetArena();
    }

    // ============================================================
    // CreateRoot
    // ------------------------------------------------------------
    // Creates a ProtobufViewAdapter from a serialized buffer and
    // the provided root message type name.
    // Used when reading frames directly from binary memory.
    // ============================================================
    std::optional<FProtobufViewAdapter> FProtobufViewAdapter::CreateRoot(const DescriptorPool* Pool,
                                                                         const std::string& RootType,
                                                                         const uint8_t* Buffer, size_t Size) {
        if (!Pool || !Buffer || Size == 0)
            return std::nullopt;
        const google::protobuf::Descriptor* Desc = Pool->FindMessageTypeByName(RootType);
        if (!Desc)
            return std::nullopt;
        auto OwnedMsg = NewOwnedMessage(Pool, Desc);
        if (!OwnedMsg)
            return std::nullopt;
        if (!OwnedMsg->ParseFromArray(Buffer, static_cast<int>(Size)))
            return std::nullopt;

        FProtobufViewAdapter Adapter(OwnedMsg.get(), true);
        Adapter.Owned = std::move(OwnedMsg);
        return Adapter;
    }

    // ============================================================
    // FromMessage
    // ------------------------------------------------------------
    // Creates a ProtobufViewAdapter from an existing in-memory
    // protobuf message by cloning it internally.
    // This avoids modifying the original instance during diffing.
    // ============================================================

    std::optional<FProtobufViewAdapter> FProtobufViewAdapter::FromMessage(const Message& M) {
        auto OwnedMsg = CloneMessage(M);
        if (!OwnedMsg)
            return std::nullopt;

        FProtobufViewAdapter Adapter(OwnedMsg.get(), true);
        Adapter.Owned = std::move(OwnedMsg);
        return Adapter;
    }

    void FProtobufViewAdapter::SetSubArrayNames(std::unordered_map<std::string, SubArrayNames> Map) {
        SubNames = std::move(Map);
    }
    // ============================================================
    // FindField
    // ------------------------------------------------------------
    // Locates a field descriptor by its name within the current
    // message descriptor. Returns nullptr if not found.
    // ============================================================
    const google::protobuf::FieldDescriptor* FProtobufViewAdapter::FindField(std::string_view Name) const {
        const auto* D = GetDesc();
        if (!D)
            return nullptr;

        return D->FindFieldByName(std::string(Name));
    }


    // ============================================================
    // GetScalarFieldString
    // ------------------------------------------------------------
    // Retrieves the string value of a scalar (non-repeated) field
    // from the current protobuf message. Returns an empty string
    // if the field does not exist or is not a string type.
    // ============================================================

    std::string FProtobufViewAdapter::GetScalarFieldString(const std::string& FieldName) const {
        const auto* M = GetMsg();
        const auto* D = GetDesc();
        const auto* R = GetRefl();

        if (!M || !D || !R)
            return {};

        const auto* Field = D->FindFieldByName(FieldName);

        if (!Field || Field->is_repeated())
            return {};

        switch (Field->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
            return R->GetString(*M, Field);

        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
            return std::to_string(R->GetUInt64(*M, Field));

        case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
            return std::to_string(R->GetInt64(*M, Field));

        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
            return std::to_string(R->GetUInt32(*M, Field));

        case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
            return std::to_string(R->GetInt32(*M, Field));

        case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
            return std::to_string(R->GetFloat(*M, Field));

        case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
            return std::to_string(R->GetDouble(*M, Field));

        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
            return R->GetBool(*M, Field) ? "true" : "false";

        case google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
            return std::to_string(R->GetEnumValue(*M, Field));

        default:
            return {};
        }
    }

    uint64_t FProtobufViewAdapter::GetUint64Field(const std::string& FieldName) const {
        const auto* M = GetMsg();
        const auto* D = GetDesc();
        const auto* R = GetRefl();

        if (!M || !D || !R)
            return 0;

        const auto* Field = D->FindFieldByName(FieldName);

        if (!Field || Field->is_repeated())
            return {};

        if (Field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_UINT64) {
            return R->GetUInt64(*M, Field);
        }
        return 0;
    }

    // ============================================================
    // GetFieldByName
    // ------------------------------------------------------------
    // Returns a nested node view (sub-structure) for the given field
    // name, if that field represents a singular message type.
    // ============================================================
    FProtobufViewAdapter FProtobufViewAdapter::GetFieldByName(const std::string& FieldName) const {
        const auto* M = GetMsg();
        const auto* D = GetDesc();
        const auto* R = GetRefl();
        if (!M || !D || !R)
            return {};
        const auto* Field = FindField(FieldName);
        if (!Field || Field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE || Field->is_repeated())
            return {};
        if (!R->HasField(*M, Field))
            return {};

        const google::protobuf::Message& Nested = R->GetMessage(*M, Field);
        return FProtobufViewAdapter(&Nested, false);
    }


    // ============================================================
    // IsScalar
    // ------------------------------------------------------------
    // Returns true if a protobuf field is a scalar POD type
    // (int, float, double, bool, enum, etc).
    // ============================================================

    bool FProtobufViewAdapter::IsScalar(const FieldDescriptor* Field) const {
        if (!Field)
            return false;

        using FD = google::protobuf::FieldDescriptor;
        switch (Field->cpp_type()) {
        case FD::CPPTYPE_INT32:
        case FD::CPPTYPE_UINT32:
        case FD::CPPTYPE_INT64:
        case FD::CPPTYPE_UINT64:
        case FD::CPPTYPE_FLOAT:
        case FD::CPPTYPE_DOUBLE:
        case FD::CPPTYPE_BOOL:
        case FD::CPPTYPE_ENUM:
            return true;
        default:
            return false;
        }
    }

    // ============================================================
    // InferContainerType
    // ------------------------------------------------------------
    // Determines the EVTXContainerType that corresponds to a given
    // protobuf field. This mapping follows the naming and layout
    // rules used in FVTXPropertyContainerArrays.
    // ============================================================

    EVTXContainerType FProtobufViewAdapter::InferContainerType(const FieldDescriptor* Field) const {
        if (!Field)
            return EVTXContainerType::Unknown;

        const std::string_view N = Field->name();

        using Pair = std::pair<std::string_view, EVTXContainerType>;
        static constexpr std::array<Pair, 25> kByName = {
            {{"transform_properties", EVTXContainerType::TransformProperties},
             {"int32_properties", EVTXContainerType::Int32Properties},
             {"int64_properties", EVTXContainerType::Int64Properties},
             {"float_properties", EVTXContainerType::FloatProperties},
             {"double_properties", EVTXContainerType::DoubleProperties},
             {"vector_properties", EVTXContainerType::VectorProperties},
             {"quat_properties", EVTXContainerType::QuatProperties},
             {"range_properties", EVTXContainerType::RangeProperties},
             {"bool_properties", EVTXContainerType::BoolProperties},
             {"string_properties", EVTXContainerType::StringProperties},

             {"byte_array_properties", EVTXContainerType::ByteArrayProperties},
             {"int32_arrays", EVTXContainerType::Int32Arrays},
             {"int64_arrays", EVTXContainerType::Int64Arrays},
             {"float_arrays", EVTXContainerType::FloatArrays},
             {"double_arrays", EVTXContainerType::DoubleArrays},
             {"vector_arrays", EVTXContainerType::VectorArrays},
             {"quat_arrays", EVTXContainerType::QuatArrays},
             {"transform_arrays", EVTXContainerType::TransformArrays},
             {"range_arrays", EVTXContainerType::RangeArrays},
             {"bool_arrays", EVTXContainerType::BoolArrays},
             {"string_arrays", EVTXContainerType::StringArrays},

             {"any_struct_properties", EVTXContainerType::AnyStructProperties},
             {"any_struct_arrays", EVTXContainerType::AnyStructArrays},
             {"map_properties", EVTXContainerType::MapProperties},
             {"map_arrays", EVTXContainerType::MapArrays}}};

        for (const auto& [K, V] : kByName)
            if (K == N)
                return V;

        return EVTXContainerType::Unknown;
    }

    std::span<const std::byte> FProtobufViewAdapter::Publish(const void* Data, size_t Size) const {
        if (!Data || Size == 0)
            return {};

        const auto* Bytes = static_cast<const std::byte*>(Data);
        Arena.emplace_back(Bytes, Bytes + Size);

        const auto& Last = Arena.back();
        return {Last.data(), Last.size()};
    }

    std::span<const std::byte> FProtobufViewAdapter::Publish(const std::vector<std::byte>& Buffer) const {
        if (Buffer.empty())
            return {};

        Arena.emplace_back(Buffer.begin(), Buffer.end());
        const auto& Last = Arena.back();
        return {Last.data(), Last.size()};
    }

    std::span<const std::byte> FProtobufViewAdapter::Publish(std::vector<std::byte>&& Buffer) const {
        Arena.push_back(std::move(Buffer));
        const auto& Last = Arena.back();
        return {Last.data(), Last.size()};
    }

    void FProtobufViewAdapter::ResetArena() const {
        Arena.clear();
        Scratch.clear();
        TempString.clear();
    }

    const google::protobuf::Descriptor* FProtobufViewAdapter::ActiveDesc() const {
        return GetDesc();
    }

    // ============================================================
    // EnumerateFields
    // ------------------------------------------------------------
    // Returns a list of all protobuf fields in the current message
    // that contain meaningful data. This function dynamically queries
    // the descriptor and reflection interface, without relying on any
    // cached pointers.
    // ============================================================
    std::span<const VtxDiff::FieldDesc> FProtobufViewAdapter::EnumerateFields() const {
        if (bFieldsCached)
            return CachedFields;

        const auto* M = GetMsg();
        const auto* D = GetDesc();
        const auto* R = GetRefl();

        if (!M || !D || !R)
            return {};

        CachedFields.reserve(D->field_count());

        auto hasData = [&](const google::protobuf::FieldDescriptor* f) -> bool {
            if (!f)
                return false;
            if (f->is_repeated())
                return R->FieldSize(*M, f) > 0;

            using FD = google::protobuf::FieldDescriptor;
            switch (f->cpp_type()) {
            case FD::CPPTYPE_MESSAGE:
                return R->HasField(*M, f);
            case FD::CPPTYPE_STRING:
                return !R->GetString(*M, f).empty();
            case FD::CPPTYPE_BOOL:
                return R->GetBool(*M, f);
            case FD::CPPTYPE_INT32:
                return R->GetInt32(*M, f) != 0;
            case FD::CPPTYPE_UINT32:
                return R->GetUInt32(*M, f) != 0;
            case FD::CPPTYPE_INT64:
                return R->GetInt64(*M, f) != 0;
            case FD::CPPTYPE_UINT64:
                return R->GetUInt64(*M, f) != 0;
            case FD::CPPTYPE_FLOAT:
                return R->GetFloat(*M, f) != 0.0f;
            case FD::CPPTYPE_DOUBLE:
                return R->GetDouble(*M, f) != 0.0;
            case FD::CPPTYPE_ENUM:
                return R->GetEnumValue(*M, f) != 0;
            default:
                return false;
            }
        };

        for (int i = 0; i < D->field_count(); ++i) {
            const auto* f = D->field(i);
            if (!f || !hasData(f))
                continue;

            VtxDiff::FieldDesc d;
            d.name = f->name();
            d.is_actors_field = (d.name == "entities");
            d.type = InferContainerType(f);
            d.is_array_like = f->is_repeated();
            d.is_map_like = f->is_map();

            CachedFields.push_back(std::move(d));
        }

        bFieldsCached = true;
        return CachedFields;
    }

    // ============================================================
    // GetFieldBytes
    // ------------------------------------------------------------
    // Retrieves the binary data for a specific field in the message.
    // Handles scalar, repeated, and nested message types.
    // ============================================================
    std::span<const std::byte> FProtobufViewAdapter::GetFieldBytes(const VtxDiff::FieldDesc& Field) const {
        const auto* M = GetMsg();
        const auto* D = GetDesc();
        const auto* R = GetRefl();

        if (!M || !D || !R)
            return {};

        const auto* F = D->FindFieldByName(Field.name);
        if (!F)
            return {};

        Scratch.clear();

        // ============================================================
        // Handle scalar (non-repeated, non-message) types
        // ============================================================
        if (!F->is_repeated() && F->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
            switch (F->cpp_type()) {
            case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
                PutLE(Scratch, R->GetInt32(*M, F));
                break;
            case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
                PutLE(Scratch, R->GetUInt32(*M, F));
                break;
            case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
                PutLE(Scratch, R->GetInt64(*M, F));
                break;
            case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
                PutLE(Scratch, R->GetUInt64(*M, F));
                break;
            case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
                PutLE(Scratch, R->GetFloat(*M, F));
                break;
            case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
                PutLE(Scratch, R->GetDouble(*M, F));
                break;
            case google::protobuf::FieldDescriptor::CPPTYPE_BOOL: {
                uint8_t val = R->GetBool(*M, F) ? 1 : 0;
                Scratch.push_back(static_cast<std::byte>(val));
                break;
            }
            case google::protobuf::FieldDescriptor::CPPTYPE_STRING: {
                std::string s = R->GetString(*M, F);
                return Publish(s.data(), s.size());
            }
            default:
                break;
            }

            return Publish(Scratch);
        }

        // ============================================================
        // Handle singular nested messages (e.g., Vector, Quat, Transform)
        // ============================================================
        if (!F->is_repeated() && F->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
            const google::protobuf::Message& Nested = R->GetMessage(*M, F);

            // Try to pack known math-style structs efficiently
            if (PackKnownStruct(Nested, Scratch))
                return Publish(Scratch);

            // Fallback: serialize full message
            std::vector<std::byte> Data(static_cast<size_t>(Nested.ByteSizeLong()));
            Nested.SerializePartialToArray(Data.data(), static_cast<int>(Data.size()));
            return Publish(std::move(Data));
        }

        // ============================================================
        // Handle repeated scalar fields
        // ============================================================
        if (F->is_repeated() && F->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
            const int Count = R->FieldSize(*M, F);
            Scratch.reserve(Count * 8); // estimated element size

            for (int i = 0; i < Count; ++i) {
                switch (F->cpp_type()) {
                case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
                    PutLE(Scratch, R->GetRepeatedInt32(*M, F, i));
                    break;
                case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
                    PutLE(Scratch, R->GetRepeatedUInt32(*M, F, i));
                    break;
                case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
                    PutLE(Scratch, R->GetRepeatedInt64(*M, F, i));
                    break;
                case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
                    PutLE(Scratch, R->GetRepeatedUInt64(*M, F, i));
                    break;
                case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
                    PutLE(Scratch, R->GetRepeatedFloat(*M, F, i));
                    break;
                case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
                    PutLE(Scratch, R->GetRepeatedDouble(*M, F, i));
                    break;
                case google::protobuf::FieldDescriptor::CPPTYPE_BOOL: {
                    uint8_t val = R->GetRepeatedBool(*M, F, i) ? 1 : 0;
                    Scratch.push_back(static_cast<std::byte>(val));
                    break;
                }
                default:
                    break;
                }
            }

            return Publish(Scratch);
        }

        return {};
    }


    // ============================================================
    // GetArraySize
    // ------------------------------------------------------------
    // Returns the number of elements in a repeated field or 1 for
    // scalar fields. Returns 0 if the field is invalid or unset.
    // ============================================================
    size_t FProtobufViewAdapter::GetArraySize(const VtxDiff::FieldDesc& Fd) const {
        const auto* M = GetMsg();
        const auto* R = GetRefl();

        if (!M || !R) {
            VTX_WARN("[GetArraySize] Invalid message or reflection for field {}", Fd.name);
            return 0;
        }

        const auto* F = FindField(Fd.name);
        if (!F) {
            VTX_ERROR("[GetArraySize] Field not found in message: {}", Fd.name);

            return 0;
        }

        const int Size = F->is_repeated() ? R->FieldSize(*M, F) : 1;

#if defined(UE_LOG)
        UE_LOG(LogTemp, Verbose, TEXT("[GetArraySize] Field %s size = %d (is_repeated=%d)"), *FString(Fd.Name.c_str()),
               Size, static_cast<int>(F->is_repeated()));
#endif

        return static_cast<size_t>(Size);
    }

    // ============================================================
    // GetArrayElementBytes
    // ------------------------------------------------------------
    // Returns a binary span for a specific element of a repeated field.
    // Handles scalar and message element types.
    // ============================================================
    std::span<const std::byte> FProtobufViewAdapter::GetArrayElementBytes(const VtxDiff::FieldDesc& Field,
                                                                          size_t Index) const {
        const auto* M = GetMsg();
        const auto* D = GetDesc();
        const auto* R = GetRefl();

        if (!M || !D || !R)
            return {};

        const auto* F = D->FindFieldByName(Field.name);
        if (!F || !F->is_repeated())
            return {};

        const int Count = R->FieldSize(*M, F);
        if (Index >= static_cast<size_t>(Count))
            return {};

        Scratch.clear();

        // Handle repeated message elements (Vector, Quat, Transform, etc.)
        if (F->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
            const google::protobuf::Message& Nested = R->GetRepeatedMessage(*M, F, static_cast<int>(Index));

            // Attempt to pack known math/struct types directly
            if (PackKnownStruct(Nested, Scratch))
                return Publish(Scratch);

            // Fallback: serialize the entire sub-message
            std::vector<std::byte> Data(static_cast<size_t>(Nested.ByteSizeLong()));
            Nested.SerializePartialToArray(Data.data(), static_cast<int>(Data.size()));
            return Publish(std::move(Data));
        }

        // Handle repeated scalar types
        switch (F->cpp_type()) {
        case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
            PutLE(Scratch, R->GetRepeatedInt32(*M, F, static_cast<int>(Index)));
            break;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
            PutLE(Scratch, R->GetRepeatedUInt32(*M, F, static_cast<int>(Index)));
            break;
        case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
            PutLE(Scratch, R->GetRepeatedInt64(*M, F, static_cast<int>(Index)));
            break;
        case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
            PutLE(Scratch, R->GetRepeatedUInt64(*M, F, static_cast<int>(Index)));
            break;
        case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
            PutLE(Scratch, R->GetRepeatedFloat(*M, F, static_cast<int>(Index)));
            break;
        case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
            PutLE(Scratch, R->GetRepeatedDouble(*M, F, static_cast<int>(Index)));
            break;
        case google::protobuf::FieldDescriptor::CPPTYPE_BOOL: {
            uint8_t val = R->GetRepeatedBool(*M, F, static_cast<int>(Index)) ? 1 : 0;
            Scratch.push_back(static_cast<std::byte>(val));
            break;
        }
        case google::protobuf::FieldDescriptor::CPPTYPE_STRING: {
            std::string Str = R->GetRepeatedString(*M, F, static_cast<int>(Index));
            return Publish(Str.data(), Str.size());
        }
        default:
            break;
        }

        return Publish(Scratch);
    }


    // ============================================================
    // GetSubArrayBytes
    // ------------------------------------------------------------
    // Returns a binary span representing the serialized data of a
    // sub-array (nested repeated message fields, e.g. TArray<TArray<T>>).
    // ============================================================
    std::span<const std::byte> FProtobufViewAdapter::GetSubArrayBytes(const VtxDiff::FieldDesc& Field,
                                                                      size_t SubIndex) const {
        const auto* M = GetMsg();
        const auto* D = GetDesc();
        const auto* R = GetRefl();

        if (!M || !D || !R)
            return {};

#if defined(UE_LOG)
        UE_LOG(LogTemp, Verbose, TEXT("[GetSubArrayBytes] Msg type = %hs | Field = %hs"),
               UTF8_TO_TCHAR(M->GetDescriptor()->full_name().c_str()), UTF8_TO_TCHAR(Field.Name.c_str()));
#endif

        const auto* F = D->FindFieldByName(Field.name);
        if (!F || !F->is_repeated() || F->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE)
            return {};

        const int OuterCount = R->FieldSize(*M, F);
        if (SubIndex >= static_cast<size_t>(OuterCount))
            return {};

        Scratch.clear();

        // Retrieve the message representing one "subarray"
        const google::protobuf::Message& SubArrayMsg = R->GetRepeatedMessage(*M, F, static_cast<int>(SubIndex));

        const auto* SubDesc = SubArrayMsg.GetDescriptor();
        const auto* SubRefl = SubArrayMsg.GetReflection();
        if (!SubDesc || !SubRefl)
            return {};

        // Iterate over nested repeated fields (usually one)
        for (int i = 0; i < SubDesc->field_count(); ++i) {
            const auto* InnerField = SubDesc->field(i);
            if (!InnerField || !InnerField->is_repeated())
                continue;

            const int InnerCount = SubRefl->FieldSize(SubArrayMsg, InnerField);
            if (InnerCount == 0)
                continue;

            // Serialize all elements inside the subarray
            for (int j = 0; j < InnerCount; ++j) {
                if (InnerField->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
                    const google::protobuf::Message& ElemMsg = SubRefl->GetRepeatedMessage(SubArrayMsg, InnerField, j);

                    // Attempt to pack known math structs directly
                    if (!PackKnownStruct(ElemMsg, Scratch)) {
                        std::vector<std::byte> Data(static_cast<size_t>(ElemMsg.ByteSizeLong()));
                        ElemMsg.SerializePartialToArray(Data.data(), static_cast<int>(Data.size()));
                        Scratch.insert(Scratch.end(), Data.begin(), Data.end());
                    }
                } else if (InnerField->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
                    std::string s = SubRefl->GetRepeatedString(SubArrayMsg, InnerField, j);
                    Scratch.insert(Scratch.end(), reinterpret_cast<const std::byte*>(s.data()),
                                   reinterpret_cast<const std::byte*>(s.data()) + s.size());
                } else {
                    // Handle scalar types
                    switch (InnerField->cpp_type()) {
                    case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
                        PutLE(Scratch, SubRefl->GetRepeatedInt32(SubArrayMsg, InnerField, j));
                        break;
                    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
                        PutLE(Scratch, SubRefl->GetRepeatedUInt32(SubArrayMsg, InnerField, j));
                        break;
                    case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
                        PutLE(Scratch, SubRefl->GetRepeatedInt64(SubArrayMsg, InnerField, j));
                        break;
                    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
                        PutLE(Scratch, SubRefl->GetRepeatedUInt64(SubArrayMsg, InnerField, j));
                        break;
                    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
                        PutLE(Scratch, SubRefl->GetRepeatedFloat(SubArrayMsg, InnerField, j));
                        break;
                    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
                        PutLE(Scratch, SubRefl->GetRepeatedDouble(SubArrayMsg, InnerField, j));
                        break;
                    case google::protobuf::FieldDescriptor::CPPTYPE_BOOL: {
                        uint8_t val = SubRefl->GetRepeatedBool(SubArrayMsg, InnerField, j) ? 1 : 0;
                        Scratch.push_back(static_cast<std::byte>(val));
                        break;
                    }
                    default:
                        break;
                    }
                }
            }

            // Only process the first repeated field per subarray
            break;
        }

        return Publish(Scratch);
    }

    // ============================================================
    // GetMapSize
    // ------------------------------------------------------------
    // Returns the number of key-value pairs in a map field.
    // Supports both native protobuf map<K,V> and Unreal-style
    // FVTXMapContainer (Keys + Values arrays).
    // ============================================================
    size_t FProtobufViewAdapter::GetMapSize(const VtxDiff::FieldDesc& Field) const {
        const auto* M = GetMsg();
        const auto* D = GetDesc();
        const auto* R = GetRefl();

        if (!M || !D || !R)
            return 0;

        const auto* F = D->FindFieldByName(Field.name);
        if (!F)
            return 0;

        // Case 1: native protobuf map<K, V>
        if (F->is_map())
            return static_cast<size_t>(R->FieldSize(*M, F));

        // Case 2: Unreal-style "FVTXMapContainer" (Keys + Values arrays)
        if (Field.type == EVTXContainerType::MapProperties) {
            const auto* KeysField = D->FindFieldByName("Keys");
            const auto* ValuesField = D->FindFieldByName("Values");
            if (!KeysField || !ValuesField)
                return 0;

            const int KeyCount = R->FieldSize(*M, KeysField);
            const int ValueCount = R->FieldSize(*M, ValuesField);
            return static_cast<size_t>(std::min(KeyCount, ValueCount));
        }

        return 0;
    }


    // ============================================================
    // GetMapKey
    // ------------------------------------------------------------
    // Retrieves the string representation of a map key at the given
    // index. Supports both native protobuf maps and FVTXMapContainer.
    // ============================================================
    std::string FProtobufViewAdapter::GetMapKey(const VtxDiff::FieldDesc& Field, size_t Index) const {
        const auto* M = GetMsg();
        const auto* D = GetDesc();
        const auto* R = GetRefl();

        if (!M || !D || !R)
            return {};

        const auto* F = D->FindFieldByName(Field.name);
        if (!F)
            return {};

        // --- Case 1: Native protobuf map<K,V> ---
        if (F->is_map()) {
            const google::protobuf::Message& Entry = R->GetRepeatedMessage(*M, F, static_cast<int>(Index));

            const auto* EntryRefl = Entry.GetReflection();
            const auto* KeyField = F->message_type()->map_key();
            if (!KeyField || !EntryRefl)
                return {};

            switch (KeyField->cpp_type()) {
            case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
                return EntryRefl->GetString(Entry, KeyField);
            case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
                return std::to_string(EntryRefl->GetInt32(Entry, KeyField));
            case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
                return std::to_string(EntryRefl->GetUInt32(Entry, KeyField));
            case google::protobuf::FieldDescriptor::CPPTYPE_INT64:
                return std::to_string(EntryRefl->GetInt64(Entry, KeyField));
            case google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
                return std::to_string(EntryRefl->GetUInt64(Entry, KeyField));
            default:
                return {};
            }
        }

        // --- Case 2: FVTXMapContainer (Keys + Values arrays) ---
        if (Field.type == EVTXContainerType::MapProperties) {
            const auto* KeysField = D->FindFieldByName("Keys");
            if (!KeysField || !KeysField->is_repeated())
                return {};

            if (Index >= static_cast<size_t>(R->FieldSize(*M, KeysField)))
                return {};

            if (KeysField->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING)
                return R->GetRepeatedString(*M, KeysField, static_cast<int>(Index));

            if (KeysField->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_INT32)
                return std::to_string(R->GetRepeatedInt32(*M, KeysField, static_cast<int>(Index)));

            return {};
        }

        return {};
    }


    // ============================================================
    // GetMapValueAsStruct
    // ------------------------------------------------------------
    // Returns a node view for the map value at the specified index.
    // Supports both native protobuf maps and FVTXMapContainer.
    // ============================================================
    FProtobufViewAdapter FProtobufViewAdapter::GetMapValueAsStruct(const VtxDiff::FieldDesc& Field,
                                                                   size_t Index) const {
        const auto* M = GetMsg();
        const auto* D = GetDesc();
        const auto* R = GetRefl();
        if (!M || !D || !R)
            return {};
        const auto* F = D->FindFieldByName(Field.name);
        if (!F)
            return {};

        if (F->is_map()) {
            const google::protobuf::Message& Entry = R->GetRepeatedMessage(*M, F, static_cast<int>(Index));
            const auto* EntryRefl = Entry.GetReflection();
            const auto* ValueField = F->message_type()->map_value();
            if (!EntryRefl || !ValueField ||
                ValueField->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE)
                return {};

            const google::protobuf::Message& ValueMsg = EntryRefl->GetMessage(Entry, ValueField);
            return FProtobufViewAdapter(&ValueMsg, false);
        }

        if (Field.type == EVTXContainerType::MapProperties) {
            const auto* ValuesField = D->FindFieldByName("Values");
            if (!ValuesField || !ValuesField->is_repeated() ||
                Index >= static_cast<size_t>(R->FieldSize(*M, ValuesField)))
                return {};

            const google::protobuf::Message& SubMsg = R->GetRepeatedMessage(*M, ValuesField, static_cast<int>(Index));
            return FProtobufViewAdapter(&SubMsg, false);
        }
        return {};
    }

    // ============================================================
    // GetArrayElementAsStruct
    // ------------------------------------------------------------
    // Returns a nested IBinaryNodeView for an element inside a
    // repeated message field (e.g., array of structs).
    // ============================================================
    FProtobufViewAdapter FProtobufViewAdapter::GetArrayElementAsStruct(const VtxDiff::FieldDesc& Field,
                                                                       size_t Index) const {
        const auto* M = GetMsg();
        const auto* D = GetDesc();
        const auto* R = GetRefl();
        if (!M || !D || !R)
            return {};
        const auto* F = D->FindFieldByName(Field.name);
        if (!F || !F->is_repeated() || F->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE)
            return {};

        const int Count = R->FieldSize(*M, F);
        if (Index >= static_cast<size_t>(Count))
            return {};

        const google::protobuf::Message& NestedMsg = R->GetRepeatedMessage(*M, F, static_cast<int>(Index));
        return FProtobufViewAdapter(&NestedMsg, false);
    }


    // ============================================================
    // IsValid
    // ------------------------------------------------------------
    // Returns true if the current adapter contains a valid message,
    // descriptor, and reflection instance.
    // ============================================================
    bool FProtobufViewAdapter::IsValid() const {
        const auto* M = GetMsg();
        const auto* D = GetDesc();
        const auto* R = GetRefl();

        return (M != nullptr && D != nullptr && R != nullptr);
    }


    // ============================================================
    // GetNestedStruct
    // ------------------------------------------------------------
    // Returns a nested IBinaryNodeView for a singular message field
    // (non-repeated). Used for nested structures or sub-messages.
    // ============================================================
    FProtobufViewAdapter FProtobufViewAdapter::GetNestedStruct(const VtxDiff::FieldDesc& Field) const {
        const auto* M = GetMsg();
        const auto* D = GetDesc();
        const auto* R = GetRefl();
        if (!M || !D || !R)
            return {};
        const auto* F = D->FindFieldByName(Field.name);
        if (!F || F->is_repeated() || F->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE)
            return {};
        if (!R->HasField(*M, F))
            return {};

        const google::protobuf::Message& Nested = R->GetMessage(*M, F);
        return FProtobufViewAdapter(&Nested, false);
    }


    // ============================================================================
    // PbViewFactory implementation
    // ============================================================================

    void PbViewFactory::SetSubArrayNames(const std::unordered_map<std::string, SubArrayNames>& Map) {
        SubNames = std::move(Map);
    }

    std::optional<FProtobufViewAdapter> PbViewFactory::CreateRoot(std::span<const std::byte> Buffer) const {
        if (RootType.empty() || Buffer.empty())
            return std::nullopt;
        const DescriptorPool* Pool = UsePool ? UsePool : DescriptorPool::generated_pool();
        const Descriptor* RootDesc = Pool->FindMessageTypeByName(RootType);
        if (!RootDesc)
            return std::nullopt;

        const uint8_t* DataPtr = reinterpret_cast<const uint8_t*>(Buffer.data());
        const size_t DataSize = Buffer.size();

        auto View = FProtobufViewAdapter::CreateRoot(Pool, RootType, DataPtr, DataSize);
        if (!View)
            return std::nullopt;

        if (!SubNames.empty())
            View->SetSubArrayNames(SubNames);
        return View;
    }


    bool PbViewFactory::InitFromFile(const std::string& Path, const std::string& InRootType) {
        RootType = InRootType;

        // File-based descriptor loading not supported; use InitFromMemory instead
        return false;
    }

    bool PbViewFactory::InitFromMemory(const uint8_t* Data, size_t Size, const std::string& InRootType) {
        RootType = InRootType;

        // If no data provided, use the default generated pool
        if (!Data || Size == 0) {
            UsePool = DescriptorPool::generated_pool();
            return true;
        }

        // Attempt to parse a serialized FileDescriptorSet from memory
        FileDescriptorSet DescriptorSet;
        if (!DescriptorSet.ParseFromArray(Data, static_cast<int>(Size))) {
            VTX_ERROR("PbViewFactory: Failed to parse FileDescriptorSet; falling back to generated_pool().");
            UsePool = DescriptorPool::generated_pool();
            return true;
        }

        // Build a custom DescriptorPool dynamically from the FileDescriptorSet
        auto NewPool = std::make_unique<DescriptorPool>();
        for (int I = 0; I < DescriptorSet.file_size(); ++I) {
            const FileDescriptor* FileDesc = NewPool->BuildFile(DescriptorSet.file(I));
            if (!FileDesc) {
                VTX_ERROR("PbViewFactory: Failed to build FileDescriptor (index {}); using generated_pool().", I);
                UsePool = DescriptorPool::generated_pool();
                return true;
            }
        }

        // Store and activate the newly built pool
        OwnedPool = std::move(NewPool);
        UsePool = OwnedPool.get();
        return true;
    }

} // namespace VtxDiff::Protobuf
