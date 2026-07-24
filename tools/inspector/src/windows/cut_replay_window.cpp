#include "windows/cut_replay_window.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <utility>

#include <imgui.h>

#include "gui/portable-file-dialogs.h"
#include "inspector_session.h"
#include "services/time_display_service.h"
#include "services/timeline_view_service.h"

namespace {

    const ImVec4 kOk {0.40f, 0.85f, 0.45f, 1.0f};
    const ImVec4 kWarn {0.95f, 0.80f, 0.30f, 1.0f};
    const ImVec4 kErr {0.95f, 0.45f, 0.45f, 1.0f};
    const ImVec4 kDim {0.65f, 0.65f, 0.65f, 1.0f};

    constexpr int64_t kTicksPerSecond = 10'000'000;

    // Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
    int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
        y -= m <= 2;
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int64_t>(doe) - 719468;
    }

    // Accepts an ISO-8601 UTC timestamp ("2026-07-24T09:50:29.195Z", 'T' or
    // space, 'Z' optional, fractional seconds optional) or a bare number
    // (unix-epoch 100ns ticks, unix milliseconds, or unix seconds -- picked by
    // magnitude). Returns unix-epoch 100ns ticks.
    bool ParseUtcInput(const char* text, int64_t& ticks_out) {
        if (!text || !*text) {
            return false;
        }

        int year = 0, month = 0, day = 0, hour = 0, minute = 0;
        double seconds = 0.0;
        if (std::sscanf(text, "%d-%d-%d%*1[T ]%d:%d:%lf", &year, &month, &day, &hour, &minute, &seconds) == 6) {
            if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || seconds >= 61.0) {
                return false;
            }
            const int64_t days = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
            const double unix_seconds = static_cast<double>(days) * 86400.0 + hour * 3600.0 + minute * 60.0 + seconds;
            ticks_out = static_cast<int64_t>(unix_seconds * static_cast<double>(kTicksPerSecond));
            return true;
        }

        char* end = nullptr;
        const double value = std::strtod(text, &end);
        if (end == text || value <= 0.0) {
            return false;
        }
        if (value > 1.0e15) { // already 100ns ticks
            ticks_out = static_cast<int64_t>(value);
        } else if (value > 1.0e11) { // unix milliseconds
            ticks_out = static_cast<int64_t>(value * 10'000.0);
        } else { // unix seconds
            ticks_out = static_cast<int64_t>(value * static_cast<double>(kTicksPerSecond));
        }
        return true;
    }

    // First nonzero tick of the table the timeline uses (created_utc preferred).
    int64_t BaseTick(const VTX::ReplayTimeData& times) {
        for (const uint64_t tick : times.created_utc) {
            if (tick != 0) {
                return static_cast<int64_t>(tick);
            }
        }
        for (const uint64_t tick : times.game_time) {
            if (tick != 0) {
                return static_cast<int64_t>(tick);
            }
        }
        return 0;
    }

    std::string FormatClock(float seconds) {
        const auto clock = VtxServices::TimelineViewService::ToClockTime(seconds);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d", clock.minutes, clock.seconds);
        return buffer;
    }

} // namespace

CutReplayWindow::CutReplayWindow(std::shared_ptr<InspectorSession> session)
    : session_(std::move(session)) {}

void CutReplayWindow::OnRender() {
    if (!is_open_)
        return;
    if (ImGui::Begin("Cut Replay", &is_open_)) {
        DrawContent();
    }
    ImGui::End();
}

void CutReplayWindow::ResetRangeToFullReplay() {
    const auto& footer = session_->GetFooter();
    const int total_frames = footer.total_frames;
    frame_start_ = 0;
    frame_end_ = std::max(total_frames - 1, 0);
    time_start_seconds_ = 0.0;
    time_end_seconds_ = static_cast<double>(footer.duration_seconds);
    utc_start_[0] = '\0';
    utc_end_[0] = '\0';
    if (total_frames > 0) {
        const auto tick_at = [&](int frame) -> int64_t {
            const auto& utc = footer.times.created_utc;
            for (int i = std::min(frame, static_cast<int>(utc.size()) - 1); i >= 0; --i) {
                if (utc[static_cast<size_t>(i)] != 0) {
                    return static_cast<int64_t>(utc[static_cast<size_t>(i)]);
                }
            }
            return 0;
        };
        const int64_t first = tick_at(0) != 0 ? tick_at(0) : BaseTick(footer.times);
        const int64_t last = tick_at(total_frames - 1);
        if (first != 0) {
            std::snprintf(utc_start_, sizeof(utc_start_), "%" PRId64, first);
        }
        if (last != 0) {
            std::snprintf(utc_end_, sizeof(utc_end_), "%" PRId64, last);
        }
    }
}

