#pragma once

#include <algorithm>
#include <cstdint>

namespace xrfg {

// V452's readiness-based application release, in display slots before the
// end of a submitted group. The end is the real image for interpolation and
// the last synthetic for extrapolation. Caller serializes access.
class PresenterReleasePhase {
public:
    struct Report {
        bool due{};
        std::uint32_t depth{}, unwritten{}, early{}, step{}, maximum{}, age_held{}, starved{};
    };

    static constexpr std::uint32_t maximum(std::uint32_t frames) noexcept {
        return frames > 1 ? frames - 1 : 0;
    }

    std::uint32_t depth() const noexcept { return depth_; }

    // Deep interpolation must leave one whole display period between enqueue
    // and the first synthetic. Reserve that slot in addition to the ordinary
    // release range, or a ready pair can take three slots forever at 2X.
    // Called with the actual submission order, so extrapolation adds no slot.
    void set_age_guard(bool enabled, std::uint32_t frames) noexcept {
        enabled = enabled && frames == 2;
        if (age_guard_ == enabled) return;
        age_guard_ = enabled;
        reset_window(frames);
        hold_ = 0;
    }

    std::uint32_t minimum(std::uint32_t frames) const noexcept {
        return age_guard_ && frames == 2 ? 1U : 0U;
    }

    std::uint32_t limit(std::uint32_t frames) const noexcept {
        return maximum(frames) + minimum(frames);
    }

    // Keep the learned phase through a presenter restart. Discard a partial
    // observation window; it no longer describes the restarted queue.
    void reset_window(std::uint32_t frames) noexcept {
        depth_ = std::clamp(depth_, minimum(frames), limit(frames));
        samples_ = unwritten_ = early_ = age_held_ = starved_ = late_ = 0;
    }

    void change_shape(std::uint32_t frames) noexcept {
        if (frames != 2) age_guard_ = false;
        reset_window(frames);
        hold_ = 0;
    }

    Report observe(bool unwritten, bool early, std::uint32_t frames,
                   bool age_held = false, bool starved = false) noexcept {
        unwritten_ += unwritten ? 1U : 0U;
        early_ += early ? 1U : 0U;
        age_held_ += age_held ? 1U : 0U;
        starved_ += starved ? 1U : 0U;
        late_ += unwritten || age_held || starved ? 1U : 0U;
        if (++samples_ < 30) return {};
        const auto ceiling = limit(frames);
        depth_ = std::clamp(depth_, minimum(frames), ceiling);
        if (hold_ > 0) --hold_;
        std::uint32_t step = 0;
        if (late_ >= 2 && depth_ < ceiling) {
            ++depth_;
            hold_ = 10;
            step = 1;
        } else if (late_ == 0 && early_ == 30 && depth_ > minimum(frames) && hold_ == 0) {
            --depth_;
            step = 2;
        }
        const Report report{true, depth_, unwritten_, early_, step, ceiling, age_held_, starved_};
        samples_ = unwritten_ = early_ = age_held_ = starved_ = late_ = 0;
        return report;
    }

    // Only anchor to an already enqueued group. At admission this is the
    // previous group, after enqueue it is the new group. Deep interpolation
    // can reach one slot before its first submission, into the preceding
    // already queued group, to preserve the application's two-period budget.
    // Predicting a future sequence deadlocks with a retained repeat.
    std::uint64_t target(std::uint64_t first, std::uint64_t last,
                         std::uint32_t frames) const noexcept {
        if (first == 0 || last < first) return 0;
        if (first == last) return last; // a prime always waits for its own pop
        const auto earliest = first - std::min<std::uint64_t>(minimum(frames), first - 1);
        const auto depth = std::min<std::uint64_t>(
            std::clamp(depth_, minimum(frames), limit(frames)), last - earliest);
        return last - depth;
    }

private:
    bool age_guard_{};
    std::uint32_t depth_{}, samples_{}, unwritten_{}, early_{}, hold_{}, age_held_{}, starved_{}, late_{};
};

} // namespace xrfg
