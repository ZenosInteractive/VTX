#pragma once

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gui/gui_layer.h"
#include "vtx/common/vtx_property_cache.h"
#include "vtx/writer/core/vtx_replay_filter.h"

class InspectorSession;

// Floating window (File > Filter Entities...) that writes a new .vtx with entities
// of the loaded replay dropped (blacklist) or exclusively kept (whitelist) by
// struct name, unique id glob or property value. Every frame survives; only the
// entity payload changes. The rewrite is VTX::FilterReplayFile on a worker thread
// with live chunk progress and cancel.
//
// Implements IGuiLayer directly (rather than the shared ImGuiWindow base) for the
// same reason as CutReplayWindow: gui/gui_window.h's `class ImGuiWindow` clashes
// with Dear ImGui's `struct ImGuiWindow` in translation units that include
// imgui_internal.h (inspector_layout.cpp).
class FilterEntitiesWindow : public IGuiLayer {
public:
    explicit FilterEntitiesWindow(std::shared_ptr<InspectorSession> session);
    ~FilterEntitiesWindow() override;

    void OnUpdate() override {}
    void OnRender() override;

    void SetOpen(bool open) { is_open_ = open; }
    bool IsOpen() const { return is_open_; }

private:
    enum class Phase { Idle, Running, Done };

    struct StructRow {
        std::string name;
        int32_t type_id = -1;
        bool selected = false;
    };

    struct PropertyRow {
        char struct_name[128] = {};
        char field_name[128] = {};
        char pattern[256] = {};
    };

    // Shared with the worker thread.
    struct Progress {
        std::atomic<int32_t> done {0};
        std::atomic<int32_t> total {0};
        std::atomic<bool> cancel {false};
    };

    // What the current rules do to the current frame (recomputed when either changes).
    struct Preview {
        std::string key;
        std::string error; ///< Rule resolution error from the matcher, if any.
        bool has_matcher = false;
        int32_t frame_index = -1;
        uint64_t seen = 0;
        uint64_t kept = 0;
        std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> buckets; ///< name -> (kept, seen)
        std::vector<uint64_t> per_struct_seen;                                      ///< indexed like structs_
    };

    void DrawContent();
    void DrawRules();
    void DrawPreview(const VTX::ReplayFilterSpec& spec);
    void DrawRunControls(const VTX::ReplayFilterSpec& spec);
    void DrawOutcome();
    VTX::ReplayFilterSpec BuildSpec() const;
    static std::string SpecSignature(const VTX::ReplayFilterSpec& spec);
    void RefreshPreview(const VTX::ReplayFilterSpec& spec, const std::string& key);
    void StartFilter(const VTX::ReplayFilterSpec& spec, const std::string& dest_path);
    void JoinWorker();

    std::shared_ptr<InspectorSession> session_;
    VTX::PropertyAddressCache schema_cache_;
    std::vector<StructRow> structs_;
    std::vector<PropertyRow> property_rules_;
    char unique_id_patterns_[4096] = {};
    int mode_ = 0; // 0 = drop matching (blacklist), 1 = keep only matching (whitelist)
    bool case_insensitive_ = false;
    bool prune_events_ = true;

    Preview preview_;

    Phase phase_ = Phase::Idle;
    std::shared_ptr<Progress> progress_;
    std::future<VTX::ReplayFilterResult> future_;
    VTX::ReplayFilterResult outcome_;
    std::string outcome_dest_;
    bool outcome_logged_ = false;
    bool is_open_ = true;
};
