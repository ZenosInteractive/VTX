#pragma once

#include <atomic>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include "services/analysis_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"

namespace VtxServices {

    class AnalysisScanner {
    public:
        struct ScanConfig {
            int32_t start_frame = 0;
            int32_t end_frame = 0;
            int32_t bucket_index = 0; // Index into Frame::GetBuckets()
            std::string bucket_name;  // Display name (from schema)
            std::vector<std::string> filter_unique_ids;
            std::vector<std::string> property_names;
        };

        enum class ScanType { EntityLifeTime, UniqueProperties, TrackProperty };

        AnalysisScanner() = default;
        ~AnalysisScanner();

        // Not copyable or movable (owns atomics and a future).
        AnalysisScanner(const AnalysisScanner&) = delete;
        AnalysisScanner& operator=(const AnalysisScanner&) = delete;
        AnalysisScanner(AnalysisScanner&&) = delete;
        AnalysisScanner& operator=(AnalysisScanner&&) = delete;

        // Resets the scanner to idle state so it can be reused for a new scan.
        void Reset();

        void Start(ScanType type, const ScanConfig& config, VTX::IVtxReaderFacade* reader,
                   const VTX::PropertyAddressCache& cache, const VTX::FileFooter& footer);

        void Cancel();

        bool IsRunning() const { return is_running_.load(); }
        float GetProgress() const { return progress_.load(); }
        int32_t GetCurrentFrame() const { return current_frame_.load(); }
        std::string GetStatus() const;

        const EntityLifeTimeResult& GetLifeTimeResult() const { return lifetime_result_; }
        const UniquePropertiesResult& GetUniquePropsResult() const { return unique_props_result_; }
        const TrackPropertyResult& GetTrackPropResult() const { return track_prop_result_; }

    private:
        void RunEntityLifeTime(const ScanConfig& config, VTX::IVtxReaderFacade* reader,
                               const VTX::PropertyAddressCache& cache, const VTX::FileFooter& footer);

        void RunUniqueProperties(const ScanConfig& config, VTX::IVtxReaderFacade* reader,
                                 const VTX::PropertyAddressCache& cache, const VTX::FileFooter& footer);

        void RunTrackProperty(const ScanConfig& config, VTX::IVtxReaderFacade* reader,
                              const VTX::PropertyAddressCache& cache, const VTX::FileFooter& footer);

        void SetStatus(const std::string& msg);

        // Returns game time for a frame index, or 0 if unavailable.
        static uint64_t GetGameTime(const VTX::FileFooter& footer, int32_t frame_index);

        // Checks if a uid passes the filter (empty filter = all pass).
        static bool PassesUidFilter(const std::vector<std::string>& filter, const std::string& uid);

        std::atomic<bool> is_running_ {false};
        std::atomic<bool> cancel_requested_ {false};
        std::atomic<float> progress_ {0.0f};
        std::atomic<int32_t> current_frame_ {-1};

        mutable std::mutex status_mutex_;
        std::string status_message_;

        std::future<void> worker_;

        EntityLifeTimeResult lifetime_result_;
        UniquePropertiesResult unique_props_result_;
        TrackPropertyResult track_prop_result_;
    };

} // namespace VtxServices
