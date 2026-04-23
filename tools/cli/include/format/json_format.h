#pragma once
#include <sstream>
#include <cmath>
#include <vector>

#include "core/cli_concepts.h"
namespace VtxCli {


    class JsonFormat {
    public:
        JsonFormat& BeginObject();
        JsonFormat& EndObject();

        JsonFormat& BeginArray();
        JsonFormat& EndArray();

        JsonFormat& Key(const std::string& key);

        JsonFormat& WriteBool(bool value);
        JsonFormat& WriteInt(int32_t value);
        JsonFormat& WriteUInt(uint32_t value);
        JsonFormat& WriteInt64(int64_t value);
        JsonFormat& WriteUInt64(uint64_t value);
        JsonFormat& WriteFloat(float value);
        JsonFormat& WriteDouble(double value);
        JsonFormat& WriteString(const std::string& value);
        JsonFormat& WriteNull();
        JsonFormat& WriteRaw(const std::string& value);
        std::string Finalize(const std::string& optional_data);

    private:
        void WriteSeparator();
        static std::string Escape(const std::string& s);

        std::ostringstream stream_;
        std::vector<bool> first_;
    };
    static_assert(FormatWriter<JsonFormat>, "JsonFormat must satisfy FormatWriter");
} // namespace VtxCli
