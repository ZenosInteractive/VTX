#include "session/vtx_session_base.h"

#include <vector>

VtxSessionBase::VtxSessionBase() {
    logger_sink_id_ = VTX::Logger::Instance().AddSink([this](const VTX::Logger::Entry& entry) { AppendLog(entry); });

    AddGuiInfoLog("Log sink connected to VTX::Logger.");
}

VtxSessionBase::~VtxSessionBase() {
    if (logger_sink_id_ != 0) {
        VTX::Logger::Instance().RemoveSink(logger_sink_id_);
    }
}

void VtxSessionBase::ClearLogs() {
    std::lock_guard<std::mutex> lock(logs_mutex_);
    logs_.clear();
    next_log_sequence_ = 1;
}

std::vector<VtxLogEntry> VtxSessionBase::GetLogsSnapshot() const {
    std::lock_guard<std::mutex> lock(logs_mutex_);
    return std::vector<VtxLogEntry>(logs_.begin(), logs_.end());
}

void VtxSessionBase::AddGuiInfoLog(const std::string& message) {
    VTX_INFO("[GUI] {}", message);
}

void VtxSessionBase::AddGuiWarningLog(const std::string& message) {
    VTX_WARN("[GUI] {}", message);
}

void VtxSessionBase::AddGuiErrorLog(const std::string& message) {
    VTX_ERROR("[GUI] {}", message);
}

void VtxSessionBase::RecordRecentFile(const std::string& path) {
    if (path.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(recent_files_mutex_);

    for (auto it = recent_files_.begin(); it != recent_files_.end(); ++it) {
        if (*it == path) {
            recent_files_.erase(it);
            break;
        }
    }

    recent_files_.push_front(path);
    while (recent_files_.size() > kMaxRecentFiles) {
        recent_files_.pop_back();
    }
}

std::vector<std::string> VtxSessionBase::GetRecentFilesSnapshot() const {
    std::lock_guard<std::mutex> lock(recent_files_mutex_);
    return std::vector<std::string>(recent_files_.begin(), recent_files_.end());
}

void VtxSessionBase::AppendLog(const VTX::Logger::Entry& entry) {
    std::lock_guard<std::mutex> lock(logs_mutex_);

    logs_.push_back(VtxLogEntry {next_log_sequence_++, entry.level, entry.timestamp, entry.message});

    while (logs_.size() > kMaxLogEntries) {
        logs_.pop_front();
    }
}
