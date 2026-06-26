#pragma once
//
// Lightweight runtime profiler for the main render loop.
//
// Usage:
//   void someFunc() {
//       PROFILE_SCOPE("someFunc");
//       ...
//   }
//
//   void Counter() {
//       PROFILE_COUNT("mpv_new_frame");  // bump a counter (no timing)
//   }
//
// At the start of every frame call Profiler::instance().beginFrame();
// At the end call Profiler::instance().endFrame();
// The debug overlay (toggled with F12 / L3) renders the per-section averages.
//
// Section identity is by const char* pointer (string literals only — do NOT
// pass dynamically-built strings).  Storage is fixed-size (no heap), and the
// per-scope cost is one steady_clock::now() at enter + one at leave (~100ns
// on the A35) plus an array index — safe to leave on in release builds.

#include <chrono>
#include <cstdint>
#include <cstring>

class Profiler {
public:
    static constexpr int  MAX_SECTIONS  = 48;
    static constexpr int  HIST_FRAMES   = 60;
    static constexpr int  STACK_MAX     = 16;

    struct Section {
        const char* name      = nullptr;
        // Current frame accumulators
        uint64_t    cur_ns    = 0;
        uint32_t    cur_calls = 0;
        // Rolling EMA over ~10 frames
        float       avg_ns    = 0.0f;
        float       max_ns    = 0.0f;     // slow-decaying max
        float       avg_calls = 0.0f;
    };

    static Profiler& instance() {
        static Profiler p;
        return p;
    }

    // Call at the start of every frame.
    void beginFrame() {
        for (int i = 0; i < num_sections_; ++i) {
            sections_[i].cur_ns    = 0;
            sections_[i].cur_calls = 0;
        }
        frame_total_scope_calls_ = 0;
        frame_start_ = clock::now();
        ++frame_id_;
        if (overhead_per_scope_ns_ < 0.0f) calibrateOverhead();
    }

    // Call at the end of every frame.
    void endFrame() {
        const auto frame_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock::now() - frame_start_).count();

        frame_history_ms_[hist_idx_] = (float)frame_ns / 1.0e6f;
        hist_idx_ = (hist_idx_ + 1) % HIST_FRAMES;
        if (frame_count_ < HIST_FRAMES) ++frame_count_;
        last_frame_ms_ = (float)frame_ns / 1.0e6f;

        // Self-overhead estimate: per-scope calibration × scope calls this frame.
        // EMA-smoothed so jitter in the per-scope cost doesn't flicker the panel.
        const float overhead_ms = overhead_per_scope_ns_ > 0.0f
            ? (overhead_per_scope_ns_ * frame_total_scope_calls_) / 1.0e6f
            : 0.0f;
        avg_overhead_ms_ = avg_overhead_ms_ * 0.90f + overhead_ms * 0.10f;
        last_total_scope_calls_ = frame_total_scope_calls_;

