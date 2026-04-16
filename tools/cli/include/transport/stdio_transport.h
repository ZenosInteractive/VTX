#pragma once
#include <iostream>

#include "core/cli_concepts.h"

namespace VtxCli {

    class StdioTransport
    {
    public:
        explicit StdioTransport(bool verbose = false, bool json_only = false)
            : verbose_(verbose), json_only_(json_only) {}

        std::optional<std::string> ReadLine()
        {
            std::string line;
            if(std::getline(std::cin,line))
            {
                return line;
            }
            return std::nullopt;
        }

        void WriteLine(const std::string& value)
        {
            std::cout << value << '\n';
            std::cout.flush();
        }

        void WriteLog(const std::string& value)
        {
            if (json_only_) {
                std::cerr << value << '\n';
                std::cerr.flush();
            } else {
                std::cout << value << '\n';
                std::cout.flush();
            }
        }

        bool IsOpen()
        {
          return !std::cin.eof();
        }

        bool IsVerbose() const { return verbose_; }
        bool IsJsonOnly() const { return json_only_; }

    private:
        bool verbose_ = false;
        bool json_only_ = false;
    };

    static_assert(Transport<StdioTransport>, "StdioTransport must satisfy Transport");

} // namespace VtxCli
