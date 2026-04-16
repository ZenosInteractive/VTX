#include "format/json_format.h"

namespace VtxCli {

    JsonFormat& JsonFormat::BeginObject()
    {
        WriteSeparator();
        stream_ << '{';
        first_.push_back(true);
        return *this;
    }
    
    JsonFormat& JsonFormat::EndObject()
    {
        first_.pop_back();
        stream_ << '}';
        return *this;
    }
    
    JsonFormat& JsonFormat::BeginArray()
    {
        WriteSeparator();
        stream_ << '[';
        first_.push_back(true);
        return *this;
    }
    
    JsonFormat& JsonFormat::EndArray()
    {
        first_.pop_back();
        stream_ << ']';
        return *this;
    }
    
    JsonFormat& JsonFormat::Key(const std::string& key)
    {
        WriteSeparator();
        stream_ << '"' << Escape(key) << "\":";
        first_.back() = true;
        return *this;
    }
    
    JsonFormat& JsonFormat::WriteBool(bool value)
    {
        WriteSeparator();
        stream_ << (value ? "true" : "false");
        return *this;
    }
        
    JsonFormat& JsonFormat::WriteInt(int32_t value)
    {
        WriteSeparator();
        stream_ << value;
        return *this;
    }
    
    JsonFormat& JsonFormat::WriteUInt(uint32_t value)
    {
        WriteSeparator();
        stream_ << value;
        return *this;
    }
    
    JsonFormat& JsonFormat::WriteInt64(int64_t value)
    {
        WriteSeparator();
        stream_ << value;
        return *this;
    }
    
    JsonFormat& JsonFormat::WriteUInt64(uint64_t value)
    {
        WriteSeparator();
        stream_ << value;
        return *this;
    }
    
    JsonFormat& JsonFormat::WriteFloat(float value)
    {
        WriteSeparator();
        //dont include nan or infinete in json
        if (std::isnan(value) || std::isinf(value))
        {
            stream_ << "null";
            return *this;
        }
        char buff[32];
        snprintf(buff, sizeof(buff), "%.6g", static_cast<double>(value));
        stream_ << buff;
        return *this;
    }
    
    JsonFormat& JsonFormat::WriteDouble(double value)
    {
        WriteSeparator();
        //dont include nan or infinete in json
        if (std::isnan(value) || std::isinf(value))
        {
            stream_ << "null";
            return *this;
        }
        char buff[32];
        snprintf(buff, sizeof(buff), "%.10g", static_cast<double>(value));
        stream_ << buff;
        return *this;
    }
    
    JsonFormat& JsonFormat::WriteString(const std::string& value)
    {
        WriteSeparator();
        stream_ << '"' << Escape(value) << '"';
        return *this;
    }
    
    JsonFormat& JsonFormat::WriteNull()
    {
        WriteSeparator();
        stream_ << "null";
        return *this;
    }
    
    JsonFormat& JsonFormat::WriteRaw(const std::string& value)
    {
        WriteSeparator();
        stream_ << value;
        return *this;
    }
    
    std::string JsonFormat::Finalize(const std::string& optional_data)
    {
        return stream_.str();
    }
    

    void JsonFormat::WriteSeparator()
    {
        if (first_.empty() == false)
        {
            if (first_.back() == false)
            {
                stream_ << ',';
            }
            else
            {
                first_.back() = false;
            }
        }
    }
    
    std::string JsonFormat::Escape(const std::string& s) {
        std::string result;
    
        result.reserve(s.size());

        for (char c : s) {
            unsigned char uc = static_cast<unsigned char>(c);

            switch (uc) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            default:
                if (uc < 0x20) {
                    char buf[7]; 
                    snprintf(buf, sizeof(buf), "\\u%04x", uc);
                    result += buf;
                } else {
                    result += c;
                }
                break;
            }
        }

        return result;
    }
    

} // namespace VtxCli
