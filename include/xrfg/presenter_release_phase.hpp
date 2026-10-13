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
        std::uint32_t depth{}, unwritten{}, early{}, step{}, maximum{};
    };

    static constexpr std::uint32_t maximum(std::uint32_t frames) noexcept {
        return frames > 1 ? frames - 1 : 0;
    }

    std::uint32_t depth() const noexcept { return depth_; }

    // Keep the learned phase through a presenter restart. Discard a partial
    // observation window; it no longer describes the restarted queue.
    void reset_window(std::uint32_t frames) noexcept {
        depth_ = std::min(depth_, maximum(frames));
        samples_ = unwritten_ = early_ = 0;
    }

    void change_shape(std::uint32_t frames) noexcept {
        reset_window(frames);
        hold_ = 0;
    }

    Report observe(bool unwritten, bool early, std::uint32_t frames) noexcept {
        unwritten_ += unwritten ? 1U : 0U;
        early_ += early ? 1U : 0U;
        if (++samples_ < 30) return {};
        const auto limit = maximum(frames);
        depth_ = std::min(depth_, limit);
        if (hold_ > 0) --hold_;
        std::uint32_t step = 0;
        if (unwritten_ >= 2 && depth_ < limit) {
            ++depth_;
            hold_ = 10;
            step = 1;
        } else if (unwritten_ == 0 && early_ == 30 && depth_ > 0 && hold_ == 0) {
            --depth_;
            step = 2;
        }
        const Report report{true, depth_, unwritten_, early_, step, limit};
        samples_ = unwritten_ = early_ = 0;
        return report;
    }

    // Only anchor to an already enqueued group. At admission this is the
    // previous group, after enqueue it is the new group. Predicting a future
    // sequence here deadlocks when the presenter has a retained repeat.
    std::uint64_t target(std::uint64_t first, std::uint64_t last,
                         std::uint32_t frames) const noexcept {
        if (first == 0 || last < first) return 0;
        const auto depth = std::min<std::uint64_t>(
            std::min(depth_, maximum(frames)), last - first);
        return last - depth;
    }

private:
    std::uint32_t depth_{}, samples_{}, unwritten_{}, early_{}, hold_{};
};

} // namespace xrfg
