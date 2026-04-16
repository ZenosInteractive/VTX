#include "core/cli_engine.h"
#include "format/json_format.h"
#include "transport/stdio_transport.h"
#include "transport/tcp_transport.h"

#include <iostream>
#include <string>
#include <cstdlib>

using DefaultEngine = VtxCli::CliEngine<VtxCli::JsonFormat, VtxCli::StdioTransport>;

static void PrintUsage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --help, -h          Show this help and exit\n"
        << "  --version           Print version and exit\n"
        << "  --verbose, -v       Enable human-readable banner on startup\n"
        << "  --port, -p <port>   Listen on TCP port instead of stdin/stdout\n"
        << "\n"
        << "Interactive commands:\n";
    DefaultEngine::PrintCommandHelp(std::cerr);
}

int main(int argc, char* argv[])
{
    bool verbose = false;
    int port = -1;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        }
        else if (arg == "--version") {
            std::cout << "vtx_cli " << VtxCli::kCliVersion << "\n";
            return 0;
        }
        else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        }
        else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        }
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (port > 0) {
        VtxCli::CliEngine<VtxCli::JsonFormat, VtxCli::TcpTransport> engine(
            static_cast<uint16_t>(port), verbose);
        engine.Run();
    } else {
        VtxCli::CliEngine<VtxCli::JsonFormat, VtxCli::StdioTransport> engine(
            verbose);
        engine.Run();
    }

    return 0;
}
