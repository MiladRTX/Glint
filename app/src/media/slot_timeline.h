#pragma once
// Internal: places recorded frames on a constant-rate grid.
//
// The console encodes at 59.94 fps but says 60, and its timestamps are the
// jittery times the payload read each frame. Each frame normally takes the
// next slot of an exact 1/fps grid. Only when the console's clock has stayed
// at least a whole slot ahead of the grid for half a second (frames lost, or
// the 59.94/60 drift adding up) are slots skipped, so jitter and bursts never
// cause skips, and the file still tracks real time.
#include <algorithm>
#include <cstdint>
#include <deque>

namespace glint {

class SlotTimeline {
public:
    explicit SlotTimeline(int fps = 60) : fps_(fps > 0 ? fps : 60) {}

    // `pts` is the unwrapped 90 kHz console time. Returns the frame's slot.
    int64_t place(int64_t pts)
    {
        if (slot_ < 0) {
            origin_ = pts;
            slot_ = 0;
            return slot_;
        }
        slot_++;
        int64_t lead = (pts - origin_) - slot_ticks(slot_);   // > 0: console ahead of the grid
        window_.push_back({pts, lead});
        while (!window_.empty() && pts - window_.front().pts > kWindow) window_.pop_front();
        if (pts - window_.front().pts >= kWindow * 9 / 10) {
            int64_t low = window_.front().lead;
            for (const Mark& m : window_) low = std::min(low, m.lead);
            int64_t period = 90000 / fps_;
            if (low >= period) {
                slot_ += low / period;   // catch up whole slots at once
                window_.clear();
            } else if (low < 0) {
                origin_ += low;   // the first frames came late: re-anchor so the grid never leads
                window_.clear();
            }
        }
        return slot_;
    }

    // Start of slot `n` in 90 kHz ticks.
    int64_t slot_ticks(int64_t n) const { return (n * 90000 + fps_ / 2) / fps_; }
    int fps() const { return fps_; }

private:
    struct Mark {
        int64_t pts;
        int64_t lead;
    };
    static constexpr int64_t kWindow = 45000;   // half a second

    int fps_;
    int64_t origin_ = 0;
    int64_t slot_ = -1;
    std::deque<Mark> window_;
};

// Unwraps the payload's 32-bit timestamps.
class PtsUnwrap {
public:
    int64_t operator()(uint32_t raw)
    {
        if (!started_) {
            started_ = true;
            last_ = raw;
            value_ = raw;
            return value_;
        }
        value_ += (int32_t)(raw - last_);
        last_ = raw;
        return value_;
    }

private:
    bool started_ = false;
    uint32_t last_ = 0;
    int64_t value_ = 0;
};

}
