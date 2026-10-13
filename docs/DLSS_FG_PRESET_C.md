# NVIDIA DLSS Frame Generation Preset C

Preset C works with OFXR's **native NVIDIA DLSS Frame Generation** backend
through the game's NVIDIA driver profile. The NVIDIA app beta supplies the
new FG runtime; OFXR continues supplying its stereo color, depth and motion
vectors through NGX. No private NGX parameter or patched NVIDIA DLL is needed.

This does not change OFXR's optical-flow or DLSS-vector synthesis methods.
It also does not change OFXR's FG resolution setting: Preset C and 67%/100%
resolution are separate choices.

## Enable it

1. Install NVIDIA's app beta containing the October 13, 2026 FG update and
   a compatible NVIDIA driver.
2. In OFXR's tray menu, open **Quality and performance → DLSS Frame
   Generation model → Choose game executable**. Select the game's rendering
   executable, then select **Preset A (standard)** or **Preset C (performance)**.
   For Galactic Racer choose `Griffin/Binaries/Win64/SWGR-Win64-Shipping.exe`.
3. Select **NVIDIA DLSS Frame Generation (experimental)** in OFXR's
   frame-generation method menu. The build must have native DLSS FG enabled,
   and the game must supply compatible DLSS depth/motion-vector guides.
4. Restart the game after changing its NVIDIA preset.

**Game default** removes the two per-profile FG overrides and restores the
driver's predefined/inherited behavior. It does not undo a global NVIDIA app
override. OFXR shows the affected NVIDIA profile and reads its current state
each time the menu opens. Other settings in that profile are preserved. A
profile can cover multiple executables belonging to the same game.

The selected game path is saved in `tray.ini`; the preset remains in NVIDIA's
profile database, so NVIDIA app changes are reflected in OFXR. You can also
select C in NVIDIA app under **DLSS Override – Model Presets → Custom →
Frame Generation**. This is the same setting. A successful save confirms the
profile selection, not whether the installed runtime supports the model.

For reproducible launches, the tray executable exposes the same implementation
as a command. It runs before the tray's single-instance check and returns a
nonzero exit code on failure:

```powershell
& .\build\Release\OFXRBridgeTray.exe --dlssg-preset C "E:\Games\Game\Binaries\Win64\Game-Win64-Shipping.exe"
```

Replace `C` with `A`, `default`, or `status`. Restart the game after a change;
recreating its NGX feature in the same process is not enough.

The controller-flip panel can change OFXR's native FG resolution and frame
multiplier during play, but it cannot currently hot-swap NVIDIA's A/C model.
Binding A/C profile writes to that gesture would only save the next launch's
selection. OFXR must not shut down the process-wide NGX instance to force a
reload: that also invalidates the game's own DLSS features on the device.

The NVIDIA app selection corresponds to these public NVAPI DRS settings:

| Setting | ID | Value |
|---|---|---|
| DLSS Frame Generation override | `0x10E41E03` | `1` (enabled) |
| DLSS Frame Generation render preset | `0x10E41DF1` | `3` (C) |

The public DLSS SDK available during validation was 310.9.1. Its headers do
not expose a native NGX FG Preset C selection parameter. Do not translate
the DRS value into an invented NGX parameter. NVIDIA's signed 310.9.16 OTA
runtime loaded automatically from its NGX cache even with the SDK's
310.9.1 `nvngx_dlssg.dll` beside OFXR. Copying the newer DLL alone did not
select C in the standalone comparison.

## Verify the effective preset

For a diagnostic launch, set `XRFG_TEST_NATIVE_DLSSG_VERBOSE=1` in the game's
environment before starting it. When NGX accepts OFXR's logging callback,
OFXR retains its runtime-path and effective-preset messages, with the process
ID, in:

```text
%LOCALAPPDATA%\OFXR Bridge\NGX\ofxr-native-dlssg.log
```

Look for the current game process and:

```text
ReportOverrideStates: render preset (requested=3, effective=3)
```

A driver-profile readback of 3 alone is insufficient: confirm the loaded
runtime and effective preset, then check that OFXR is actually generating.
Some games initialize NGX before OFXR and retain their own logging callback;
the preset message can be absent in those games even with verbose mode on.
Use the isolated test below to verify the runtime's A/C behavior, and loaded
module paths plus OFXR generation diagnostics for the live game. Absence of
the message is not proof of either preset.
With `XRFG_TEST_NATIVE_DLSSG_DECISIONS=1`, OFXR's per-process native decision
log records NGX's output-disable flags. `eyes=2 outputs=1 disable=0,0,0,0`
indicates accepted stereo 2X output. Remove these diagnostic environment
variables for normal use.

