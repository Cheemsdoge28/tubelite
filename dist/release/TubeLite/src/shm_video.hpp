#pragma once
#include <cstdint>
#include <vector>
#include <mutex>

// A single decoded video frame: RGBA pixels, row-major, stride = width * 4.
struct VideoFrame {
    std::vector<uint8_t> pixels;
    int width  = 0;
    int height = 0;
};

// Lock-free-ish triple-buffered frame transport between the video worker thread
// and the SDL compositor thread.
//
// Three slots are maintained:
//   write_slot  – exclusively owned by the producer (video thread)
//   ready_slot  – the latest complete frame, exchanged under a short mutex
//   read_slot   – exclusively owned by the consumer (compositor thread)
//
// The mutex is held only for index-swaps (O(1), nanoseconds).
// Pixel buffers are never copied; only std::vector ownership is swapped.
class FrameRing {
public:
    FrameRing() = default;

    // ── Producer API (video thread only) ─────────────────────────────────────

    // Get the write buffer.  The producer fills this freely without any lock.
    VideoFrame& writeBuf() { return bufs_[write_slot_]; }

    // Ensure the write buffer is large enough for (w × h) RGBA pixels.
    void prepareWrite(int w, int h) {
        VideoFrame& f = bufs_[write_slot_];
        const size_t need = static_cast<size_t>(w) * h * 4;
        if (f.pixels.size() < need) f.pixels.resize(need);
        f.width  = w;
        f.height = h;
    }

    // Publish the written frame.  Swaps write_slot ↔ ready_slot under the
    // mutex, making the new frame immediately available to the consumer.
    void commit() {
        std::lock_guard<std::mutex> g(mu_);
        std::swap(write_slot_, ready_slot_);
        has_new_ = true;
    }

    // ── Consumer API (compositor thread only) ─────────────────────────────────

    // Swap the latest ready frame into the read slot.
    // Returns true if a new frame was available.
    bool tryConsume() {
        std::lock_guard<std::mutex> g(mu_);
        if (!has_new_) return false;
        std::swap(read_slot_, ready_slot_);
        has_new_ = false;
        return true;
    }

    // Access the last consumed frame (valid after the first successful tryConsume).
    const VideoFrame& readBuf() const { return bufs_[read_slot_]; }

private:
    VideoFrame  bufs_[3];
    std::mutex  mu_;
    int         write_slot_ = 0;   // producer writes here
    int         ready_slot_ = 1;   // shared exchange slot
    int         read_slot_  = 2;   // consumer reads here
    bool        has_new_    = false;
};
