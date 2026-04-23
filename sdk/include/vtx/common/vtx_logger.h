#pragma once
#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace VTX {

    // ==========================================================
    // Class: DiffLogger
    // ----------------------------------------------------------
    // Thread-safe, lightweight logging utility for the diff engine.
    // Replicates UE_LOG macros
    // ==========================================================

    constexpr const char* ANSI_COLOR_RESET   = "\033[0m";
    constexpr const char* ANSI_COLOR_INFO    = "\033[32m"; // Green
    constexpr const char* ANSI_COLOR_WARN    = "\033[33m"; // Yellow
    constexpr const char* ANSI_COLOR_ERROR   = "\033[31m"; // Red
    constexpr const char* ANSI_COLOR_DEBUG   = "\033[36m"; // Cian

    class Logger {
    public:
        enum class Level {
            Info,
            Warning,
            Error,
            Debug
        };

        struct Entry {
            Level level = Level::Info;
            std::string timestamp;
            std::string message;
        };

        using SinkId = uint64_t;
        using SinkCallback = std::function<void(const Entry&)>;

        static Logger& Instance() {
            static Logger Instance;
            return Instance;
        }

        SinkId AddSink(SinkCallback callback) {
            std::scoped_lock lock(mute_);
            const SinkId sink_id = next_sink_id_++;
            sinks_.emplace(sink_id, std::move(callback));
            return sink_id;
        }

        void RemoveSink(SinkId sink_id) {
            std::scoped_lock lock(mute_);
            sinks_.erase(sink_id);
        }

        // Suppress Debug-level messages globally (Info / Warning / Error still
        // print).  Useful for benchmarks, production deployments, or noisy
        // workloads where per-chunk trace output would dominate timings.
        // Thread-safe; can be toggled any time.
        void SetDebugEnabled(bool enabled) {
            debug_enabled_.store(enabled, std::memory_order_relaxed);
        }

        bool IsDebugEnabled() const {
            return debug_enabled_.load(std::memory_order_relaxed);
        }

        void Log(Level LogLvl, const std::string& Message) {
            if (LogLvl == Level::Debug && !IsDebugEnabled()) {
                return;
            }

            std::vector<SinkCallback> sinks_copy;
            Entry entry;

            {
                std::scoped_lock lock(mute_);

                const auto now = std::chrono::system_clock::now();
                const auto time = std::chrono::system_clock::to_time_t(now);

                std::ostringstream timestamp_stream;
                timestamp_stream << std::put_time(std::localtime(&time), "%H:%M:%S");

                entry.level = LogLvl;
                entry.timestamp = timestamp_stream.str();
                entry.message = Message;

                std::cout << GetColorCode(LogLvl)
                          << "[" << entry.timestamp << "] "
                          << "[" << ToString(LogLvl) << "] "
                          << Message
                          << ANSI_COLOR_RESET << std::endl;

                sinks_copy.reserve(sinks_.size());
                for (const auto& [_, sink] : sinks_) {
                    sinks_copy.push_back(sink);
                }
            }

            for (const auto& sink : sinks_copy) {
                sink(entry);
            }
        }

        template<typename... Args>
        void Info(std::string_view format_str, Args&&... args) {
            Log(Level::Info, std::vformat(format_str, std::make_format_args(args...)));
        }

        template<typename... Args>
        void Warn(std::string_view format_str, Args&&... args) {
            Log(Level::Warning, std::vformat(format_str, std::make_format_args(args...)));
        }

        template<typename... Args>
        void Error(std::string_view format_str, Args&&... args) {
            Log(Level::Error, std::vformat(format_str, std::make_format_args(args...)));
        }

        template<typename... Args>
        void Debug(std::string_view format_str, Args&&... args) {
            Log(Level::Debug, std::vformat(format_str, std::make_format_args(args...)));
        }
    private:
        std::mutex mute_;
        std::unordered_map<SinkId, SinkCallback> sinks_;
        SinkId next_sink_id_ = 1;
        std::atomic<bool> debug_enabled_{true};

        template<typename ...Args>
        void LogFormatted(Level log_level, const char* message, Args... args) {
            if constexpr (sizeof...(args) == 0) {
                Log(log_level, message);
                return;
            }

            int size = std::snprintf(nullptr, 0, message, args...);
            if (size <= 0) {
                Log(log_level, "");
                return;
            }

            std::string buffer(size + 1, '\0');
            std::snprintf(&buffer[0], size + 1, message, args...);
            buffer.resize(size);

            Log(log_level, buffer);
        }

        const char* ToString(Level LogLvl) const {
            switch (LogLvl) {
            case Level::Info:    return "INFO";
            case Level::Warning: return "WARN";
            case Level::Error:   return "ERROR";
            case Level::Debug:   return "DEBUG";
            default:             return "LOG";
            }
        }

        const char* GetColorCode(Level LogLvl) const {
            switch (LogLvl) {
            case Level::Info:    return ANSI_COLOR_INFO;
            case Level::Warning: return ANSI_COLOR_WARN;
            case Level::Error:   return ANSI_COLOR_ERROR;
            case Level::Debug:   return ANSI_COLOR_DEBUG;
            default:             return ANSI_COLOR_RESET;
            }
        }

    };
    #define VTX_INFO(...)  VTX::Logger::Instance().Info(__VA_ARGS__)
    #define VTX_WARN(...)  VTX::Logger::Instance().Warn(__VA_ARGS__)
    #define VTX_ERROR(...) VTX::Logger::Instance().Error(__VA_ARGS__)
    #define VTX_DEBUG(...) VTX::Logger::Instance().Debug(__VA_ARGS__)
} // namespace VTX
