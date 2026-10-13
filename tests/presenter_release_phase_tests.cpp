#include "xrfg/presenter_release_phase.hpp"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
xrfg::PresenterReleasePhase::Report window(
    xrfg::PresenterReleasePhase& phase, unsigned late, unsigned early, unsigned frames) {
    xrfg::PresenterReleasePhase::Report report;
    for (unsigned i = 0; i < 30; ++i) {
        report = phase.observe(i < late, i >= 30 - early, frames);
        require(report.due == (i == 29), "one report per complete window");
    }
    return report;
}
}

int main() {
    xrfg::PresenterReleasePhase phase;
    require(window(phase, 1, 0, 3).depth == 0, "one isolated late frame must not deepen");
    auto report = window(phase, 2, 0, 3);
    require(report.depth == 1 && report.step == 1 && report.unwritten == 2,
            "two unwritten leaders release the next frame a slot earlier");
    require(window(phase, 30, 0, 3).depth == 2, "3X may move two slots earlier");
    require(window(phase, 30, 0, 3).depth == 2, "depth is bounded by the group");
    // A newly saturated maximum does not restart the cooldown.
    for (int i = 0; i < 8; ++i)
        require(window(phase, 0, 30, 3).depth == 2, "do not flap back immediately");
    report = window(phase, 0, 30, 3);
    require(report.depth == 1 && report.step == 2, "return after ten windows with early output");
    require(window(phase, 0, 29, 3).depth == 1, "all leaders must be a period early");
    window(phase, 2, 0, 3);
    phase.reset_window(3);
    require(phase.depth() == 2, "presenter restart preserves learned depth");
    phase.change_shape(2);
    require(phase.depth() == 1, "3X to 2X clamps depth");
    phase.change_shape(3);
    require(phase.depth() == 1, "2X to 3X preserves valid depth");
    phase.observe(true, false, 3);
    phase.reset_window(3);
    require(window(phase, 0, 0, 3).unwritten == 0, "restart clears incomplete observations");

    // Admission must wait only for the previous, already queued group, never
    // for next_sequence (12 here). Repeats cannot advance a popped sequence.
    require(phase.target(10, 11, 2) == 10, "admission anchor stays in the queued pair");
    require(phase.target(9, 9, 3) == 9, "a prime always waits for its own pop");
    require(phase.target(0, 0, 2) == 0, "first admission cannot wait for an unqueued frame");
    require(phase.target(12, 11, 2) == 0, "an invalid anchor cannot underflow");
    for (unsigned frames : {2U, 3U}) {
        for (unsigned count : {1U, 2U, 3U}) {
            const auto target = phase.target(100, 99 + count, frames);
            require(target >= 100 && target <= 99 + count,
                    "interpolation and extrapolation targets stay in the submitted group");
        }
    }
    std::cout << "Presenter release phase adaptation and admission bounds passed\n";
}