bool CutReplayWindow::ResolveRequestedFrames(int& start_frame, int& end_frame, std::string& error_out) const {
    const auto& footer = session_->GetFooter();
    const int total_frames = footer.total_frames;
    const float duration = footer.duration_seconds;

    const auto frame_at_seconds = [&](double seconds) {
        return VtxServices::TimelineViewService::FrameAtElapsedSeconds(static_cast<float>(seconds), footer.times,
                                                                       total_frames, duration);
    };

    switch (range_mode_) {
    case 0: // time
        if (time_end_seconds_ < time_start_seconds_) {
            error_out = "End time is before start time.";
            return false;
        }
        start_frame = frame_at_seconds(time_start_seconds_);
        end_frame = frame_at_seconds(time_end_seconds_);
        return true;
    case 1: // frame
        start_frame = frame_start_;
        end_frame = frame_end_;
        return true;
    default: { // UTC
        int64_t start_ticks = 0;
        int64_t end_ticks = 0;
        if (!ParseUtcInput(utc_start_, start_ticks) || !ParseUtcInput(utc_end_, end_ticks)) {
            error_out = "UTC bounds must be ISO-8601 (2026-07-24T09:50:29.195Z) or a unix seconds/ms/ticks number.";
            return false;
        }
        if (end_ticks < start_ticks) {
            error_out = "UTC end is before UTC start.";
            return false;
        }
        const int64_t base = BaseTick(footer.times);
        if (base == 0) {
            error_out = "The replay has no time table; use the Frame range mode.";
            return false;
        }
        start_frame = frame_at_seconds(static_cast<double>(start_ticks - base) / kTicksPerSecond);
        end_frame = frame_at_seconds(static_cast<double>(end_ticks - base) / kTicksPerSecond);
        return true;
    }
    }
}

CutReplayWindow::Outcome CutReplayWindow::RunCut(std::string source_path, VTX::FileFooter footer,
                                                 VtxServices::ReplayCutPlan plan, std::string dest_path) {
    // Touches only its by-value arguments -- safe to run detached from the window.
    Outcome out;
    out.dest_path = dest_path;
    out.ok = VtxServices::ReplayCutService::ExecuteCut(source_path, footer, plan, dest_path, out.error);
    return out;
}

void CutReplayWindow::StartCut(const VtxServices::ReplayCutPlan& plan, const std::string& dest_path) {
    phase_ = Phase::Running;
    spinner_tick_ = 0;
    outcome_ = Outcome {};
    future_ = std::async(std::launch::async, &CutReplayWindow::RunCut, session_->current_file_path_,
                         session_->GetFooter(), plan, dest_path);
}