        // Per-section rolling stats
        for (int i = 0; i < num_sections_; ++i) {
            auto& s = sections_[i];
            const float new_ns = (float)s.cur_ns;
            s.avg_ns    = s.avg_ns    * 0.90f + new_ns           * 0.10f;
            s.avg_calls = s.avg_calls * 0.90f + (float)s.cur_calls * 0.10f;
            if (new_ns > s.max_ns) s.max_ns = new_ns;
            else                   s.max_ns *= 0.985f;   // slow decay
        }
    }

    // One-time calibration: measure how long an empty enter+leave pair costs
    // on this CPU.  Called lazily at first beginFrame so the result reflects
    // the actual runtime CPU governor / frequency state.
    void calibrateOverhead() {
        constexpr int N = 2000;
        // Reserve a calibration slot so the test enter/leaves don't pollute a
        // real section's stats.
        int cal_idx = findOrRegister("_calibrate_");
        const auto t0 = clock::now();
        for (int i = 0; i < N; ++i) {
            enter(cal_idx);
            leave(cal_idx);
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock::now() - t0).count();
        overhead_per_scope_ns_ = (float)ns / (float)N;
        // Clear the calibration counters so the panel doesn't show "2000 calls
        // of _calibrate_".  The section name remains registered.
        sections_[cal_idx].cur_ns    = 0;
        sections_[cal_idx].cur_calls = 0;
        sections_[cal_idx].avg_ns    = 0.0f;
        sections_[cal_idx].avg_calls = 0.0f;
        sections_[cal_idx].max_ns    = 0.0f;
        frame_total_scope_calls_ -= N;   // we just inflated the running counter
    }

    // Returns a per-process unique index for `name` (string literal pointer).
    // Allocates a new slot on first sight.  Returns -1 if the table is full.
    int findOrRegister(const char* name) {
        for (int i = 0; i < num_sections_; ++i) {
            if (sections_[i].name == name) return i;
        }
        // Fallback for callers that may build the same name via different
        // pointers (rare, but cheap to check)
        for (int i = 0; i < num_sections_; ++i) {
            if (sections_[i].name && std::strcmp(sections_[i].name, name) == 0)
                return i;
        }
        if (num_sections_ < MAX_SECTIONS) {
            sections_[num_sections_].name = name;
            return num_sections_++;
        }
        return -1;
    }

    void enter(int idx) {
        if (idx < 0 || stack_depth_ >= STACK_MAX) return;
        stack_[stack_depth_].idx = idx;
        stack_[stack_depth_].t0  = clock::now();
        ++stack_depth_;
    }

    void leave(int idx) {
        if (stack_depth_ == 0) return;
        --stack_depth_;
        if (stack_[stack_depth_].idx != idx) {
            // Mismatched enter/leave — bail rather than corrupt stats.
            return;
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock::now() - stack_[stack_depth_].t0).count();
        Section& s = sections_[idx];
        s.cur_ns    += (uint64_t)ns;
        s.cur_calls += 1;
        ++frame_total_scope_calls_;
    }

    void count(int idx) {
        if (idx < 0) return;
        sections_[idx].cur_calls += 1;
    }

    // Read-only accessors
    int             sectionCount()       const { return num_sections_; }
    const Section&  section(int i)       const { return sections_[i]; }
    float           lastFrameMs()        const { return last_frame_ms_; }
    uint64_t        frameId()            const { return frame_id_; }
    // Profiler self-overhead per frame (EMA of overhead_per_scope_ns × call count).
    // Subtract from frame total to estimate "useful" frame work.
    float           avgOverheadMs()      const { return avg_overhead_ms_; }
    float           perScopeOverheadNs() const { return overhead_per_scope_ns_; }
    int             lastTotalScopeCalls() const { return last_total_scope_calls_; }
    void            getFrameHistory(float* out, int& count) const {
        count = frame_count_;
        // Oldest-first ordering for sparkline display
        const int start = (hist_idx_ - frame_count_ + HIST_FRAMES) % HIST_FRAMES;
        for (int i = 0; i < frame_count_; ++i) {
            out[i] = frame_history_ms_[(start + i) % HIST_FRAMES];
        }
    }

private:
    using clock      = std::chrono::steady_clock;
    using time_point = clock::time_point;

    Section sections_[MAX_SECTIONS];
    int     num_sections_ = 0;

    struct StackFrame { int idx; time_point t0; };
    StackFrame stack_[STACK_MAX];
    int        stack_depth_ = 0;

    time_point frame_start_{};
    float      frame_history_ms_[HIST_FRAMES] = {0};
    int        hist_idx_     = 0;
    int        frame_count_  = 0;
    float      last_frame_ms_ = 0.0f;
    uint64_t   frame_id_      = 0;

    // Self-overhead bookkeeping
    float      overhead_per_scope_ns_ = -1.0f;   // -1 = uncalibrated
    int        frame_total_scope_calls_ = 0;     // bumped in leave()
    int        last_total_scope_calls_  = 0;
    float      avg_overhead_ms_         = 0.0f;
};

// ── RAII scope helper ─────────────────────────────────────────────────────────
class ScopedProfile {
public:
    explicit ScopedProfile(const char* name) {
        idx_ = Profiler::instance().findOrRegister(name);
        Profiler::instance().enter(idx_);
    }
    ~ScopedProfile() {
        Profiler::instance().leave(idx_);
    }
private:
    int idx_;
};

#define PROFILE_CONCAT_(a, b) a##b
#define PROFILE_CONCAT(a, b)  PROFILE_CONCAT_(a, b)

// Time a scope.  Pass a string literal (identity is by pointer).
#define PROFILE_SCOPE(name) ScopedProfile PROFILE_CONCAT(_prof_scope_, __LINE__)(name)

// Bump a counter without timing — handy for "this happened" events
// (e.g. mpv produced a new frame, HUD static layer rebuilt, etc.).
#define PROFILE_COUNT(name)                                                    \
    do {                                                                       \
        static int _prof_count_idx_ = Profiler::instance().findOrRegister(name);\
        Profiler::instance().count(_prof_count_idx_);                          \
    } while (0)
