/**
* @file protobuff_adapter.h
 *
 * @brief Policies and logic for binding C++ structures to protobuff tables.
 *
 * @details This file implements a static reflection mechanism specifically for
 * loading data from protobuffer binary ,essage into C++ objects (SoA or AoS).
 *
 * @author Zenos Interactive
 */

#pragma once
#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>
#include <string>
#include <vector>
#include <stdexcept>

namespace VTX {

    class ProtobufAdapter {
        const google::protobuf::Message& message_;
        const google::protobuf::FieldDescriptor* field_desc_; // Null if root or full obj

    public:
        explicit ProtobufAdapter(const google::protobuf::Message& msg)
            : message_(msg)
            , field_desc_(nullptr) {}

        //submessage constructor, used internally
        ProtobufAdapter(const google::protobuf::Message& msg, const google::protobuf::FieldDescriptor* fd)
            : message_(msg)
            , field_desc_(fd) {}

        bool HasKey(const std::string& key) const {
            const auto* descriptor = message_.GetDescriptor();
            return descriptor->FindFieldByName(key) != nullptr;
        }

        template <typename T>
        T GetValue() const {
            return T {};
        }


        ProtobufAdapter GetChild(const std::string& key) const {
            const auto* descriptor = message_.GetDescriptor();
            const auto* field = descriptor->FindFieldByName(key);

            if (!field)
                throw std::runtime_error("Field not found: " + key);

            const auto* reflection = message_.GetReflection();

            if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
                if (field->is_repeated()) {
                    return ProtobufAdapter(message_, field);
                } else {
                    const auto& sub_message = reflection->GetMessage(message_, field);
                    return ProtobufAdapter(sub_message);
                }
            }

            return ProtobufAdapter(message_, field);
        }


        template <typename T>
        T GetValueFromField() const {
            if (!field_desc)
                return T {}; // Error

            const auto* ref = message.GetReflection();

            if constexpr (std::is_same_v<T, int> || std::is_same_v<T, int32_t>) {
                return ref->GetInt32(message_, field_desc);
            } else if constexpr (std::is_same_v<T, int64_t>) {
                return ref->GetInt64(message_, field_desc);
            } else if constexpr (std::is_same_v<T, bool>) {
                return ref->GetBool(message_, field_desc);
            } else if constexpr (std::is_same_v<T, float>) {
                return ref->GetFloat(message_, field_desc);
            } else if constexpr (std::is_same_v<T, double>) {
                return ref->GetDouble(message_, field_desc);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return ref->GetString(message_, field_desc);
            }
            return T {};
        }

        template <typename T>
        T GetValue() const {
            return GetValueFromField<T>();
        }

        bool IsArray() const { return field_desc_ && field_desc_->is_repeated(); }

        size_t Size() const {
            if (IsArray()) {
                return message_.GetReflection()->FieldSize(message_, field_desc_);
            }
            return 0;
        }

        ProtobufAdapter GetElement(size_t index) const {
            if (!IsArray())
                throw std::runtime_error("Not an array");

            const auto* ref = message_.GetReflection();

            if (field_desc_->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
                const auto& elem_msg = ref->GetRepeatedMessage(message_, field_desc_, index);
                return ProtobufAdapter(elem_msg);
            }

            return ProtobufAdapter(message_, field_desc_);
        }
    };

} // namespace VTX