## Repeatable comparison

Build `xrfg_d3d12_history_tests` with native DLSS FG enabled, then run with
64-bit Windows Python:

```powershell
python scripts/test-dlssg-preset.py --output build/preset-c-validation --rounds 3
```

The output directory must be new. The script copies the native test and its
adjacent DLL under a unique executable name, gives only that copy a temporary
NVIDIA profile, and compares explicit A and C. It verifies effective presets
in the native correctness tests, runs interleaved timing cases, then removes
its temporary profile. Logs, binary hashes and `results.json` remain in the
output directory. No global or game profile is changed. An externally killed
Python process can leave its unique profile behind; `results.json` records
the profile name for removal.

Recorded three-frame sequences can be checked against their held-out middle
frame with repeated `--replay <sequence-directory>` arguments. Replay runs
native FG at both 100% and 67% resolution. These quality measurements and the
timings exercise OFXR's native implementation directly, not a game FPS counter.

## Measurements on October 13, 2026

RTX 5090, driver 617.42, FG runtime 310.9.16, shared stereo, 3004×3004 per eye,
100% FG resolution, camera fit disabled. Three interleaved A/C rounds, eight
warmup pairs and 96 measured pairs per case. Values below are the median of
each round's p10 GPU span, reducing the effect of incidental GPU scheduling.

| Case | A | C | FG GPU time reduction |
|---|---:|---:|---:|
| 2X, still head | 1.927 ms | 1.158 ms | 39.9% |
| 2X, turning head | 2.372 ms | 1.524 ms | 35.8% |
| 3X, still head | 2.968 ms | 1.454 ms | 51.0% |
| 3X, turning head | 3.518 ms | 1.817 ms | 48.3% |

These are reductions in FG cost, **not overall game FPS gains**. Rebuilding
the current checkout and rerunning correctness and timing tests reproduced
the improvement. Native stereo, real-copy, reset and missing-depth tests
passed under both A and C.

Quality remains scene dependent. In the recorded race-sway sequence at 100%,
moving-pixel MAE changed from 9.231 to 9.252 and SSIM from 0.6662 to 0.6645.
In the Tatooine sequence, moving-pixel MAE increased from 10.820 to 11.767
and SSIM fell from 0.7418 to 0.7217. Lower MAE and higher SSIM are better.
Preset C is a performance/quality tradeoff, not an unconditional upgrade.

### Galactic Racer live check

The October 13 check used Galactic Racer's Lantaana / Griffin's Flight scene
through UEVR and Meta XR Simulator at 90 Hz, 1680×1760 per eye, native FG at
100% and forced 2X. The game profile was set to C before launch, and the game
loaded NVIDIA's signed 310.9.16 OTA FG module. OFXR recorded 14,966 accepted
stereo output decisions with no NGX output-disable flags across the menu and
race session. The retained presenter log contained both real and generated
submissions. No OFXR control/runtime errors were reported.

A 20-second race-scene sample recorded 897 GPU spans: median 0.711 ms,
p10 0.709 ms and p90 0.715 ms, with 44.9 rendered application FPS. This is a
functional simulator check, not a headset comfort/visual-quality assessment
or a completed race benchmark.

The game retained its own NGX logging setup, so it did not emit OFXR's
effective-preset message. Explicit `requested=3, effective=3` confirmation
was obtained in the isolated native tests. Changing A/C in the profile and
recreating OFXR's feature within the same game process did not change the
timings; restart the game rather than treating that as a live A/B switch.

### Galactic Racer A/C comparison using the OFXR selector

Separate game processes were launched with A and C applied through
`OFXRBridgeTray.exe --dlssg-preset`. Same RTX 5090, driver 617.42, signed
310.9.16 OTA runtime, Meta XR Simulator at 90 Hz, 1680×1760 per eye,
100% native FG resolution, forced 2X, shared feature and camera fit off.
Each warmed sample lasted 20 seconds; the camera was held still. The numbers
are median GPU time for OFXR's complete stereo synthesis span.

| Scene | A | C | FG GPU time reduction | Rendered FPS A / C |
|---|---:|---:|---:|---:|
| Title screen | 1.147 ms | 0.693 ms | 39.6% | 45.0 / 45.0 |
| Campaign hub, fixed view | 1.188 ms | 0.697 ms | 41.3% | 45.0 / 44.8 |

