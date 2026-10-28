#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace glint {

// Puts console frames on an exact 1/fps timeline (local recording, live
// broadcast).
//
// The console stamps a frame when the payload reads it, which jitters by
// +-6 ms and comes in bursts, and its encoder runs at 59.94 fps. Following
// that clock made the timeline wander, so constant-frame-rate output dropped
// and repeated frames. Instead every frame takes the next slot of an exact
// 1/fps grid, and slots are skipped only when the console clock has stayed a
// frame or more further ahead than the least-delayed frames so far, at a
// steady cadence, for half a second: frames were lost, or the 59.94 vs 60
// drift has added up to one frame.
class FrameGrid {
public:
    void reset(int fps) {
        fps_ = fps > 0 ? fps : 60;
        frames_ = 0;
        slot_ = 0;
        lags_.clear();
        floor_ = 0.0;   // the first frame sits exactly on the grid
    }

    // `ticks`: the frame's console time in 90 kHz ticks since the first frame
    // (unwrapped). Returns its slot; slots only ever increase.
    int64_t place(int64_t ticks) {
        const double frame_ticks = 90000.0 / fps_;
        const size_t window = (size_t)std::max(fps_ / 2, 8);
        int64_t slot = frames_ == 0 ? 0 : slot_ + 1;
        double lag = (double)ticks / frame_ticks - (double)slot;
        floor_ = std::min(floor_, lag);
        lags_.push_back(lag);
        if (lags_.size() > window) lags_.pop_front();
        if (lags_.size() == window) {
            auto range = std::minmax_element(lags_.begin(), lags_.end());
            double low = *range.first - floor_, spread = *range.second - *range.first;
            // A wide spread is a burst of buffered frames draining after a
            // stall: nothing was lost, so keep counting.
            if (low >= 1.0 && spread <= 4.0) {
                int64_t skip = (int64_t)std::floor(low);
                slot += skip;
                for (double& earlier : lags_) earlier -= (double)skip;
            }
        }
        slot_ = slot;
        frames_++;
        return slot;
    }

    // A slot's time in 90 kHz ticks.
    int64_t ticks(int64_t slot) const { return (int64_t)std::llround((double)slot * 90000.0 / fps_); }

private:
    int fps_ = 60;
    uint64_t frames_ = 0;
    int64_t slot_ = 0;          // slot of the last frame
    std::deque<double> lags_;   // console clock minus grid (frames), recent frames
    double floor_ = 0.0;        // lowest of those seen: frames read with the least delay
};

} // namespace glint
