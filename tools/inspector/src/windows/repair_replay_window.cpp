#include "windows/repair_replay_window.h"

#include <chrono>
#include <filesystem>
#include <utility>

#include <imgui.h>

#include "gui/portable-file-dialogs.h"

namespace {
    const ImVec4 kOk {0.40f, 0.85f, 0.45f, 1.0f};
    const ImVec4 kWarn {0.95f, 0.80f, 0.30f, 1.0f};
    const ImVec4 kErr {0.95f, 0.45f, 0.45f, 1.0f};
    const ImVec4 kDim {0.65f, 0.65f, 0.65f, 1.0f};
} // namespace

void RepairReplayWindow::OnRender() {
    if (!is_open_)
        return;
    // Keep Begin/End paired even if content is skipped; the title-bar close button
    // drives is_open_ -> the layout prunes the window next frame.
    if (ImGui::Begin("Repair Replay", &is_open_)) {
        DrawContent();
    }
    ImGui::End();
}

void RepairReplayWindow::PickVtx() {
    auto f = pfd::open_file("Select the crashed .vtx", ".", {"VTX Files (.vtx)", "*.vtx", "All Files", "*"});
    if (!f.result().empty()) {
        vtx_path_ = f.result()[0];
        // Auto-fill the sidecar RepairReplayFile actually reads: "<path>.recovery".
        recovery_path_ = VTX::RecoveryJournalPath(vtx_path_);
    }
}

void RepairReplayWindow::PickRecovery() {
    auto f = pfd::open_file("Select the .vtx.recovery sidecar", ".",
                            {"Recovery sidecar (*.recovery)", "*.recovery", "All Files", "*"});
    if (!f.result().empty())
        recovery_path_ = f.result()[0];
}

RepairReplayWindow::Outcome RepairReplayWindow::RunRepair(std::string vtx_path, std::string recovery_path) {
    // Touches only its by-value arguments and locals -- safe to run detached from the
    // window instance even if the window is closed mid-repair.
    Outcome out;
    namespace fs = std::filesystem;
    std::error_code ec;

    // RepairReplayFile always reads the sidecar adjacent to the .vtx ("<path>.recovery").
    // If the user picked a sidecar living elsewhere, stage a copy next to the .vtx --
    // but never clobber a sidecar already sitting there.
    const std::string expected = VTX::RecoveryJournalPath(vtx_path);
    bool staged = false;
    if (recovery_path != expected) {
        if (fs::exists(expected, ec)) {
            out.result.error = "A sidecar already exists next to the .vtx (" + expected +
                               "). Remove it, or open the .vtx that sits beside the sidecar you want to use.";
            return out;
        }
        fs::copy_file(recovery_path, expected, ec);
        if (ec) {
            out.result.error = "Could not stage the sidecar next to the .vtx: " + ec.message();
            return out;
        }
        staged = true;
        out.note = "Sidecar was copied next to the .vtx for the repair.";
    }

    out.result = VTX::RepairReplayFile(vtx_path);

    // On success RepairReplayFile deletes the (staged) sidecar itself; on failure remove
    // our temp copy so the filesystem is left as we found it (the user's original
    // sidecar elsewhere is untouched either way).
    if (staged && fs::exists(expected, ec))
        fs::remove(expected, ec);

    return out;
}

void RepairReplayWindow::StartRepair() {
    phase_ = Phase::Running;
    spinner_tick_ = 0;
    outcome_ = Outcome {};
    future_ = std::async(std::launch::async, &RepairReplayWindow::RunRepair, vtx_path_, recovery_path_);
}

void RepairReplayWindow::DrawContent() {
    // Collect the worker's result once it's ready (non-blocking poll).
    if (phase_ == Phase::Running && future_.valid() &&
        future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        outcome_ = future_.get();
        phase_ = Phase::Done;
    }

    ImGui::TextWrapped("Reconstruct a crashed .vtx from its \".recovery\" sidecar left by an "
                       "interrupted recording.");
    ImGui::Spacing();

    namespace fs = std::filesystem;
    std::error_code ec;
    const bool busy = (phase_ == Phase::Running);

    ImGui::BeginDisabled(busy);

    ImGui::TextUnformatted("Replay file (.vtx)");
    if (ImGui::Button("Browse...##vtx"))
        PickVtx();
    ImGui::SameLine();
    if (vtx_path_.empty())
        ImGui::TextColored(kDim, "(none selected)");
    else
        ImGui::TextWrapped("%s", vtx_path_.c_str());

    ImGui::Spacing();

    ImGui::TextUnformatted("Recovery sidecar (.vtx.recovery)");
    if (ImGui::Button("Browse...##rec"))
        PickRecovery();
    ImGui::SameLine();
    if (recovery_path_.empty())
        ImGui::TextColored(kDim, "(auto-filled when you pick the .vtx)");
    else
        ImGui::TextWrapped("%s", recovery_path_.c_str());

    ImGui::EndDisabled();

    const bool vtx_ok = !vtx_path_.empty() && fs::exists(vtx_path_, ec);
    const bool rec_ok = !recovery_path_.empty() && fs::exists(recovery_path_, ec);
    if (!vtx_path_.empty() && !vtx_ok)
        ImGui::TextColored(kWarn, "The .vtx path does not exist on disk.");
    if (!recovery_path_.empty() && !rec_ok)
        ImGui::TextColored(kWarn, "The .recovery sidecar does not exist on disk.");

    ImGui::Separator();

    const bool can_repair = vtx_ok && rec_ok && !busy;
    ImGui::BeginDisabled(!can_repair);
    if (ImGui::Button("Repair", ImVec2(120, 0)))
        StartRepair();
    ImGui::EndDisabled();

    if (phase_ == Phase::Done) {
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            phase_ = Phase::Idle;
            outcome_ = Outcome {};
        }
    }

    ImGui::Spacing();

    // Progress / result panel.
    if (phase_ == Phase::Running) {
        ++spinner_tick_;
        const char* dots[] = {"", ".", "..", "..."};
        ImGui::TextColored(kWarn, "Repairing%s", dots[(spinner_tick_ / 15) % 4]);
        ImGui::TextColored(kDim, "Verifying chunks and rebuilding the footer. This may take a "
                                 "moment for large replays.");
    } else if (phase_ == Phase::Done) {
        const VTX::RepairResult& r = outcome_.result;
        if (!r.ok()) {
            ImGui::TextColored(kErr, "Repair FAILED");
            ImGui::TextWrapped("%s", r.error.c_str());
        } else if (r.was_clean && !r.repaired) {
            ImGui::TextColored(kOk, "Nothing to repair");
            ImGui::TextWrapped("The file was already complete (no crash journal to apply). "
                               "It is safe to open as-is.");
        } else {
            ImGui::TextColored(kOk, "Repair SUCCEEDED");
            ImGui::Text("Recovered %d chunk(s), %d frame(s).", r.recovered_chunks, r.recovered_frames);
            ImGui::TextColored(kDim, "The .vtx now has a valid footer and opens normally; the "
                                     "sidecar has been consumed.");
        }
        if (!outcome_.note.empty())
            ImGui::TextColored(kDim, "%s", outcome_.note.c_str());
    }
}