The title samples contained 903 A spans and 904 C spans; the campaign samples
contained 904 A spans and 897 C spans. The simulator's
90 Hz / 2X pacing holds rendered FPS near 45; the saving is GPU headroom,
not a measured increase in uncapped game FPS. Initial loading/warmup samples
and an unsuccessful UEVR backbuffer initialization were excluded. This is
a simulator measurement, not a moving-head headset or full-race benchmark.
Retained native logs contained 12,200 accepted stereo outputs for A and
7,909 for C, with no NGX output-disable flags. Both presenter logs contained
real and generated submissions; OFXR control/runtime errors, invalid guide
publications, snapshot failures and queue mismatches were zero. These counts
cover the sessions, not only the timed windows.

The Release tray and its settings regression test passed. An isolated NVIDIA
profile test switched C → A → default → C, verified driver readback after
each change, and checked that an unrelated power-management setting survived.
The temporary test profile was removed. The game profile was left on C.

### Galactic Racer during a race in OpenXR Simulator

The subsequent test used **OpenXR Simulator** from `E:/Github/OpenXR-Simulator`,
not Meta XR Simulator. Its existing preview-window thread fix was rebuilt into
the runtime DLL; the `preview_window_owner` and `projection_timing` tests passed.
The visible preview displayed both eyes and remained responsive. The tested
runtime DLL SHA-256 was
`7F7B5126F2C75A7C7CB6802F20156699A1DF85CE22EC8B6A363C88CF41860312`.

Both presets were tested in **Arcade → Podracing → Tatooine / King of the
Racers**, with the game's built-in autopilot driving the vehicle. Each preset
was applied through OFXR before a fresh game process. The setup was RTX 5090,
driver 617.42, signed FG runtime 310.9.16, 90 Hz, 3004×3004 per eye, 100% native
FG, forced 2X, shared stereo feature, and camera fit off. Each measurement had
a four-second warmup followed by 30 seconds of timing.

| Preset | Median stereo FG GPU time | p10 / p90 | GPU spans | Rendered FPS |
|---|---:|---:|---:|---:|
| A | 1.977 ms | 1.960 / 2.064 ms | 1,355 | 45.0 |
| C | 1.180 ms | 1.148 / 1.212 ms | 1,355 | 45.0 |

C reduced median FG GPU time by **40.3%** (0.797 ms). This is GPU headroom,
not a measured uncapped game FPS increase: the simulator's 90 Hz / 2X pacing
held rendered FPS near 45. These were separate race segments, not identical
replay frames; the desktop mirror also changed from windowed to fullscreen
before the accepted A sample. The stereo render size and simulator preview
size (650×320) stayed the same.

Each accepted run had 35 position samples with all 34 successive intervals
advancing: at least 211.4 m per interval for A and 204.6 m for C. Vehicle speed
ranged from 211.2–284.3 m/s for A and 198.4–272.8 m/s for C. Earlier samples
containing starting-grid waits or pauses were rejected. Position changes were
checked because the game's velocity getter retains a nonzero value while
paused. Both accepted timing windows reported active guides (status 6) and
1,363 guide pairs used; final session checks reported zero OFXR control errors,
invalid guide publications, snapshot failures, and queue mismatches.

Local evidence is under `build/dlssg-preset-c-20261013/race/`:
`continuous-race-results.json`, the `A-openxr-continuous` and
`C-openxr-continuous` timing/motion files, and their stereo screenshots.
`summarize_continuous.py` rejects stationary position intervals before
calculating the comparison. This validates generation during actual racing;
it does not establish full-race image quality or moving-head headset comfort.

## Sources and implementation notes

- [NVIDIA's October 13 announcement](https://www.nvidia.com/en-us/geforce/news/dlss-5-faster-ai-model-hogwarts-legacy-rtx-spark-laptops/)
  describes the new performance FG model and NVIDIA app beta selection.
- [NVIDIA's public driver-setting definitions](https://github.com/NVIDIA/nvapi/blob/main/NvApiDriverSettings.h)
  define the override IDs and C value.
- [DLSS SDK releases](https://github.com/NVIDIA/DLSS/releases) identify the
  published SDK separately from the app's OTA runtime.

IDA Pro 9.0 headless inspection of the installed signed 310.9.16 runtime
confirmed that its Preset C handler enables FG Lite candidate blending and
disables its UI recomposition option. This was used to understand the
behavior; OFXR does not depend on those private parameter names or offsets.
