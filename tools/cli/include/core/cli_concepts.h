#pragma once
#include <concepts>
#include <string>
#include <optional>
#include <cstdint>

namespace VtxCli {

    template<typename T>
    concept FormatWriter = requires(T writer, 
                                    bool bool_value,
                                    int32_t int32_value,
                                    uint32_t uint32_value,
                                    int64_t int64_value,
                                    uint64_t uint64_value,
                                    float float_value,
                                    double double_value,
                                    const std::string& string_value
                            )
    {
        //structure
        {writer.BeginObject()}->std::same_as<T&>;
        {writer.EndObject()}->std::same_as<T&>;
        {writer.BeginArray()}->std::same_as<T&>;
        {writer.EndArray()}->std::same_as<T&>;
        //key
        {writer.Key(string_value)}-> std::same_as<T&>;
        //primitive values
        {writer.WriteBool(bool_value)};
        {writer.WriteInt(int32_value)};
        {writer.WriteUInt(uint32_value)};
        {writer.WriteInt64(int64_value)};
        {writer.WriteUInt64(uint64_value)};
        {writer.WriteFloat(float_value)};
        {writer.WriteDouble(double_value)};
        {writer.WriteString(string_value)};
        {writer.WriteNull()};
        
        //To pass preformatted content
        {writer.WriteRaw(string_value)} -> std::same_as<T&>;
        
        //Finelize
        {writer.Finalize(string_value)} ->std::convertible_to<std::string>;//convertible_to allows to return const string&
            
    };
    
    template<typename T>
    concept Transport = requires(T transport,
                                const std::string& string_value)
    {
        {transport.ReadLine()}->std::convertible_to<std::optional<std::string>>;
        {transport.WriteLine(string_value)};
        {transport.IsOpen() }   -> std::convertible_to<bool>;
    };

} // namespace VtxCli
