# Bridge flight recorder

The recorder belongs to the OFXR OpenXR layer and is independent from game or
VR-mod logging. It is disabled by default.

V066 also provides an independent transparent numeric FPS overlay. It is green
when a recent synthetic submission succeeded and red when generation is
inactive. It counts accepted nonempty OpenXR submissions, not physical headset
scanout, and does not replace the recorder for crash or fallback diagnosis. See
[FPS_OVERLAY.md](FPS_OVERLAY.md) for its controls and limitations.

Enable **Bridge flight recorder** from the tray before starting the game. Each
OpenXR process then creates a bounded file named
`ofxr-bridge-flight-YYYYMMDD-HHMMSS-pidNNNN.log`. Use **Open bridge logs** to
open its directory.

The log records layer negotiation, graphics binding, swapchain eligibility,
projection mapping, frame-generation stages, recovery events and potentially
blocked OpenXR call boundaries. An unmatched begin record can identify the
operation which did not return during a freeze.

For a compatibility report, reproduce the problem once and attach the newest
flight log. Also provide the game and version, VR mod, headset, OpenXR runtime,
GPU and driver, selected backend/options and exact reproduction steps. If no
log was created, state that explicitly: the layer may not have loaded or the
process may have stopped before recorder initialization.

The recorder does not capture video and is not a replacement for a native
crash dump. Game logs and crash dumps remain useful when available.

## Adaptive presenter release (release 38 / upstream V452)

On non-SteamVR runtimes, `presenter_transition` with `result=802` reports one
window of 30 generated groups. A group is one application's submissions in
display order: synthetic(s) then real for interpolation, real then
synthetic(s) for extrapolation.

| Field | Meaning |
|---|---|
| `a` | Release depth in display slots before the group's last submission; 0 is the last. Bounds are 0–1 at ordinary 2X, 1–2 for deep 2X interpolation, and 0–2 at 3X |
| `b & 0xffff` | Groups whose first actual synthetic was unwritten at its pop or held for that reason |
| `(b >> 16) & 0xffff` | Groups where the following group's first synthetic was already written and old enough for the pipeline when the current group's last submission popped |
| `(b >> 32) & 0xffff` | Groups whose first synthetic was held for minimum queue age (added in release 38) |
| `(b >> 48) & 0xffff` | Groups arriving after the presenter had to repeat an image because no next group was queued (added in release 38) |
| `c & 0xff` | Step: 0 unchanged, 1 release earlier, 2 release later |
| `c >> 8` | Maximum permitted depth |

Two or more groups delayed by queue age, unwritten output, or late arrival
move release one slot earlier. A group affected by several causes counts once.
Moving back requires a window with none of these delays, all 30 following leaders ready and eligible
early, and expiration of the ten-window cooldown. The learned depth survives
presenter restarts and is clamped when switching between 2X and 3X.

Deep 2X interpolation reserves one release slot for its minimum queue age.
It can release one additional slot earlier when the application needs more
than one display period to render. Without that allowance, a ready pair can
occupy three slots instead of two. Shallow and real-first extrapolation
groups do not reserve this extra slot.

For `presenter_pair_release`, `a` is the wait in microseconds. On non-SteamVR,
`b` is the most recently popped submission sequence, `c` the target sequence,
and `result` the surplus beyond the target. At admission the anchor is the
previous queued group; after enqueue it is the new group. Deep interpolation
can target the submission immediately before its anchor's first submission;
that submission has already been queued as well.
A repeated image cannot advance that sequence. A prime waits for its own pop.

SteamVR keeps its existing serial-based `presenter_pair_release` fields and
3X delay records (`presenter_transition`, `result=801`); it does not emit 802.
Readiness and submission counts do not prove headset scanout or visual quality.