void CutReplayWindow::DrawContent() {
    if (phase_ == Phase::Running && future_.valid() &&
        future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        outcome_ = future_.get();
        phase_ = Phase::Done;
    }

    if (!session_->HasLoadedReplay()) {
        ImGui::TextDisabled("Load a replay first; Cut works on the currently loaded .vtx.");
        range_initialized_ = false;
        return;
    }
    if (session_->GetFormat() != VTX::VtxFormat::FlatBuffers) {
        ImGui::TextColored(kWarn, "Cut currently supports flatbuffers (.vtx \"VTXF\") replays only.");
        return;
    }
    if (!range_initialized_) {
        ResetRangeToFullReplay();
        range_initialized_ = true;
    }

    const auto& footer = session_->GetFooter();
    ImGui::TextWrapped("Write a new .vtx containing a sub-range of the loaded replay. The range snaps to "
                       "whole chunks: chunks are copied verbatim, and the footer is rebuilt for the cut.");
    ImGui::Spacing();

    const bool busy = (phase_ == Phase::Running);
    ImGui::BeginDisabled(busy);

    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo("Range mode", &range_mode_, "Time (elapsed seconds)\0Frame\0UTC\0");

    if (range_mode_ == 0) {
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputDouble("Start (s)", &time_start_seconds_, 0.0, 0.0, "%.3f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputDouble("End (s)", &time_end_seconds_, 0.0, 0.0, "%.3f");
        ImGui::TextColored(kDim, "%s -> %s of %s", FormatClock(static_cast<float>(time_start_seconds_)).c_str(),
                           FormatClock(static_cast<float>(time_end_seconds_)).c_str(),
                           FormatClock(footer.duration_seconds).c_str());
    } else if (range_mode_ == 1) {
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Start frame", &frame_start_, 0, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("End frame", &frame_end_, 0, 0);
        ImGui::TextColored(kDim, "Replay frames: 0 .. %d", std::max(footer.total_frames - 1, 0));
    } else {
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputText("Start UTC", utc_start_, sizeof(utc_start_));
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputText("End UTC", utc_end_, sizeof(utc_end_));
        ImGui::TextColored(kDim, "ISO-8601 (2026-07-24T09:50:29.195Z) or unix seconds / ms / 100ns ticks.");
    }

    if (ImGui::Button("Reset to full replay")) {
        ResetRangeToFullReplay();
    }

    ImGui::EndDisabled();
    ImGui::Separator();

    // Live plan preview.
    int requested_start = 0;
    int requested_end = 0;
    std::string input_error;
    VtxServices::ReplayCutPlan plan;
    if (ResolveRequestedFrames(requested_start, requested_end, input_error)) {
        plan = VtxServices::ReplayCutService::PlanCut(footer, requested_start, requested_end);
    } else {
        plan.error = input_error;
    }

    if (!plan.valid) {
        ImGui::TextColored(kWarn, "%s", plan.error.c_str());
    } else {
        const float start_sec = VtxServices::TimelineViewService::FrameToElapsedSeconds(
            plan.first_frame, footer.times, footer.total_frames, footer.duration_seconds);
        const float end_sec = VtxServices::TimelineViewService::FrameToElapsedSeconds(
            plan.last_frame, footer.times, footer.total_frames, footer.duration_seconds);
        ImGui::Text("Cut (snapped to chunks): frames %d .. %d  (%d frames)", plan.first_frame, plan.last_frame,
                    plan.last_frame - plan.first_frame + 1);
        ImGui::Text("Time %s .. %s   |   chunks %d .. %d of %d   |   ~%.1f MB", FormatClock(start_sec).c_str(),
                    FormatClock(end_sec).c_str(), plan.first_chunk, plan.last_chunk,
                    static_cast<int>(footer.chunk_index.size()), plan.chunk_bytes / (1024.0 * 1024.0));
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!plan.valid || busy);
    if (ImGui::Button("Cut && Save As...", ImVec2(160, 0))) {
        namespace fs = std::filesystem;
        const fs::path source(session_->current_file_path_);
        const std::string default_name = source.stem().string() + "_cut.vtx";
        auto dialog = pfd::save_file("Save cut replay", (source.parent_path() / default_name).string(),
                                     {"VTX Files (.vtx)", "*.vtx", "All Files", "*"});
        std::string dest = dialog.result();
        if (!dest.empty()) {
            if (fs::path(dest).extension().empty()) {
                dest += ".vtx";
            }
            StartCut(plan, dest);
        }
    }
    ImGui::EndDisabled();

    if (phase_ == Phase::Done) {
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            phase_ = Phase::Idle;
            outcome_ = Outcome {};
        }
    }

    ImGui::Spacing();
    if (phase_ == Phase::Running) {
        ++spinner_tick_;
        const char* dots[] = {"", ".", "..", "..."};
        ImGui::TextColored(kWarn, "Cutting%s", dots[(spinner_tick_ / 15) % 4]);
        ImGui::TextColored(kDim, "Copying chunks and rebuilding the footer.");
    } else if (phase_ == Phase::Done) {
        if (!outcome_.ok) {
            ImGui::TextColored(kErr, "Cut FAILED");
            ImGui::TextWrapped("%s", outcome_.error.c_str());
        } else {
            ImGui::TextColored(kOk, "Cut SUCCEEDED");
            ImGui::TextWrapped("Wrote %s", outcome_.dest_path.c_str());
            ImGui::TextColored(kDim, "The new file opens like any other replay; its frames are renumbered "
                                     "from 0 and UTC timestamps stay absolute.");
        }
    }
}
