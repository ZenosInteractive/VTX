#include "services/playback_service.h"

namespace VtxServices {

    int PlaybackService::Update(float delta_time_seconds, int current_frame, int total_frames, float duration_seconds) {
        if (!is_playing_ || total_frames <= 0 || duration_seconds <= 0.0f) {
            return current_frame;
        }

        float fps = static_cast<float>(total_frames) / duration_seconds;
        float time_per_frame = 1.0f / fps;

        time_accumulator_ += delta_time_seconds * playback_speed_;

        while (time_accumulator_ >= time_per_frame) {
            current_frame++;
            time_accumulator_ -= time_per_frame;

            if (current_frame >= total_frames) {
                current_frame = total_frames - 1;
                is_playing_ = false;
                break;
            }
        }

        return current_frame;
    }

    void PlaybackService::Play(int current_frame, int total_frames) {
        if (current_frame >= total_frames - 1) {
            // Reset if at end
            time_accumulator_ = 0.0f;
        }
        is_playing_ = true;
    }

    void PlaybackService::Pause() {
        is_playing_ = false;
    }

    void PlaybackService::Stop() {
        is_playing_ = false;
        time_accumulator_ = 0.0f;
    }

} // namespace VtxServices
