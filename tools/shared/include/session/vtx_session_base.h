#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "vtx/common/vtx_logger.h"

struct VtxLogEntry {
    uint64_t sequence = 0;
    VTX::Logger::Level level = VTX::Logger::Level::Info;
    std::string timestamp;
    std::string message;
};

class VtxSessionBase {
public:
    VtxSessionBase();
    virtual ~VtxSessionBase();

    void ClearLogs();
    std::vector<VtxLogEntry> GetLogsSnapshot() const;
    void AddGuiInfoLog(const std::string& message);
    void AddGuiWarningLog(const std::string& message);
    void AddGuiErrorLog(const std::string& message);
    void RecordRecentFile(const std::string& path);
    std::vector<std::string> GetRecentFilesSnapshot() const;

    bool is_file_loaded_ = false;
    std::string current_file_path_;

protected:
    void AppendLog(const VTX::Logger::Entry& entry);

private:
    mutable std::mutex logs_mutex_;
    std::deque<VtxLogEntry> logs_;
    uint64_t next_log_sequence_ = 1;
    VTX::Logger::SinkId logger_sink_id_ = 0;
    mutable std::mutex recent_files_mutex_;
    std::deque<std::string> recent_files_;
    static constexpr size_t kMaxLogEntries = 1000;
    static constexpr size_t kMaxRecentFiles = 10;
};
