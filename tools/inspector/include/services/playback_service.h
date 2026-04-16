#pragma once
//
// PlaybackController — frame advancement / playback state machine.
// SDK-candidate: pure logic, no GUI dependency. Takes delta-time and
// replay metadata, outputs the next frame index.
//
// The GUI (TimelineWindow) owns an instance and feeds it delta time
// from ImGui::GetIO().DeltaTime each tick.
//

#include <algorithm>

namespace VtxServices {

class PlaybackService {
public:
    /// Advance playback by the given wall-clock delta (seconds).
    /// Returns the (possibly updated) current frame index.
    /// Does nothing when paused or when replay metadata is invalid.
    int Update(float delta_time_seconds, int current_frame, int total_frames, float duration_seconds);

    void Play(int current_frame, int total_frames);
    void Pause();
    void Stop();

    bool IsPlaying() const { return is_playing_; }
    float GetSpeed() const { return playback_speed_; }
    void SetSpeed(float speed) { playback_speed_ = std::clamp(speed, 0.1f, 10.0f); }

private:
    bool is_playing_ = false;
    float playback_speed_ = 1.0f;
    float time_accumulator_ = 0.0f;
};

} // namespace VtxServices
