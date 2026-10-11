# Building OFXR Bridge

OFXR Bridge currently targets 64-bit Windows and builds with Visual Studio
2022, CMake 3.24 or newer and a recent Windows SDK containing `fxc.exe`.

## Dependencies

The repository already contains the exact OpenXR headers and NVIDIA Optical
Flow interface headers used by the project. NVIDIA's runtime API is supplied by
the installed display driver and is not required at build time.

FidelityFX SDK v1.1.4 is intentionally not committed. Clone the official SDK
at the pinned revision and build its static DX12 Optical Flow components:

```powershell
git clone --branch v1.1.4 --depth 1 `
  https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git `
  external/FidelityFX-SDK-v1.1.4

cmake -S external/FidelityFX-SDK-v1.1.4/sdk `
  -B build-fidelityfx -G "Visual Studio 17 2022" -A x64 `
  -DFFX_ALL=OFF -DFFX_OF=ON -DFFX_API_BACKEND=DX12_X64 `
  -DFFX_BUILD_AS_DLL=OFF

cmake --build build-fidelityfx --config Release
```

The expected outputs are:

```text
external/FidelityFX-SDK-v1.1.4/sdk/bin/ffx_sdk/ffx_backend_dx12_x64.lib
external/FidelityFX-SDK-v1.1.4/sdk/bin/ffx_sdk/ffx_opticalflow_x64.lib
```

The pinned FidelityFX commit is
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`.

## Build and test

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The distributable files are generated under `build/Release`:

```text
OFXRBridgeTray.exe
ofxr/
  XR_APILAYER_XRFrameBridge_diagnostic.dll
  ofxr_bridge.ini
```

Do not distribute PDBs, static libraries, test executables or the NVIDIA SDK.

## Native NVIDIA DLSS Frame Generation

V439 adds an optional native NGX DLSS FG path. The default build keeps this
option off. Obtain the NVIDIA DLSS SDK and configure its root explicitly.
Native builds also fetch pinned SafetyHook v0.6.9 and its Zydis dependency:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DXRFG_NATIVE_DLSSG=ON -DXRFG_DLSS_SDK_ROOT=E:/Github/DLSS
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure -LE needs_gpu_timing
ctest --test-dir build -C Release -R '^xrfg_native_dlssg_tests$' --output-on-failure
```

The native GPU test needs an adapter and driver for which NGX reports frame
generation available. The implementation was tested with SDK 310.9.1 at commit
`374959484e79a640feaba44c93ac8cfb0a03f5b5` of NVIDIA/DLSS and
driver 616.56 on an RTX 5090. Use 310.6 or later: its `nvngx_dlssg.dll`
generates the same pixels about 4% faster than 310.5.3 at 3004x3004 (2% for
a turning head), and 310.6, 310.7 and 310.9.1 measure alike. Older SDKs
still build. SDK directories must contain
`include/nvsdk_ngx_helpers_dlssg.h`, `lib/Windows_x86_64/x64/nvsdk_ngx_d.lib`,
`lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib`,
and `lib/Windows_x86_64/rel/nvngx_dlssg.dll`.

The build copies the production feature DLL next to the layer and into
`build/Release/ofxr`. Keep `nvngx_dlssg.dll` with the other files in `ofxr`
when copying the tray build, together with `licenses/NVIDIA-DLSS.txt`.
The tray installs that DLL beside its cached layer. In the tray, select
**NVIDIA DLSS Frame Generation (experimental)** before starting a new OpenXR
session. **OFXR frame generation** restores the existing algorithm. The
corresponding configuration is `[tray] frame_generation=dlss` in the saved
tray settings, or `[ofxr] frame_generation=dlss` for a directly loaded layer.
The V2 provider control uses `frame_generation=1`; zero selects OFXR.

Native generation requires complete, continuous guide publications for both
source frames: motion, depth, their exact output/resource rectangles, jitter,
scales, and valid near/far and reversed/infinite depth conventions. The V2
guide publication API supplies these inputs. Native builds can also capture
these resources directly from a D3D12 DLSS/Ray Reconstruction upscaler.
Capture uses the NGX feature's rectangles, motion scale, jitter and depth flags;
camera depth metadata comes from OpenXR depth submissions or UEVR's public
SDK projection matrix. Both reversed, infinite UEVR eye projections must be
valid and agree before publication. Separate upscaler eyes can map onto a
single side-by-side OpenXR texture. Other depth conventions still require
explicit metadata or the V2 publication API. A motion-only V1 producer can
continue using OFXR; the native path shows the current frame until depth is
available. Its motion statistics report `waiting_for_depth` (status 7).
An unavailable native feature fails initialization and reports the session
error; it does not silently switch algorithms. If NGX later refuses to create
or evaluate a feature, that pair shows the current frame, the statistics
report `native_unavailable` (status 8), and creation is retried after 120
pairs instead of disabling generation. Separate OpenXR composition layers
continue through the existing layer path.

Capture runs whenever DLSS motion vectors are selected, not only for native
generation, so OFXR's own synthesis (`motion_vectors=dlss` with
`frame_generation=ofxr`) uses the game's vectors without a guide provider.
Like native generation, it pairs each view of a side-by-side texture with its
own eye's guide. In Galactic Racer under UEVR on the Meta XR Simulator
(1440x1584 per eye, simulated head still), median GPU time per stereo pair:

| Method | Racing at speed | 3D scene behind a menu |
|---|---|---|
| OFXR + DLSS vectors | 0.13 ms | 0.12 ms |
| OFXR FidelityFX flow | 0.30 ms | 0.30 ms |
| Native DLSS FG 2X | 1.05 ms | 1.08 ms |
| OFXR NVIDIA medium flow | 2.26 ms | 2.3 ms |

Scene motion leaves the medians unchanged; the heavier race widens the tails
(native's 90th percentile rises from 1.08 to 1.5 ms) through contention with
the game's own rendering. With the final build, four interleaved rounds of the
same race measured OFXR + DLSS vectors 0.125 ms, FidelityFX flow 0.30 ms,
native 1.04 ms and NVIDIA medium flow 2.45 ms, every method holding the
simulator's 90 frames a second.

On a Steam Frame through SteamVR (3004x3004 per eye at 120 Hz, with
`power.pauseCompositorOnStandby` off so the session runs unworn), racing the
Arcade time trial at speed, four interleaved rounds of ten seconds per method
measured:

| Method | GPU per stereo pair (median) | Game frames a second |
|---|---|---|
| OFXR + DLSS vectors | 0.44 ms | 117.8 |
| OFXR FidelityFX flow | 0.90 ms | 113.7 |
| Native DLSS FG 2X | 4.9 ms | 109.8 |
| OFXR NVIDIA medium flow | 7.8 ms | 113.5 |

On a second track, Tatooine's King of the Racers podrace, four more
interleaved rounds with the final build measured OFXR + DLSS vectors 0.45 ms
at 116.7 frames a second, FidelityFX flow 0.90 ms at 115.5, native 5.2 ms at
112.0 and NVIDIA medium flow 6.9 ms at 115.5: the same order. The tracks'
sections load the GPU differently, so single rounds of a method ranged by up
to 11 frames a second; NVIDIA flow's long span runs largely on
the optical-flow engine beside the game's rendering. Offline at that size
(`XRFG_TEST_BENCH_EYE=3004x3004` with either benchmark) the same native pair
takes 2.4 ms, so about half of the live figure is the GPU shared with the
game. With 310.9.1 a
pair there costs 1.89 ms with a still head and 2.36 ms turning (3X: 2.93 and
3.50 ms), and a feature per eye would cost 17% more still and 11% more
turning. Live in the game's lighter hub scene, where the GPU is not saturated,
the still-head pair measured 1.93 ms. Skipping the reseed instead is worse: a quarter pixel of
unaligned history already triples the error of a reseeded pair on detailed
content, so the 0.1 pixel threshold stays.

The second game tested is Hubris, a native Unreal Engine 4 VR game with DLSS
310.2.1 (delivered over the air by the driver) and no frame generation of its
own, launched with `-hmd=OpenXRHMD -dx12` through SteamVR. It renders both
eyes with one DLSS feature into a double-wide 5136x2568 swapchain and submits
no OpenXR depth. In its menu scene, where the game is not GPU-bound and every
method held 119.5-119.9 frames a second, the median GPU time per stereo pair
was 0.32 ms for OFXR + DLSS vectors, 0.67 ms for FidelityFX flow, 1.40 ms for
native at 67% resolution, 1.87 ms for native and 3.1 ms for NVIDIA medium
flow. Native's two rounds measured 1.872 and 1.880 ms, and the vectors' 0.323
both times. Starting on DLSS vectors and switching to native in the menu, and
back, measured the same.

A DLSS feature the game created before the capture hook was installed is
recovered from its evaluation parameters, which normally still hold its
creation flags. Without them, capture assumes render-resolution motion and
takes the depth convention from the camera metadata.

A game with no camera metadata at all - Hubris submits no OpenXR depth, and
UEVR is not involved - still states its depth direction in the feature's
`DepthInverted` flag, which its own DLSS relies on. Capture then publishes
nominal 0.1 and 1000 planes (or NGX's own `DLSSG.CameraNear`/`CameraFar`
parameters, if the game set them) in that direction. Offline, NGX's output
was bit-identical for near planes from 0.01 to 100, finite or infinite, while
the wrong direction took the moving-edge error from 8.7 to 35.3. While the
convention is unknown, capture still publishes the motion, with no depth:
OFXR + DLSS vectors needs nothing more, and native shows the current frame
(`waiting_for_depth`).

OFXR never shuts NGX down. The driver keeps one NGX instance per adapter for
the whole process, and `NVSDK_NGX_D3D12_Shutdown1` shuts down every loaded
feature module for that device, including the game's own DLSS upscaler.

That shared instance also decides where feature DLLs come from: NGX searches
the path list of the process's first initialisation only. In Hubris, which
starts NGX for its own DLSS before OFXR does and ships no
`nvngx_dlssg.dll`, OFXR's later initialisation succeeded, but the
capabilities reported frame generation unavailable with
`FrameGeneration.FeatureInitResult` 0xBAD00004 (feature not found).
Creating the feature anyway failed with 0xBAD0000B, and loading OFXR's copy
of the DLL into the process beforehand changed nothing. The SDK's static
library routes every D3D12 initialisation through the core's exported
`NVSDK_NGX_D3D12_Init_Ext` or `NVSDK_NGX_D3D12_Init_ProjectID`, with a copy of
the caller's `NVSDK_NGX_FeatureCommonInfo` as the last argument. So when the
OpenXR instance is created for D3D12 with native generation or DLSS vectors
selected, the layer loads the driver's `_nvngx.dll` (from
`HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath`) and hooks those
two exports, appending its own folder to each call's path list. Engines
create the instance before their renderer to choose the adapter, so the hook
precedes the game's DLSS. In Hubris this made frame generation available,
with up to five frames reported, and loaded OFXR's DLL. Paths the game lists
come first, so a game's own `nvngx_dlssg.dll` - Galactic Racer ships 310.6.0
with its Streamline plugin - is still the one used. A game that started NGX
before the layer loaded, such as one UEVR injects later, and that ships no
`nvngx_dlssg.dll` cannot run native generation; it still runs OFXR + DLSS
vectors. `XRFG_TEST_NATIVE_DLSSG_NO_DISCOVERY=1` leaves the paths alone, to
reproduce that case. With `XRFG_TEST_NATIVE_DLSSG_VERBOSE=1` the layer
records NGX's initialisation result, the frame-generation capability values
and any feature-creation failure in
`%LOCALAPPDATA%\OFXR Bridge\NGX\ofxr-native-dlssg.log`.

Nor does it initialise NGX twice for a device. A UEVR resolution change
mid-race once faulted the GPU, with 3D HEIGHT and WIDTH CT violations, in the
half second the swapchains were recreated. That was with native generation on
the Meta XR Simulator, going from 1440x1584 to 2154x2369 per eye. The old
swapchain's feature had released the device's NGX entry, so the new one
initialised NGX again while the game was recreating its own DLSS features. The
same resize with the layer unloaded ran cleanly. With the device kept
initialised, resizes from 1.0 to 1.5, back to 1.0 and on to 2.0 ran without a
fault, one run each. Swapchain teardown likewise keeps the hand-over copier's
command lists and staging alive when its copies are not seen to finish within
two seconds, rather than freeing them under the GPU.

Both eyes of a stereo pair share one native feature, side by side, with a
64-pixel seam between them that repeats each eye's edge. Most of an NGX
evaluation's cost is fixed, so one double-width evaluation costs far less than
one per eye; the seam keeps either eye's history from reaching the other.
A single eye, or `XRFG_NATIVE_DLSSG_PER_EYE=1`, gives each eye its own feature
at its submitted viewport size, and a shared feature NGX refuses falls back to
that automatically. NGX refuses features wider or taller than 8192 pixels, so
eyes too wide for both and a full seam narrow the seam to fit, down to 16
pixels, and wider eyes (from 4089 pixels) get a feature each without a refused
pair. The shared feature measured cheaper than one per eye at every size
tried: by 29% (still head) and 28% (turning) at 1440x1584, 18% and 14% at
2448x2448, 17% and 11% at 3004x3004, and 14% and 9% at 3600x3600. `XRFG_NATIVE_DLSSG_SEAM` overrides the seam width for
experiments; without a seam the eyes visibly bleed into each other.

A is rotationally mapped into B's camera plane; the same mapping removes
tracked rotation/FOV motion from engine vectors. When that alignment moves any
pixel by more than a tenth of a pixel, the feature is reseeded with the
aligned A before evaluating B; a still head or a translation alone keeps the
history. A reset clears a shared feature's history for both eyes, so a reseed
packs the aligned A of both. It packs only A's colour: NGX's output from a
reset is bit-identical whatever motion and depth it is given, even noise
(checked with SDK 310.5.3 and 310.9.1), so the seed writes neither and the
reset reads a 64-pixel square of them. With the seed's own 16x8 thread groups,
which suit its rotated bilinear reads better than the pack's 8x8, a turning
pair at 3004x3004 costs 2.49 rather than 2.58 ms; a still head is unchanged.
Keeping the history through small rotations instead, and aligning the
generated image into B's camera in the compose pass, saves the reseed but
measured two to five times the error on detailed content even at a quarter
pixel of rotation; `XRFG_TEST_NATIVE_DLSSG_ROTATION_SWEEP=1` repeats that
quality measurement for the reseeding path.
This preserves OFXR's current B pose/FOV contract and its bit-exact real-frame
copy. On an RTX 5090 at 2064x2208 per eye with SDK 310.9.1, a stereo pair
takes about 1.32 ms of GPU time at 2X with a still head and 1.63 ms with a
turning head, and 2.00/2.36 ms at 3X; through the synthesizer at 2004x2004 a
2X pair takes 1.48 ms. Both benchmarks generate from patterned frames: blank
ones compress to almost nothing in GPU memory, and NGX then measures 20-25%
faster than it does on real content. Set `XRFG_TEST_NATIVE_DLSSG_BENCH=1`
and run `xrfg_d3d12_history_tests` to repeat that measurement;
`XRFG_TEST_NATIVE_DLSSG_LAYOUT_BENCH=1` times NGX alone for one eye, two
features and one shared feature. `XRFG_TEST_FG_BENCH=1` instead times every
frame-generation method through the synthesizer (OFXR FidelityFX and NVIDIA
flow, DLSS vectors, native 2X/3X) at 2004x2004 per eye, plus the game-side
guide snapshot copies. Close VR games first: GPU contention makes the medians
meaningless. Its median GPU time per stereo pair on an RTX 5090, by eye size
(`XRFG_TEST_BENCH_EYE`):

| Method | 1440x1584 | 2004x2004 | 3004x3004 | 3600x3600 |
|---|---|---|---|---|
| OFXR + DLSS vectors | 0.13 ms | 0.23 ms | 0.52 ms | 0.72 ms |
| OFXR FidelityFX, half-res flow | 0.20 ms | 0.31 ms | 0.58 ms | 0.78 ms |
| OFXR FidelityFX, full-res flow | 0.38 ms | 0.64 ms | 1.30 ms | 1.80 ms |
| Native DLSS FG 2X | 1.14 ms | 1.49 ms | 2.38 ms | 3.42 ms |
| OFXR NVIDIA medium flow | 1.30 ms | 1.61 ms | 2.89 ms | 3.80 ms |
| Native DLSS FG 3X | 1.66 ms | 2.13 ms | 3.73 ms | 5.02 ms |

NGX receives colour display-encoded, as its programming guide requires, and
motion as a fraction of the feature with the feature's size as its motion
scale. The same vectors in pixels with a unit scale measurably lose quality.
The vectors are passed undilated and NGX dilates them at depth edges; dilating
them in the pack measured the same and cost about 35 us more per pair.
When the game renders both guides at two thirds of the output or less (DLSS
Quality and below), they are packed to a grid two thirds the feature's size,
which loses none of their detail, and NGX is told that render size. A pair at
3004x3004 is then 1.8-2.2% faster at the median and the quality tests measure
the same; a half-size grid measured worse depth edges, and NGX's motion scale
must then be the grid's size, not the output's.

Of a 2X pair at 3004x3004, OFXR's own work is the pack of B (about 180 us),
the reseed's colour-only pack of the aligned A (about 105 us, turning head
only) and the composition (about 75 us); NGX's evaluations are the rest (the
reset's about 250 us). With SDK 310.9.1, NGX's evaluations alone take 1.21 ms of a 1.32 ms
pair at 2064x2208 (1.47 of 1.63 ms turning), and at 3004x3004 1.66 of 1.89 ms
(2.02 of 2.36 ms). The
remainder writes the full-resolution colour, motion and depth NGX takes in its
own feature layout and composes its output, about 350 MB per pair at
3004x3004: as long as the RTX 5090's memory bandwidth needs to move it. With
patterned frames, 16-bit depth for NGX and a 16 rather than 64 pixel seam
still measure the same. NGX evaluates 8-bit colour faster than 10-bit, so an
8-bit swapchain's frames are packed at 8 bits, which hold their codes exactly,
while the reseed's resampled A keeps 10 bits in a texture of its own (about 73
MB at 3004x3004). That makes a pair 2-4% faster than 10 bits throughout and
lowers the error of almost every quality test (the rotation sweep's from 0.32
to 0.31; two sRGB stereo cases move by 0.0001); NGX takes the differently
formatted reset with SDK 310.5.3, 310.6 and 310.9.1 alike. 8 bits throughout
would be 3-4% faster still, but rounding the resampled A raises the sweep's
error to 0.40. Other swapchain formats keep 10 bits. These also measured no faster or slower, within about 15 us
at 3004x3004: 16x16, 32x8 or 16x8 groups for B's pack (wider groups help only
the seed), NGX's undocumented `DLSSG.InternalWidth` (50 and 75%),
`DLSSG.DynamicResolution` and `DLSSG.EvalFlags` (0, 1, 2, 4, 8) parameters,
every evaluation option (`notRenderingGameFrames`, `menuDetectionEnabled`,
`colorBuffersHDR`, `cameraMotionIncluded`, `orthoProjection`,
`automodeOverrideReset`, and `minRelativeLinearDepthObjectSeparation` at 1 and
1000), and giving the seed its own input textures so both packs run before
either evaluation, which would also cost about 220 MB more video memory.
32-bit motion is 2% slower, and NGX's depth edges measure worse with it (0.33
against 0.21). These feature versions accept only render preset 1.

Generating below the eye's resolution is a trade rather than an optimisation,
so it is an option: `[ofxr] dlssg_resolution`, 25 to 100 percent per axis and
100 by default. The tray offers 100, 67 and 50 under **DLSS Frame Generation
resolution** (`[tray] dlssg_resolution`), and `OFXR_RequestNativeDlssgScaleV2`
changes it in a running game. Below 100 the pack averages the real frame into
the smaller feature and maps the guides onto its grid. The composition then
upsamples NGX's frame and restores the real frame's detail: following B's
engine motion to where each generated pixel's content lies in B, it adds B's
texel there less B as packed, wherever the generated frame agrees with the
packed B to within half the display range. Its second step follows
the nearest surface's motion among a depth texel and its four neighbours, as
NGX dilates at depth edges; that cut the error at moving edges by a tenth for
about 30 us. Where the frames disagree - an occlusion, or content the vectors
do not describe - the pixel stays as generated, and softer. At 3004x3004 a
turning-head 2X pair costs 1.74 ms at 67% and 1.40 ms at 50%, against 2.35 ms
(still head: 1.48 and 1.24 against 1.90; 3X turning: 2.55 and 2.09 against
3.50). Live in Galactic Racer on the
Meta XR Simulator at 2160x2376 per eye, racing, two interleaved rounds
measured 1.60 ms per pair at full resolution, 1.19 at 67% and 0.97 at 50%
(OFXR + DLSS vectors: 0.26). On the Steam Frame at 3004x3004 and 120 Hz,
racing the Arcade time trial, five interleaved ten-second rounds per setting:

| Method | Game frames a second (mean) | GPU span per pair (median) |
|---|---|---|
| Native DLSS FG 2X, 100% | 99.5 | 4.4-4.9 ms |
| Native DLSS FG 2X, 67% | 106.3 | 1.49-1.51 ms |
| Native DLSS FG 2X, 50% | 107.8 | 1.24-1.25 ms |
| OFXR + DLSS vectors | 112.5 | 0.45 ms |

The game's frame rate is the measure here: against OFXR + DLSS vectors' frame
time, native costs the game about 1.6 ms a frame at full resolution, 1.0 at
67% and 0.8 at 50%. A long pass's span overstates its cost, because the GPU
runs the game's work in between: the full-resolution span is twice its
offline time, while 75% (1.7 ms) and below match theirs. A feature per eye
did not avoid that (5.0-5.3 ms at full resolution).

`XRFG_TEST_NATIVE_DLSSG_SCALE_QUALITY=1` measures what that costs on a 1024x768
scene where a detailed background slides behind a striped square, with
two-thirds guides. The scene is rendered analytically with 4x4 supersampling,
so the true midpoint frame is exact. Four pairs of slides are averaged: 3.3 to
9.4 pixels for the background and 9.8 to 21.2 for the square, every one a
fraction of a pixel on each scale's grid. A single scene with whole-pixel motion
on one scale's grid, as an earlier version of this benchmark had, flattered that
scale several times over. Against the true midpoint frame:

| Method | Error | At the square's edges | Without the detail restore |
|---|---|---|---|
| Native DLSS FG, 100% | 4.25 | 8.7 | |
| Native DLSS FG, 85% | 3.98 | 10.0 | 6.7 |
| Native DLSS FG, 75% | 6.8 | 18.3 | 10.0 |
| Native DLSS FG, 67% | 5.37 | 13.6 | 8.7 |
| Native DLSS FG, 50% | 10.7 | 23.4 | 15.4 |
| OFXR + DLSS vectors | 1.74 | 7.55 | |
| OFXR FidelityFX flow, half / full resolution | 15.8 / 13.7 | 45.2 / 45.6 | |
| OFXR NVIDIA medium flow | 14.1 | 51.3 | |
| A blend of the two frames | 28.8 | | |

The composition upsamples the reduced frame with Catmull-Rom rather than
bilinear: 7% less error at 67% for no measurable cost. The packed B it compares
with stays bilinear, since filtering both alike measured worse.

With the game's vectors exact, as here, OFXR + DLSS vectors now has less error
than native generation everywhere, at the moving edges too (7.55 against 8.7),
for about a sixth of the GPU time. Native generation at 67% errs more than
both (13.6 at the edges), and at 50% far more (23.4). Real frames tell
differently (below): there 67% and 50% erred alike, and less than 100%, so
the tray offers 50% again. Native generation needs the same DLSS guides as
OFXR + DLSS vectors, so that is always the cheaper alternative to it. The optical-flow
methods, for games without DLSS, lose the fast striped square entirely.

`XRFG_TEST_SCALE_QUALITY_EFFECT` adds content the game's vectors do not
describe to the same scene, carrying the background's vectors. `shadow` is
the square's shadow moving with it over the background. `translucent` is a
translucent disc sliding the other way. `particles` are 150 small bright
sprites, each moving its own way. `novelocity` gives the square no vectors of
its own, as materials that output no velocity do. Error where the effect
shows, two pixels around (for `novelocity`, overall and at the edges):

| Method | Shadow | Translucent | Particles | No velocity |
|---|---|---|---|---|
| OFXR + DLSS vectors | 6.16 | 8.75 | 30.6 | 16.9 / 62.4 |
| Native DLSS FG, 100% | 5.93 | 10.8 | 32.4 | 17.1 / 47.9 |
| Native DLSS FG, 67% | 7.15 | 10.4 | 36.1 | 16.9 / 53.7 |
| OFXR FidelityFX flow, half / full | 14.8 / 14.0 | 22.9 / 21.4 | 42.2 / 41.7 | 15.8 / 13.7, 45 |
| OFXR NVIDIA medium flow | 14.3 | 21.8 | 42.1 | 14.1 / 51.3 |

Native generation is no better than OFXR + DLSS vectors where the vectors are
wrong: NVIDIA's network leans on the same vectors. Particles defeat every
method, and an object that writes no velocity defeats both vector methods,
with optical flow a little better overall.

Real game content is measured from recorded frames. With
`XRFG_TEST_CAPTURE_FRAMES=<folder>` in the game's environment, the
synthesizer copies each source it is handed - colour, views, and every eye's
motion and depth snapshot - on the GPU, and writes a run to
`<folder>/seq<N>/frame<M>` only once it is complete (`XRFG_TEST_CAPTURE_COUNT`
frames, default 3; `XRFG_TEST_CAPTURE_SEQUENCES` runs, default 4,
`XRFG_TEST_CAPTURE_GAP` frames apart, default 600; `XRFG_TEST_CAPTURE_SKIP`
frames first, or `XRFG_TEST_CAPTURE_WAIT=1` to start when a file named `go`
appears). Writing each frame as it came stalled the game for over 100 ms, so
the frames of a run were unevenly spaced in time; copying first keeps them
within a few milliseconds of the game's own pacing. `XRFG_TEST_REPLAY` then
generates frame 1 of a run from frames 0 and 2 (`XRFG_TEST_REPLAY_FIRST`
picks another start) with every method. Frame 2's vectors are scaled by
(dt1 + dt2) / dt2 from the recorded DLSS frame times to reach frame 0, and
OFXR's methods generate at dt1 / (dt1 + dt2); native generation is fixed at a
half. The error is against the real frame 1, overall and where frames 0 and
2 differ by more than 6 of 255. `XRFG_TEST_REPLAY_SAVE` writes crops, and
`XRFG_TEST_REPLAY_VECTOR_GAIN`, `_GUIDE_SHIFT` and `_SWAP_EYES` test the
guides.

The first Galactic Racer recordings showed the game's vectors fitting the
image motion worse than a blend. Compared with OpenCV's optical flow between
the frames, each eye's colour matched the other eye's vectors (median
difference 0.7 pixels, against 3.6 and 4.6 for its own). Galactic Racer has a
DLSS feature per eye, and the resolver assigned the two streams to eyes in
the order they first appeared; on every recording the right eye's came first.
The guide marked as the left eye had its depth at x=2008 of UEVR's
double-wide depth target. With the eyes swapped back, the replays' error
where the scene moved fell from 9.6 to 3.5 (OFXR + DLSS vectors) and 4.8 to
3.6 (native) on one run, and from 10.4 to 4.6 and 5.8 to 5.1 on another. The
resolver now orders the eyes by where their motion and depth rectangles sat
in the game's targets before the snapshots cropped them, and by first
appearance only when those are equal.
`test_dlss_motion_vector_stereo_stream_pairing` publishes the right eye
first from a shared side-by-side target.

Recorded again with the fix, on Galactic Racer's Jakku time trial on a Steam
Frame (3004x3004 per eye), six triplets with even frame times measured, where
the scene moved:

| Method | Average | Range |
|---|---|---|
| Native DLSS FG, 67% | 8.4 | 4.1-10.7 |
| Native DLSS FG, 100% | 8.5 | 4.1-11.1 |
| OFXR FidelityFX full-res flow | 8.8 | 4.2-12.8 |
| OFXR NVIDIA medium flow | 8.9 | 4.2-13.1 |
| OFXR FidelityFX half-res flow | 9.2 | 4.3-12.8 |
| OFXR + DLSS vectors | 10.7 | 4.7-16.4 |
| A blend of the two frames | 14.6 | 9.5-19.4 |

OFXR + DLSS vectors had the least error in one triplet and was within a few
tenths of the best in two more. In one run, along a wall passing fast and
close, the game's vectors did not describe the motion at any scale
(`XRFG_TEST_REPLAY_VECTOR_GAIN` from 0 to 2, `_GUIDE_SHIFT` of a frame either
way) and it erred 16.1 and 16.4, as much as a blend, while native generation,
whose network has its own flow, erred 10.6 and 10.9 and the optical-flow
methods 9.1 to 10.9.

Eight more triplets, recorded later in the same race with the one-shot
trigger, gave, where the scene moved: OFXR + DLSS vectors 7.81, native 7.91
at 67% and 8.05 at 100%, FidelityFX flow 8.14 (half) and 8.20 (full), NVIDIA
flow 8.32, a blend 13.0. Over all fourteen, native generation at 67% had the
least error (8.1), then 100% (8.2), optical flow (8.5-8.6) and OFXR + DLSS
vectors (9.1). The same eight triplets checked three things:

- OFXR + DLSS vectors' occlusion handling holds on real frames: the shader
  before it measured 8.47 against 7.81, worse on six of the eight.
- With the eyes put right, native generation uses the game's vectors well:
  scaled by 0, 0.5 or 1 they measured 7.82, 8.26 and 7.55 at 100% (7.39 at
  67% with 1).
- Native generation's own settings, by its mean error:

  | Resolution | No detail restore | Detail restore 2 (default) | Restore 4 |
  |---|---|---|---|
  | 100% | | 8.05 | |
  | 85% | | 7.79 | |
  | 75% | | 7.84 | |
  | 67% | 8.35 | 7.91 | 7.90 |
  | 50% | 8.59 | 7.91 | 7.89 |

  Every reduced resolution beat full resolution, and 50% matched 67%: the
  sharp synthetic scene overstates what generating at a lower resolution
  loses in a game whose DLSS renders at two thirds of the output. The
  detail restore is worth 0.4-0.7; strengths 2 and 4 measure alike. 85% is
  best by a little, but it costs more than 100% (it loses the two-thirds
  guide grid), so the tray offers 100%, 67% and 50%.

#### The sweep on 42 recorded triplets

Recorded later on a Steam Frame (3004x3004 per eye) and replayed with every
setting: 22 triplets from Galactic Racer's Jakku time trial and 20 from its
Tatooine podrace, including a crash with debris, a cave and the pod's shadow
on fast ground. Besides the error where the scene moved, the replay now
scores SSIM over 8x8 luma blocks, sharpness (the output's gradient against the
truth's, below 1 blurrier) and gradient error, since the absolute error alone
favours a blur. Paired differences are against the same triplets' baseline.

Native generation, by resolution (detail restore 2, NGX's depth scale 1):

| Resolution | Error | SSIM | Sharpness | Gradient error |
|---|---|---|---|---|
| 100% | 8.15 | 0.770 | 0.75 | 8.40 |
| 85% | 7.96 | 0.785 | 0.83 | 8.03 |
| 75% | 8.01 | 0.785 | 0.83 | 8.07 |
| 67% | 8.04 | 0.784 | 0.83 | 8.13 |
| 60% | 8.14 | 0.781 | 0.83 | 8.23 |
| 50% | 8.04 | 0.787 | 0.86 | 8.18 |
| 40% | 8.40 | 0.775 | 0.88 | 8.57 |

Full resolution is the blurriest: the detail restore, which only runs below
100%, also puts back sharpness NVIDIA's frame lacks. Restoring the real
frame's detail above a small blur at 100% as well (offsets of 0.5-1.5 pixels,
with or without a stricter agreement weight) erred 0.12-0.42 more, as the
detail landed where the vectors were wrong. So the default is now 67%: less
error than 100%, sharper, and about a quarter cheaper. 85% is best by a
little but costs more than 100%.

Native settings, paired against the above (negative is better):

| Setting | 100% | 67% |
|---|---|---|
| NGX linearised depth scale 0.03 / 0.05 / 0.1 / 0.2 / 0.3 / 0.5 | -0.13 / -0.13 / -0.12 / -0.12 / -0.10 / -0.08 | -0.10 / -0.10 / -0.09 / -0.08 / -0.07 / -0.06 |
| Depth scale 10 | +0.01 | -0.00 |
| Near/far partition 150 / 4000 | +0.10 / +0.24 | +0.03 / +0.20 |
| Object separation 10 / 160 | +0.37 / -0.09 | +0.36 / -0.04 |
| Depth scale 0.1 with partition 150 / separation 10 | -0.10 / +0.08 | -0.07 / +0.12 |
| Vectors marked as already dilated | +0.21 | +0.00 |
| A feature per eye | -0.00 | +0.00 |
| Seam 16 / 128 | +0.00 / -0.03 | +0.03 / +0.01 |
| Vectors scaled by 0.8 / 1.2 | +0.44 / +0.79 | +1.19 / +0.98 |
| Detail restore 0 / 1 / 4 / 8 (67%; 50% alike) | | +0.23 / +0.02 / -0.01 / -0.01 |

The depth scale of 0.1, NVIDIA's suggestion for compressed depth, is now the
default: better on 31 and 32 of the 42 triplets, with SSIM up 0.0016-0.0018.
Detail restore 4 improved the gradient error by 0.08-0.12 but the error by
only 0.01-0.03, on half the triplets, so 2 stays.

OFXR's flow options, with the composition below:

| Configuration | Error | SSIM | Gradient error |
|---|---|---|---|
| NVIDIA slow, half input | 7.59 | 0.792 | 7.93 |
| NVIDIA medium, half / three-quarter / full | 7.67 / 7.69 / 7.67 | 0.791 / 0.791 / 0.791 | 7.94 / 7.93 / 7.92 |
| NVIDIA fast, half / full | 7.94 / 7.99 | 0.777 / 0.776 | 8.29 / 8.33 |
| NVIDIA medium bidirectional / slow full bidirectional | 8.40 / 8.32 | 0.779 / 0.780 | 8.16 / 8.15 |
| FidelityFX half / three-quarter / full | 7.86 / 7.82 / 7.83 | 0.779 / 0.781 / 0.782 | 8.16 / 8.12 / 8.08 |
| OFXR + DLSS vectors | 8.40 | 0.806 | 7.92 |

With every OFXR method forced to native generation's fixed half, as when frame
times are uneven, OFXR + DLSS vectors measured 9.10, FidelityFX 8.25 and NVIDIA
medium 8.12, against native's 7.96 at 67%.

**Optical flow's composition.** Where the flow's two samples disagreed, the
flow methods faded to the same-pixel blend after the headset's turn. On real
game frames that blend is a double image, so it was replaced, measured on all
42 triplets (FidelityFX half / NVIDIA medium / NVIDIA fast):

- Solving for the flow's endpoint, as OFXR + DLSS vectors does, sharpened but
  erred more: 8.25 / 8.44 / 9.30 against 8.21 / 8.17 / 9.01. Flow fields are
  too noisy for it.
- Trusting the flow everywhere: FidelityFX 8.02, but a pure head turn erred
  13.6 against 0.16, since the camera-only blend is exact there.
- Whichever explains both frames better, the flow or the camera alone, with
  the flow keeping a tie: FidelityFX 7.85 at a slope of 4, 7.86 at 8 and 7.91
  at 16; without the tie's bias, 8.32. Slope 8 keeps a pure head turn at 0.08
  (slope 4: 0.57, which `xrfg_vertical_fov_tests` rejects). NVIDIA medium
  7.67, fast 7.94.
- Keeping NVIDIA's cost and consistency weights on top of the selection:
  medium 7.74, fast 8.70; the fast preset's endpoint check alone, fast 8.64;
  the bidirectional consistency alone, 8.36. B's warped sample where the
  forward flow does not lead back: 8.81 (8.66 with a looser threshold). All
  lean on the blend or on one side where the game moved, so only the
  bidirectional option keeps its consistency check.
- B's warped sample where the flow is chosen but its samples disagree: 8.18 /
  7.90, worse on 40 of the 42.

**OFXR + DLSS vectors' motion edge.** B's sample stands in for disagreeing
samples wherever a vector 16 pixels away differs. Thresholds of 1, 0.5, 0.25
and 0.1 pixel measured 8.49, 8.44, 8.43 and 8.40; always, 8.34, but that
steps a fade (22.5 on the synthetic fade, which a positive threshold keeps at
0.08 as long as the motion is uniform). Second differences, which ignore
smooth gradients, measured 8.64 on 20 triplets: the gradient's B sample helps.

**The hybrid.** DLSS vectors + FidelityFX flow, choosing per pixel by which
branch's samples agree better (lower is better):

| Variant | Error | SSIM | Gradient error |
|---|---|---|---|
| Selection slope 8, flow loses ties | 7.19 | 0.814 | 7.59 |
| Slope 4 / 16 | 7.21 / 7.17 | 0.818 / 0.810 | 7.49 / 7.70 |
| Tie bias 0.5 / 1.5 | 7.18 / 7.29 | 0.807 / 0.815 | 7.69 / 7.59 |
| Samples compared blurred over 2x2 (kept) | 7.17 | 0.815 | 7.54 |
| Plus the vectors' covered-background choice trusted | 7.56 | 0.807 | 7.89 |
| Plus no flow where the vectors' samples agree within 0.02 (kept) | 7.18 | 0.815 | 7.54 |

Comparing unblurred samples, sharp stripes resampled at a fraction of a pixel
made exact vectors look worse than a flow that was a stripe off: the
synthetic striped square erred 3.14 against 1.74 for the vectors alone;
blurred, 1.96. Its shadow scene's shadow erred 5.36, against 6.16 for the
vectors. At the synthetic occlusion edges it erred 4.5 against 0.68; trusting
the vectors' covered-background conclusion cut that to 2.8 but cost the real
frames 0.4, since their vectors are not exact. 1.61 ms a pair offline at
3004x3004, 1.58-1.71 ms live.

Since release 18 the hybrid is the default. The engine is chosen when a
session starts, before the game has necessarily run DLSS, so the layer starts
on the tray's engine and the first DLSS publication in the process
(`dlss_motion_vector_publications`) takes the hybrid up through
`apply_embedded_control`, as a settings change would; a game without DLSS
keeps the tray's engine at its own resolution rather than FidelityFX alone at
a quarter. In Galactic Racer the game ran DLSS 2.5 s after the session was
created and 7 s before generation began, so the first context built was
already the hybrid, at 1.43 ms a pair at 2316x2316. Hybrid pairs had also
counted as temporal rejections in the guide statistics (one rejection for
every use); they no longer do.

#### Extrapolation

Meta's runtimes were examined for how they predict. The PC runtime's ASW
extrapolates from optical flow (video-encoder macroblock motion, median
filtered) by splatting a mesh, the smallest displacement winning overlaps,
with no hole filling and a prediction factor clamped to [-1, 1.5]. The PC
runtime advertises no Application SpaceWarp; the Meta XR Simulator's AppSW
draws a grid mesh with a vertex per motion-vector texel, displaced by the
app's motion and depth in world space, depth-tested, each vertex taking the
nearest-depth vector of four taps, sky taking camera motion only, and the
mesh stretching over disocclusions. OFXR's extrapolation gathers instead of
splatting, in the same composition pass as its interpolation, but orders
surfaces by the game's depth as AppSW does and stretches the background the
same way. A mesh warp, built since, measured better and replaced the gather
for extrapolation (below).

On the 42 triplets, predicting frame 2 from frames 0 and 1 a whole frame
ahead (1 + dt2/dt1 spans from A):

| Variant | Error | SSIM |
|---|---|---|
| Showing frame 1 again | 20.62 | 0.489 |
| 13 candidates, ordered by motion (faster nearer), two triplets only | Tatooine a0: 36.8 (depth: 20.6), where the pod moves with the camera; Jakku seq1: 7.7 (7.3) | |
| 13 candidates, ordered by the game's depth | 12.58 | 0.677 |
| Plus a still-content hypothesis checked against A | 12.56 | 0.674 |
| Search only near motion edges (second differences within 48 px) | 12.56 | 0.676 |
| Nine candidates over three steps there (until release 7) | 12.68 | 0.679 |
| Five: the pixel and its four neighbours 12 px off (kept) | 12.63 | 0.684 |
| Five, 8 px off | 12.66 | 0.686 |
| From FidelityFX's flow alone, half / full resolution | 12.44 / 12.12 | 0.651 / 0.658 |
| The flow's edge test at 6 px and its four nearest starts, 8 px off (kept) | 12.41 | 0.660 |
| Vectors and flow, the better prediction per pixel (`extrapolate=2`) | 11.87 | 0.684 |
| The same with five candidates (kept) | 11.88 | 0.688 |

The flow's search was tuned on the same triplets, timed warm on them
(`XRFG_TEST_REPLAY_TIMING_PAIRS=7`, one replay at a time):

| Flow extrapolation variant | Error | SSIM | Gradient error | GPU a pair |
|---|---|---|---|---|
| Edge test at 3 px, nine starts at 12 and 40 px | 12.46 | 0.652 | 12.40 | 1.94 ms |
| Edge test at 6 px | 12.46 | 0.654 | 12.40 | 1.69 ms |
| Edge test at 12 px | 12.52 | 0.654 | 12.43 | 1.47 ms |
| Five starts, at 12 px | 12.39 | 0.658 | 12.20 | 1.35 ms |
| 6 px, five starts at 12 px | 12.40 | 0.659 | 12.20 | 1.22 ms |
| **6 px, five starts at 8 px** (kept) | **12.41** | **0.660** | **12.16** | **1.22 ms** |
| 6 px, five starts at 20 px | 12.43 | 0.657 | 12.27 | 1.22 ms |
| 9 px, five starts at 12 px | 12.43 | 0.659 | 12.22 | 1.15 ms |
| 6 px, nine starts at 12 and 24 px | 12.40 | 0.657 | 12.31 | 1.68 ms |
| Two steps instead of three | 13.05 | 0.641 | 12.64 | 1.67 ms |

FidelityFX's flow comes in blocks, so its motion steps by a few pixels
between them where there is no edge, and its neighbours 40 px off are another
block's noise as often as another surface. On the same frames, timed the same
way, extrapolating from the vectors cost 1.13 ms and from both 1.93 ms with
nine candidates, and 0.76 ms and 1.49 ms with five; letting the vectors skip
the flow below an error of 0.04 or 0.08 instead of 0.02 saved 1% and erred
more (11.90, 11.98). In
release 6 the vectors-only shader inherited the combined mode's constants
layout and read depth from the wrong ones (13.83 error, SSIM 0.666); that is
fixed.

A shadow cast by the pod moves with the ground's vectors; the still-content
hypothesis did not fix it, since the ground's texture moves under it, but the
flow follows it. The per-pixel choice between the two compares each one's
point against the frame before, blurred a little, and matches the better of
the two per triplet (11.78). Timed warm, live in Galactic Racer at 2316x2316
per eye it cost 1.27-1.91 ms a pair, against 0.24 ms from the vectors alone
and 1.44-1.74 ms from the flow before its tuning: real frames' vectors rarely
explain the frame before closely enough to skip the flow, so FidelityFX's
flow is most of its cost. Its flow pass offers only its own point; letting it
search too cost twice as much on the benchmark for 0.05 less error. The
layer shows the real frame at its own display time and the prediction a
period later, with the deferred current copy and the deeper pipeline off and
a two-slot synthetic ring; `xrfg_layer_extrapolate_*` check the order inline,
on the presenter and pipelined. Live in Galactic Racer the flight log showed
every real frame handed over before its prediction.

Latency was measured from the flight log rather than inferred from that
order. `presenter_content` records, per submission, the display time the game
was promised for the newest real frame in it and the display time it went
down for; `clock_origin` puts the log's clock on the performance counter,
which is SteamVR's XrTime. Against the game's xrWaitFrame returns
(`latency_compare.py` in the Galactic Racer test folder), a real frame went
down 45.5-47.0 ms after the wait extrapolating, 54.1-55.3 ms for native
generation interpolating in the same session, and 62.5 ms interpolating with
the deeper pipeline. Those runs also showed every promise early: by a period
extrapolating and two with the deeper pipeline, because the game took more
than one display period to hand its frame over. `promise_shown_time` now
follows the measured lateness in whole periods - the median of a 64-frame
window, once two windows in a row name it and it saves a quarter of a period
a frame; afterwards real frames went down at their promised time in every
mode. It first required nine in ten of a window to agree, which Kayak VR
(Unreal Engine 4, OpenXR) never did: its frames go down in two groups a frame
apart, 72% and 27%, and with FidelityFX flow and NVIDIA's slow preset the
larger group went down a period after its promise. With the median 73% go
down at it in every method (`xrfg_layer_promise_two_groups`, every fourth
frame handed over late, fails with the old rule). At
UEVR's full resolution the Jakku race loaded the GPU enough that SteamVR
halved the rate in some rounds (its lead from wait to display then reads 27 or
50-58 ms instead of 35.3); those rounds are left out.

#### Where each synthetic is placed

A synthetic is shown one display period before its real frame
(interpolating) or after it (extrapolating), and the layer placed it by the
display times the game stamped its frames with: `1 - period / interval`, or
`1 + period / interval`. Scored against where each synthetic was actually
shown between its two real frames (`presenter_content`, kinds 2 and 1; the
synthetic's share of the way from the previous real frame's shown time to
the current one's), the stamps were a poor guide. Across seven recorded
races, for every gap between the stamps the median of where the synthetic
belonged was the midpoint:

| Stamp gap (periods) | Pairs | Mean | At 1/3 | At 1/2 | At 2/3 |
|---|---|---|---|---|---|
| 0 | 670 | 0.487 | 13% | 83% | 4% |
| 1 | 1,359 | 0.489 | 23% | 60% | 17% |
| 2 | 42,432 | 0.500 | - | 98% | - |
| 3 | 2,255 | 0.494 | 32% | 40% | 28% |
| 4 | 797 | 0.503 | 15% | 68% | 17% |
| 6 | 598 | 0.516 | 18% | 55% | 28% |

A pair takes its two slots whatever the stamps say (Galactic Racer's swing a
period either way; Unreal Engine 4's OpenXR plugin stamps one frame in ten of
Hubris with the time of the frame before), and a frame that misses its slot
repeats the frame before rather than widening the pair - which side of the
synthetic the repeat falls on is not known when it is made. There the
midpoint is also the smoother choice: the stall is the repeat, and the
midpoint spreads what is left of the motion evenly over the frames after it.
Mean fraction error on the recorded races:

| Placement | GR A | GR B | GR C (halved) | GR D (halved) | Hubris |
|---|---|---|---|---|---|
| Stamps (before) | 0.0197 | 0.0195 | 0.0811 | 0.0618 | 0.0173 |
| Stamps, with the period doubled while SteamVR halves the rate and a repeated stamp placed in its slot | 0.0200 | 0.0172 | 0.0303 | 0.0392 | 0.0027 |
| The cadence's share | **0.0100** | **0.0100** | **0.0160** | **0.0219** | **0.0011** |

Other rules on the stamps (a two-frame jump at the usual hand-over rhythm
taken as a lead) and rules on the hand-over timing were tried as well; none
came near. Live on the final build, in Galactic Racer on the Steam Frame at
2316x2316 (stamps here with the doubled period):

| Mode | Cadence (the layer) | Stamps | Off by more than 0.1 |
|---|---|---|---|
| Interpolate, deeper pipeline | 0.0080 | 0.0159 | 4.7% / 6.4% |
| Interpolate, shallow pipeline | 0.0046 | 0.0089 | 2.7% / 4.4% |
| Interpolate, 3X | 0.0211 | 0.0311 | 3.5% / 9.3% |
| Extrapolate (against the last pair's motion continued) | 0.0126 | 0.0205 | 4.9% / 5.9% |

A second race on the final build, every method switched in turn: the cadence
0.0087, the stamps 0.0165. In Hubris the cadence erred 0.0010 against 0.0124 (0.0016 against 0.0108 on
release 14), at 3X a second race 0.0056 against 0.0187. Extrapolating, against
the next frame's motion in hindsight (its hand-over interval spread over the
display until it is shown), the cadence erred 0.0404 and the stamps 0.0451; a
second race gave 0.0066 against 0.0091 against the last pair's motion and a
tie against the next frame's (0.0335 against 0.0329). Those two references
judge extrapolation on display times and on hand-over times; the game's own
steps are a third, and the right one: the DLSS vectors span the game's step
from the previous frame (`FrameTimeDeltaInMsec`, recorded in
`dlss_evaluation`), and the shown motion is even when the extrapolated frame
sits midway in game time between the current frame and the next. Against
that, scaling by the game's step (1 + period / step) erred 0.0179 where the
fixed share erred 0.0266, the stamps 0.0303 and the hand-over interval
0.0394, and live on that rule 0.0596 against 0.1103 in a race whose steps
swung 12-20 ms; against display times alone the fixed share stays closer
(0.0089 and 0.0050 against 0.0204 and 0.0601), as display times assume even
steps. In Subnautica 2 (steps of 14-24 ms) the rule erred 0.0286, the layer
on it 0.0313, the fixed share 0.0365 and the stamps 0.0516. The layer
extrapolates by the game's step where the game gives DLSS one between half
and twice the frame period.
`synthesis_fraction` records each pair's fraction and stamp gap in the flight
log, and `xrfg_layer_cadence_fraction` checks on the fake runtime, with
repeated stamps and the rate halved part way (`XRFG_TEST_REPEAT_STAMP_EVERY`,
`XRFG_TEST_THROTTLE_AFTER_WAITS`), that every pair is placed at the
midpoint.

#### Each synthetic in its own camera

A synthetic was generated in B's camera - the synthesizer first turns A into
B's camera with the submitted poses, then interpolates the scene's own
motion - and went to the runtime in B's composition layer, with B's pose and
field of view. It is shown a display period before B (two and one at 3X).
SteamVR reprojects every submitted image from the pose it was submitted with
to the head's pose when it is shown, so under a head turn it turned each
synthetic back by the angular velocity times the periods between, and the
trailing edge had no pixels: black strips flickering at the game's frame
rate. At 3X, 144 Hz and 120 degrees a second, 1.7 and 0.8 degrees of view.

Now each synthetic has a camera of its own, C, and is submitted with it.
`synthetic_camera_snapshot` in the layer takes, per view, the orientation
`pose_at_fraction(A, B, f)` - the rotation from A's submitted pose to B's
turned through f of its angle about its axis, which past 1 carries the turn
on - and the position likewise, keeping B's field of view and rectangle. f is
`usable_synthesis_fraction` of the fraction the content is placed at (see
above), so content and pose agree: the midpoint, 1/3 and 2/3 at 3X,
1 + periods / step extrapolating. The same snapshot gives the synthesizer its
target views and replaces the synthetic's views' poses in
`build_generated_frame_end_info`, so the two are bit-identical. A turn over 45
degrees or a move over half a metre between A and B - a recentre, a teleport,
no head in a frame - keeps B's pose for the frame. So does a pair the
synthesizer did not generate in its camera: the ticket's
`synthetics_in_target_camera` is false for a native pair NGX skipped, whose
outputs are copies of B. Camera views are built for every resource of a frame
or for none, so a frame is never half in one camera.

The synthesizer evaluates the existing synthesis at a point of B per output
pixel instead of at the pixel: for output pixel p of C, the point where its
ray meets B's image, q = K_B R_B<-C K_C^-1 p (the pure-rotation homography,
from each view's FOV tangents and rectangle). Every composition already
samples A, B, the flows and the vectors at continuous coordinates, so the
turn adds no resample; a final resample pass after the compose would have
cost a second bilinear filter of the whole frame and a full-screen pass. A
ray that leaves B's view takes A along it, through R_A<-C = R_A<-B R_B<-C -
the far side of the turn, which A saw - and one neither saw takes B's nearest
edge, evaluated as any point of B. The root constants are full (62 and two
tables), so the rotation from C into B rides in the other view's mapping,
which a view's composition never reads (the pack, which reads both, has
dispatched by then; extrapolation's depth rectangle shares it, in another
field), and bit 4 of the flags says it is there; an output whose camera is
B's in every view, within two microradians, draws exactly as before. Every
path takes it: FidelityFX flow, DLSS vectors, the hybrid, NVIDIA flow, the
repeated-capture pair, extrapolation's gather and its mesh warps (the grid
moves in B and is drawn in C; the fill under it goes through the same
mapping), and the 3X second synthetic with its own C. Native DLSS FG's
compose already resamples NGX's frame at a continuous point; four more root
constants carry the rotation, and A sits in the seed's fallback slot, which
the compose did not read.

Extrapolating, C turns on past B, and the strip beyond B's view lies on the
side the head turns towards, which neither frame saw: it takes B's nearest
edge, stretched, where B's pose left the runtime to show black there. A head
that stops short of the prediction leaves C past where it is shown, and the
runtime turns the frame back by the miss - as it does any prediction,
including the game's own for B. The same treatment applies.

Against the scene rendered from C, on a 12-degree turn between A and B
(`test_synthetic_camera_follows_display_pose`, 160x80 per eye), mean absolute
error per channel in 255:

| Path | 2X inside B's view | 3X, 1/3 and 2/3 | Beyond B, from A | Black pixels |
|---|---|---|---|---|
| FidelityFX flow | 0.23-0.24 | 0.25-0.26, 0.22-0.23 | 0.15-0.17 | 0 |
| DLSS vectors | 0.12 | 0.13, 0.13-0.14 | 0.15-0.17 | 0 |
| Hybrid | 0.12-0.13 | 0.13-0.15, 0.13-0.14 | 0.15-0.17 | 0 |
| NVIDIA flow (hardware) | 0.13-0.14 | 0.14, 0.14-0.15 | 0.15-0.17 | 0 |
| Native DLSS FG, 100% / 67% | 0.17 / 0.28 | 0.17-0.18 / 0.28-0.29 | 0.15-0.17 | 0 |
| Extrapolation, vectors, gather / mesh (1.5; 5/3 and 4/3) | 0.16-0.17 / 0.26 | 0.17 / 0.21-0.32 | (unseen: 10-20) | 0 |

Generated in B's camera, the share of each view the runtime would have left
without pixels was 11-12% at 2X and 14-15% and 8% for the two at 3X. The
FidelityFX-flow extrapolation errs 32 on this scene with or without its own
camera (the vectors, 0); it is held to having no black pixel. The call
chain checks on the fake runtime that every synthetic goes down with the pose
its share of the way between the real frames on either side, at 2X and 3X,
inline and on the presenter, and with B's under `synthetic_pose=real`
(`xrfg_layer_synthetic_pose_real_*`).

GPU time a pair at 2004x2004 per eye on an RTX 5090
(`XRFG_TEST_FG_BENCH`, p10), B's camera against the synthetic's own:

| Method | B's camera | Own camera |
|---|---|---|
| FidelityFX, half-resolution flow | 325 us | 338 us |
| DLSS vectors | 231 us | 243 us |
| DLSS vectors + FidelityFX flow | 565 us | 578 us |
| DLSS vectors extrapolation | 207 us | 219 us |
| FidelityFX 3X (two synthetics) | 433 us | 458 us |
| NVIDIA medium, its composition | 136 us | 150 us |
| Native DLSS FG 2X | 1496 us | 1501 us |

`[ofxr] synthetic_pose=interpolated|real` (interpolated unless `real`;
`XRFG_TEST_SYNTHETIC_POSE` overrides) is read at xrCreateSession, at a
control change and twice a second after (`follow_synthetic_pose`): a pair
takes its cameras and the poses it submits them with from one snapshot at
its own xrEndFrame, so nothing has to drain. The tray carries the line through
its rewrites of the runtime ini, as it does `max_file_mb`.

`reprojection_angle` records, while the recorder runs, how far the runtime
will turn each frame of a pair: the angle between the frame's submitted pose
(its views' mean orientation, which cancels a symmetric eye cant) and a VIEW
space of the layer's own located in the frame's projection space at the
display time it is submitted for, just before the hand-over (a, thousandths
of a degree); the head's turn over the display period before that time (b);
and the setting (c). result is presenter_submission's kind. With the head
turning steadily, `real` should read about b for a 2X synthetic and 2b for
the first of a 3X pair, `interpolated` about what the real frames read. The
fake runtime's VIEW space sits at the application's own poses: a real frame
handed over at its display time reads 0, and the first synthetic, submitted
at B's time with the pose halfway from A's, 2.005 degrees - half its pair's
turn (`layer_flight_logging.cmake`).

Inline (no presenter thread), a pair goes down in the application's own
frame - the synthetic, or at 3X the first of the two - and its real frame the
group's other display periods later, in the layer's own cycles
(`submit_current_cycle`). The application rendered for the time its wait
returned, so each real frame was shown that much after the pose it was
rendered for. On the Meta XR Simulator, with `XRFG_TEST_HEAD_SWAY_DEG=20` and
`_PERIOD_S=3` (now also applied to the head `reprojection_angle` measures
against, so a swaying run reads what a perfectly predicting runtime would
turn), hello_xr's real frames read a median of 0.32 degrees, one period of
the turn, frame after frame. `[ofxr] inline_promise=1` (the default;
`XRFG_TEST_INLINE_PROMISE` overrides) measures inline as the presenter does
(`observe_promise_lateness`, with a `PromiseLateness` of its own; a
`promise_correction` record with result 1): each real frame's display time
against the time the application's wait returned, before any shift; while
inline pairs run (set by a pair, cleared by three unpaired frames in a row or
a cooldown), the wait returns that much later and the application's own frame
goes down under the runtime's time for it. A replacement wait at the
application's begin, which puts its frame a period later still, is measured
the same way. On the simulator the correction followed 64 frames in and real
frames then read 0. A synthetic in its own camera is then halfway between
the real frames' poses and times; without the promise, inline, it was placed
a period early. `xrfg_layer_inline_promise` runs 360 paired frames on a fake
runtime that paces its waits a period apart (unpaced, the bunched-pair
detector promotes the presenter): in the second half every real frame goes
down at a promised time and every synthetic a period before one. The other
call-chain scenarios pin the inline schedule as it was and run with the
promise off.

Both promises are measured against the time a real frame's poses were
located for, not the time it is stamped with (`pose_time_of`): the layer
keeps the application's last 64 xrLocateViews results (time, space, first
view's pose) and takes the one whose pose the frame's first projection view
carries, bit for bit, in the projection's space; a pose no recent locate
returned leaves the stamp. UEVR locates a frame's poses on its game thread
for the time its previous frame was promised (its pipeline state's frame
state, moved a period on only when its game thread ran past the frame
wait), and stamps the frame with its own wait's time: in Galactic Racer at
2X every frame went down at its stamp, yet with the test head sway every
frame needed a median 0.65 degrees of turning, and fitting the angles
against the sway's known motion put the poses at -22.2 ms (RMS 53 against
61 at +22.2 and 659 at 0). Measured against the poses, the presenter's
correction settled at 5 periods instead of 3, real frames then needed 0
degrees (median) and synthetics 0.004, and the fit moved to 0 ms. Its cap
rose from 3 to 6. `xrfg_layer_promise_pose_lag` runs the promise call chain
with an application that submits the poses it located for its previous
frame's time (`XRFG_TEST_POSE_LAG=1`; the fake head moves there, since a
still head gives every time the same poses): the correction moves two
periods further and every real frame then goes down 20 ms before its stamp,
at its poses' time.

Two more test hooks move the runtime's own devices where it lists
`XR_EXT_conformance_automation` (the Meta XR Simulator does), so that its
compositor sees them: `XRFG_TEST_RUNTIME_FLIP_S=<period>` holds the right
controller upside down for the first half of each period, and
`XRFG_TEST_RUNTIME_HEAD_SWAY_DEG` / `_PERIOD_S` sways the head (records:
`test_runtime_device`). With the flip, hello_xr on the simulator showed the
status panel 0.29 s after the controller turned over and hid it 0.51 s after
it came upright, every cycle: the gesture's input path, end to end. With the
flip, `XRFG_TEST_RUNTIME_PANEL_ROW=<row>` works the panel's controls once, a
second into the first flip: the right thumbstick pushed down row + 1 times
(the first shows the cursor), then right once, each held 150 ms with 150 ms
between, then the trigger held 600 ms, so the setting on that row is stepped
to its next choice and applied (`test_runtime_device` a=5, the `status_panel` 6
record and the runtime ini say what happened). The
simulator accepted the head moves but did not move its head for any of the
input paths tried, though Meta XR Operator's `openxr_set_head_pose` moves it
on the same simulator; that tool, driven at the display rate below the layer,
is how a runtime head turn was made there.

`app_locate_views` records the application's own xrLocateViews, forwarded
unchanged: the display time it asks poses for (a) and the view configuration
(b). Against app_wait_frame's predicted time and the frames'
presenter_content it shows whether a game renders for the time its frame is
shown. Users of Galactic Racer under UEVR reported "something fighting the
camera" in head turns, and UEVR adds a period to its predicted time when its
game thread runs ahead of the frame wait. On the Meta XR Simulator with native
DLSS FG at 2X, UEVR asked for one returned period past the last wait in 95.6%
of calls and two in 4.4%, and every one of those frames went out with, and was
shown at, the time it was rendered for (shown minus requested 0.00 ms in both
groups): there the period it adds is not a misprediction.

#### DLSS vectors for a swapchain per eye

`resolve_dlss_motion_vectors` matches the game's DLSS evaluations to the image
being captured: both eyes' evaluations for an array image or a double-wide
UEVR one, and for a single image the evaluation whose output has its size. A
game with a swapchain per eye, each the size of its eye's DLSS output,
matched both eyes' evaluations, and the newest went to both images. At a
release-time capture that can be right (the eye just rendered), but the
capture at xrEndFrame (`capture_at_end_frame=1`, the default for native
D3D12 games) comes after both eyes, so one eye always took the other's
vectors. Synthesis then followed the wrong eye's motion and composed the
mismatch as a blend: a double image of the generated frames in OFXR's
vectors mode, and wrong guides for native DLSS FG, while the hybrid, which
falls back to FidelityFX flow per pixel where the vectors do not explain the
frames, looked right - what users of MSFS 2024 and CONTROL Resonant reported.

Now an evaluation whose output is the image itself belongs to it.
Otherwise, for an image the layer has seen submitted as one eye of a
two-view projection (`note_projection_eyes`, flight record
`presenter_transition` 750: a the swapchain, b the eye + 1), the layer notes
at each release of it how many evaluations had been published, and the eye
takes the newest evaluation published by its own image's release: a game
evaluates an eye before it releases that eye's image. Where both eyes'
releases follow the same evaluation (both evaluated before either is
released), two streams go by where their inputs sit and then by which was
seen first, as for array images, and a single stream by the order evaluated,
left first. Streams no longer evaluated (more than four publications behind)
are not an eye's, and a stream both eyes take is read as every other serial
for each eye.

Lies of P under UEVR showed why the release matters. It evaluates one DLSS
feature for each eye in turn, each just before that eye's image is released
(the flight log's `dlss_evaluation` records, a the DLSS handle, b the count
captured): evaluation, left release, evaluation, right release, xrEndFrame.
The first version of this fix took the odd evaluations of a single stream for
the left eye; the capture hook, installed when UEVR loads the layer, can start
counting on either eye, and a launch that began on the right gave the left
eye the right eye's vectors and the right eye none (status temporal
mismatch, used 0). Lies of P also keeps a stream of the eyes' size from
start-up and evaluates another for its spectator view, so a stream counted as
shared only when it was the only one never was. Live on the final version, a
launch in stereo used both eyes' vectors from the first seconds (status used,
no rejections after start-up), with OFXR + DLSS vectors 0.36 ms, FidelityFX
0.87 ms and the hybrid 1.31 ms a pair at 119.6 frames a second, the fixed
share's placement erring 0.0299 against 0.0450 for the stamps. UEVR does not
reach separate eye images on every launch (in some the right eye is a 4x4
image or the left a double-wide one); the vectors then go unused, as they
should. `xrfg_d3d12_history_tests` checks two streams and one, each eye
evaluated before its release and both before either, stale and other-size
streams beside them, and the direct match (and fails without each change),
and `xrfg_layer_projection_eyes` checks that a swapchain per eye is named its
eye and a double-wide one none (`XRFG_TEST_SPLIT_EYE_ONE_LAYER` makes the
call chain's split-eye run submit both views in one projection layer).

#### Vectors written into the other eye's image

A stream both eyes take is not always one feature over two views. Under
UEVR's Synchronized Sequential, and Alternating/AFR without its ghosting fix,
one Unreal view renders both eyes in turn, and Unreal writes a view's vectors
from that view's previous frame - the other eye's. Each eye's vectors then
lead into the other eye's image, a game frame earlier, not into its own
previous image. In the Stellar Blade demo on the Meta XR Simulator (1680x1760
per eye, the left eye's field of view -54 to +40 degrees, the right's its
mirror) the median vector with the camera still was -401.8 pixels: where a
distant point of the left eye's image sits in the right eye's, (tan -54 -
tan -40) / (tan 40 - tan -54) x 1680 = -407. Turning the camera one way and
the other added a part of the motion (-475 to -503, and -303 to -323). The
replay put OFXR + DLSS vectors at 23.7 where the scene moved (a blend: 24.4),
the hybrid at 13.2 and native DLSS FG at 11.8 (67%) and 9.5 (100%), against
NVIDIA's optical flow at 9.1.

The resolver keeps a stream's last three evaluations (`older` beside
`previous` and `latest`), and for an eye of a stream both eyes take it hands
that eye's frame the other eye's evaluation between its own two
(`DlssMotionVectorFrame::bridge`). `D3D12GuideComposer`, recorded in
`submit_pair` once the work slot is reset, gives such a frame vectors of its
own in a texture of the slot's (`guide_composer.hlsl`). Its `Decide` pass
reads both frames' vectors on an 8x8 grid over the central 80% and counts the
points where the eyes' difference `vB.x - vR.x` lies nearer the difference
their projections make for a distant point, from this eye's field of view and
its mirror, than to zero; `Compose` then follows B's vector into the bridge's
image and adds the bridge's vector there, `vB + vR(p + vB)`, where at least
seven points in ten agreed, and copies B's vectors otherwise. A projection
difference under 16 pixels - two eyes with the same field of view - cannot be
told from motion and always copies. The composed frame takes the bridge's
previous jitter, A's, and gives up its bridge; it replaces the frame's guides,
so every method, the frame dump and the next pair (where the frame is A) see
it.

With the composer, the replay of Synchronized Sequential frames turning the
camera put OFXR + DLSS vectors at 10.2, the hybrid at 9.7 and native DLSS FG
at 9.3 (100%) and 10.0 (67%), beside NVIDIA at 9.4 and FidelityFX at 10.2.
Warping each run's older frame by the newer one's composed vectors at scales
0, 0.5, 1, 1.5 and 2 rebuilt the newer frame best at 1 (12.5 and 13.4,
against 17.3-27.0). `test_guide_composer` composes vectors into the other eye
(the projection shift plus half the motion each way) to the motion, copies
vectors into the eye's own image unchanged, and leaves an eye without a bridge
alone; the per-eye test checks each eye's bridge, also where both eyes were
evaluated before either was released. `XRFG_TEST_GUIDES_ONLY=1` runs these
tests alone.

Stellar Blade's demo was then checked turning its camera (the right stick
driven through a UEVR XInput callback) in each way it can be run, recording
every run and replaying it; where the scene moved, generating each middle
frame:

| UEVR | Mode | Native 100% / 67% | OFXR + DLSS vectors | Hybrid | NVIDIA |
|---|---|---|---|---|---|
| nightly 01139 | Native Stereo | 7.11 / 7.31 | 6.91 | 6.76 | 6.84 |
| nightly 01139 | Synchronized Sequential | 9.52 / 10.43 | 9.86 | 9.57 | 9.56 |
| a custom build | Native Stereo | 7.95 / 8.00 | 8.04 | 7.78 | 7.88 |
| a custom build | Native Stereo, its stereo fix on | 9.68 / 10.65 | 7.82 | 7.87 | 7.76 |
| a custom build | Synchronized Sequential | 9.32 / 10.04 | 10.22 | 9.66 | 9.37 |

The game's right eye draws its sky as horizontal streaks in Native Stereo
under both builds - in the real frames, which generation reproduces; neither
`r.VolumetricCloud 0` nor `r.VolumetricRenderTarget 0` changed it, and only
the custom build's stereo fix drew it right. With the fix the game also ran
at the full 45 + 45 a second where it had held 37. Its runs turned the camera
faster (about 90 pixels a frame against 50), and native generation fell
behind there; but native always generates at a half and the replay scores
at the frames' own share (their frame times ran 19-32 ms), and with every
method generating at a half (`XRFG_TEST_REPLAY_FRACTION=0.5`) they all erred
9.2-9.5 against native's 9.7. The replay now also prints each eye's error
where the scene moved (`eye_moving=<left>,<right>`): native's was higher in
both eyes alike there, so neither eye's guides are at fault.

Native generation is aligned by the head's turn alone; a game camera's own
turn, from a mouse or a stick, reaches NGX only through the vectors.
`XRFG_TEST_REPLAY_NATIVE_GAME_ROTATION=1` fits the whole turn between the two
frames to B's vectors (Gauss-Newton on a rotation taking B's view rays to the
rays of where the vectors put their content in A, over a 48x48 grid, keeping
the closest three quarters of the points each pass), aligns A by it and turns
the generated frame back by half of it. On the fast runs above (5.6-5.7
degrees a pair) native erred 9.31 against 9.68 at 100% and 10.18 against
10.65 at 67%. Live it would need the fit on the GPU and the rotation on the
CPU for the compose and the reseed decision, a frame later; for 4% it is not
built.

On the Steam Frame the character itself ghosted as the camera turned about
her (Native Stereo, 3474x3474 an eye, about 37 game frames a second). The
vectors were right - within 1.7 pixels of measured image flow, near zero on
her and 35-54 pixels a frame on the scene behind - and NGX placed both at
half, but in its frames her suit was smeared sideways and half see-through
along the side the background slid past. NGX's object separation did it: at
NVIDIA's 40 (linearised depth, near-plane units: her 21 against the scene's
79-320) NGX cut her from the background, and told the camera was still - the
engine's camera is not known here, only the head's - it carried the
background's motion across her. At 1000 it separates nothing in view and the
smear is gone. On six triplets of that turn native at 67% erred 9.43 against
13.78 at 40 (400: 12.36; 2000 and 4000 as 1000; blending 15.00), and on the
42 Galactic Racer triplets 8.08 against 7.96, better on 13. 1000 is now the
default (`XRFG_TEST_NATIVE_DLSSG_SEPARATION` overrides it).

Handing NGX the camera's motion was tried instead, since its clip-to-previous
matrices are identity here while the vectors carry the game camera's orbit.
`XRFG_TEST_REPLAY_NATIVE_CAMERA_FIT=1` fits a rigid motion (rotation and
translation) to B's vectors and depth - Gauss-Newton on the reprojection
error over a 64x64 grid, keeping the closest 70% of the points after the
first passes - and hands it to NGX per eye
(`D3D12NativeDlssG::set_test_camera_motion`, with
`XRFG_NATIVE_DLSSG_PER_EYE=1`). On the four Steam Frame triplets it fitted
2.4-2.7 degrees a pair to a median 0.6 pixels, and NGX's frames did not
change: 13.33 with it and without at separation 40, 8.19 against 8.20 at
1000. NGX takes the motion from the vectors; the separation decides how it
treats her edge. Feature per eye or both eyes in one measured alike too.

Scored on the character alone (her pixels within 3 m, inside her box, and
a band about her outline) on simulator triplets turning twice as fast (her
52 pixels a frame, the scene 70-100), at 67%: separation 40 erred 13.0
inside her and 12.4 on her outline, 1000 11.8 and 11.1; at 100%, 10.7 and
10.0.

In Stellar Blade the detail restore of a reduced feature costs more than
it gives. On the four Steam Frame triplets at separation 1000: 67% with the
restore 8.22, with falloff 4 / 8 / 16 8.06 / 7.86 / 7.65, without it 7.38,
and 100% 6.93; on the fast simulator ones 8.95, 8.64 / 8.50 / 8.39, 8.27 and
8.44. Restoring only where the motion field does not break (a second
difference over 16 pixels under a pixel) measured 8.13 and 8.68, so it is
not kept. The game's own DLSS-G (310.1.0, which UEVR's injection leaves NGX
loading from its Streamline folder) measured the same as 310.9.1 on all of
these to the hundredth. 67% stays the default, which Galactic Racer's
triplets favour; for Stellar Blade choose 100% in the tray, which cost no
frame rate on the simulator (45 a second either way at 3004x3004 an eye).
NGX's near/far partition of 50 or 150 moved separation 40 to 12.41 or 13.20
and changed nothing at 1000; a seam of 128 changed nothing.

The matrices alone are ignored, but NGX does take the camera's motion when
the vectors are made to leave it out, which is its contract: camera motion
not included, the clip-to-previous matrices describing it, and vectors that
carry what moves on its own. `XRFG_TEST_REPLAY_NATIVE_OBJECT_MOTION=1` (with
`XRFG_TEST_REPLAY_NATIVE_CAMERA_FIT=1` and `XRFG_NATIVE_DLSSG_PER_EYE=1`)
hands the pack the fitted motion as well (`set_test_camera_motion`'s
`rigid`): for each guide texel it unprojects the depth into B's camera,
moves it by the fit, projects it again and takes that from the vector, and
the evaluation says the vectors hold object motion alone. The fit applies to
the generating pair only; the seeding pair, with one frame on both sides,
keeps none. Error where the scene moved, native at 67% and at 100%:

| Set | Base | Object motion |
|---|---|---|
| Steam Frame turns (six triplets, 2.5 degrees a pair, fit 0.6 px) | 9.43 / 8.28 | 9.17 / 8.26 |
| Simulator fast turns (six triplets, 4.4-4.8 degrees a pair, fit 12-15 px) | 9.05 / 8.91 | 7.53 / 7.53 |

SSIM rose on every fast triplet (0.66 to 0.70 at 67%). At NVIDIA's
separation of 40 with object motion the fast set measured 7.68 and the Steam
Frame set 9.27: once NGX knows the camera moved, its own separation no longer
smears her, though 1000 still measures better. The matrices alone
(`CAMERA_FIT` without `OBJECT_MOTION`) measured as the base, 9.41, and a
feature per eye as the shared one. Aligning A by the whole fitted turn
instead (`XRFG_TEST_REPLAY_NATIVE_GAME_ROTATION=1`) at separation 1000
measured 9.37 on the Steam Frame set, with higher SSIM on the slow triplets
(0.77 against 0.71 on one). A capture taken live on the OpenXR Simulator
(3004x3004 an eye, 44 game frames a second, the camera still) replayed the
same with and without, 11.39 at 67% and 9.96 at 100%, and its live generated
frames sat midway: each as far from the frame before as from the frame
after, 11 against 21 between the two.

Live, that is `[ofxr] dlssg_camera=1` (the tray's **DLSS Frame Generation
camera motion**, `XRFG_NATIVE_DLSSG_CAMERA` overriding it), off by default.
It needs a feature per eye, since one evaluation has one clip space, and
the camera's motion comes from the GPU a pair late: after each eye's pack a
compute shader (`FitNativeDlssGCamera`, one group of 256 threads an eye)
fits a rotation and translation to the vectors and depth just packed, over
a 32x32 grid of the eye, by ten Gauss-Newton sweeps on the reprojection
error with numeric Jacobians, keeping from the third sweep the closer seven
tenths of the points by the sweep before's residuals (a histogram in group
memory, the cut read off inside its bin). The result is copied to a
readback buffer the pair's completion fence guards; `poll_fits` reads it
before the next record, and a fit with a quarter of the points as inliers, a
median residual under 48 eye pixels and a turn under a radian is kept for
the eye's stream. A pair whose A is that fit's B, or at most two pairs on,
packs its vectors less the camera's flow (`depth_to_z` linearises the
depth, the pack unprojects, moves and projects each guide texel) and
evaluates with the matrices from the same motion and camera motion not
included. Fits that fail or lag leave the pair as before. Replayed, with
the generating pair run twice so the second has the first's fit
(`XRFG_TEST_REPLAY_NATIVE_GPU_FIT=1`), error where the scene moved:

| Set | Base 67% / 100% | GPU fit 67% / 100% | CPU fit, for reference |
|---|---|---|---|
| Steam Frame turns | 9.43 / 8.28 | 9.00 / 8.04 | 9.17 / 8.26 |
| Simulator fast turns | 9.05 / 8.91 | 8.00 / 8.02 | 7.53 / 7.53 |

SSIM rose on every triplet. The GPU fit reads the packed, aligned vectors,
so a head turn is already out of what it fits. A single thread group runs
alone on the GPU, so the fit's size is its cost: a 64x64 grid with sixteen
sweeps took 0.6 ms a pair at 3004x3004 and measured the same as this one.
The FG bench at 3004x3004, 2X, median: the shared feature 2.03 ms still and
2.39 turning, a feature per eye 2.38 and 2.71, with the fit 2.52 and 2.85.
Live in Stellar Blade on the OpenXR Simulator (3004x3004, 100%), with the
first kernel, the camera path used 4,735 fits and dropped 697 - a loading
screen's, with 360 inliers and translations of thousands - at 3.51 ms a
pair against 2.32 for the shared feature, and the game went from 43.9 to
41.6 frames a second.

The fit is a pair late, and the game's frames do not keep their time. In a
release-24 orbit on the OpenXR Simulator one pair spanned a 50 ms game hitch
between 26 ms pairs: the scene moved 241 pixels where its neighbours moved
107 to 115, the pair got the pair before's fit, and the generated frame
doubled a background sign in both eyes, its scene about 25 pixels along
where 120 belonged. `fitted_motion` now takes the current pair's guide
frame time against the one recorded with the fit (`CameraMotion::dt_ms`):
within a ratio of 0.75 to 1.33 the fit's rotation angle and translation are
scaled by the ratio, beyond it the pair goes without the camera's motion
(`kFitScaleMin`, `kFitScaleMax`; the decision log writes the ratio when it
leaves 0.9 to 1.1). Replayed with the fit taken from the pair before as live
(`XRFG_TEST_REPLAY_NATIVE_FIT_PREVIOUS=1`, which seeds with frame -1 to
frame 0 and reads that fit back; the generating pair spans two frame times,
so its ratio is about two; `XRFG_TEST_NATIVE_FIT_SCALE=0` uses the fit
unscaled), error where the scene moved, 100%:

| Triplet | No camera | Own fit | Pair before's fit, unscaled | Scaled |
|---|---|---|---|---|
| r24_turn f1 (26 + 50 ms) | 9.51 | 9.75 | 9.69 | 9.79 |
| r24_turn f2 (50 + 26 ms) | 9.77 | 9.97 | 9.81 | 10.06 |
| user_rot5 seq0 f1 (26 + 50 ms) | 14.27 | 14.48 | 13.14 | 14.35 |
| user_rot5 seq1 f1 (26 + 25 ms) | 8.30 | 7.13 | 9.18 | 8.05 |
| user_rot5 seq3 f1 (26 + 26 ms) | 6.23 | 6.49 | 7.90 | 6.68 |
| Simulator fast turns, 67%, mean of 4 | 9.10 | 8.00 | 7.88 | 7.94 |

On the evenly paced pairs the scaled fit lands between the unscaled one and
the pair's own; on the hitch pairs no fit beats none, and the replay cannot
say more there, since native generation's frame sits at a half of the pair
while the truth sits at a third or two thirds of an uneven one. Hence the
rule: scale small differences, drop large ones. Live, a hitch pair and the
pair after it (whose fit is the hitch's, at half the ratio) both go without
the camera's motion, which is release 23's behaviour. What that looks like
(a release-27 2X capture at the user's setting, a stall of the capture's own
making, 27 then 50 ms frames, the background 220 to 230 pixels across the
pair): the generated frame of the hitch pair ghosts, the character's face
and hair half-transparent with the car behind her showing through in one
eye and a faint doubled shop sign in the other, while the pairs around it
are clean, their background at the half and the character crisp. A 220
pixel step in one pair is beyond what NGX interpolates cleanly with or
without a camera, so the dropped fit stays the conservative choice; the
alternatives, repeating the real frame on such a pair (a stutter instead of
a ghost) or scaling the fit, want a live sample before either is taken.

The replay pointed at the library first, and the live test took that back.
The exact live hitch pair (A frame 2, B frame 3, the right eye's head crop)
replayed through each DLSS-G library in turn: with the game's own 310.1.0
the dropped fit reproduces the live frame (1.2 to 1.7 mean difference on
the crop; every other variant 2.6 to 6.2 away), and so does the camera off;
with 310.9.1 the same pair with the fit dropped keeps her face whole. The
ideal same-pair fit (7.20 degrees against 3.89 for the pair before, 1.85
times, exactly the frame-time ratio) ghosts with either library, so scaling
would be worse and the dropped fit stays. The two libraries score the same
on 24 recorded turn triplets (12.17 and 12.17, 10.59 and 10.60). The layer
cannot choose the library for the game - Stellar Blade initialises NGX
before the layer loads (`ngx_core_already_loaded`, no `ngx_discovery_paths`
in its capture log), so the game's own path list decides, and the module
its process held was
`SB\Plugins\Runtime\Nvidia\Streamline\Binaries\ThirdParty\Win64\nvngx_dlssg.dll`,
310.1.0 as shipped - so 310.9.1 was copied over it for a live run (the
demo's folder is back as shipped since; the copies are under
`build/captures/sb/dlssg_backup`). Live at 2X with 100% and the camera, four
full-stick orbits and 25 pairs were clean at 39 to 40 frames a second, but
none had a 50 ms hitch; a 12-frame capture with three 25 ms suspensions of
the process injected (`build/captures/sb/r27_2x_hitchlive`) gave five 50 ms
pairs, and two of them - the two where the car passes behind her head -
ghost exactly as the original did, her face translucent and in one a
doubled outline, while the three with her facing the camera are clean. The
library is not the fix. The replay misled because with `FIT_PREVIOUS` the
seeding pair is evaluated without a fit, so NGX's history in the replay
never held the camera split; live, the pairs before a hitch are split
(object-only vectors, camera matrices) and the hitch pair switches to full
vectors with an identity camera. The hypothesis under test is that this
switch on a dropped pair is what NGX mishandles, and the live hitch run
with the camera off throughout (`XRFG_NATIVE_DLSSG_CAMERA=0` through the
launcher's `-GameEnv`, same library, same injected stalls,
`build/captures/sb/hitchlive_cam0`) bore it out: four hitch pairs, her face
opaque in every one, the one with the phone booth and the car right behind
her head included; what remains is the car's smear and doubling around her
silhouette, disocclusion at a doubled step, with nothing showing through
her. The same with the game's own 310.1.0 and the camera off from the ini,
the user's configuration now (`build/captures/sb/hitchlive_cam0_orig`): five
hitch pairs from 44.6 to 50 ms, one of them a 45.5 ms frame after a 16.7
(ratio 2.7), and her head, hair and body solid on every one, the extreme
pair with a faint copy of her hair ornament beside her head and a
background sign doubling a little on another. So the library does not
matter at all, and the camera option is what ghosts on a hitch pair.
What the dropped-fit pair did differently was flip NGX's reading of the
vectors - the pairs before it read "objects' own" with the camera's
matrices, the hitch pair "everything included" with the identity - so the
first attempt kept the flag steady, telling NGX "objects' own" with
identity matrices on every pair without a fit. Live under the same recipe
(`build/captures/sb/hitchlive_cam1_fix`, four 50 ms pairs) it did not help:
her hair partly see-through on one pair, her ponytail gone against the car
on another, and on a 50 ms pair after a 21.7 ms one her upper body heavily
translucent with the rocks behind showing through, the worst seen. The
change is taken back. The camera option changes two things at once - the
split of the vectors, and a feature per eye in place of the shared-stereo
one - and which is at fault on a hitch pair is the open question; the cuts
are per-eye features without the camera (`XRFG_NATIVE_DLSSG_CAMERA=0
XRFG_NATIVE_DLSSG_PER_EYE=1`), the previous fit used unscaled and never
dropped (`XRFG_TEST_NATIVE_FIT_SCALE=0`), and the fit scaled by the
frame-time ratio and never dropped (`XRFG_TEST_NATIVE_FIT_DROP=0`, a knob
added for this), each under the same injected stalls.

The cuts ran (`hitch_a_cam0_pereye`, `hitch_c_cam1_nodrop`), and then two
wide runs of the user's own configuration, the shared feature with the
camera off, eight sequences of six frames over fourteen seconds of orbit
with each library (`hitch_user_shared_orig_wide`,
`hitch_user_shared_new_wide`). Per-eye features ghost most hitch pairs with
either library and with or without the split; the shared feature ghosted
1 of 4 such pairs on 310.1.0 (a three-quarter view with the phone booth
and car behind her head) and 2 to 3 of 4 on 310.9.1 (side-on, her face
and head translucent with the stairs and road through them), after a first
shared 310.9.1 run with 0 of 4. So it is NGX at a doubled step, whatever
the library or the feature layout; the shared feature is better than one
per eye, which is the camera option's warning, and the library advice does
not hold. What a busy near occluder does on such a pair (a phone booth
sweeping past the camera on a 45 ms pair, half-transparent over her) is the
same step seen from the other side. The only frame that does not show the
ghost is the real one, so native generation now skips a pair whose frame
time runs over 1.55 times the median of the eight before it
(`Skip::hitch`; the synthesizer shows the current frame as it does for a
guide skip, and the status is left alone) - one repeated frame where the
game hitched anyway. The ratio is against a median, so a game that settles
at a lower rate generates again after four pairs, and a pair that misses
one display slot at 2X (1.5 times) is kept. `XRFG_NATIVE_DLSSG_HITCH_SKIP=0`
turns it off; the decision log writes `hitch ... skipped` for each. The
same wide capture with the rule in confirmed it (`hitch_skip_user_wide`,
the user's configuration, 117 suspensions over eight sequences): 53
`hitch ... skipped` lines, all 42.8 to 50 ms against medians of 22 to 30;
every captured hitch pair shows exactly the real frame (generated against
current 0.00, against 14 to 16 between the real frames) and every other
pair is generated as before, the generated frame halfway between its
neighbours - a 37.1 ms pair at 1.4 times included, and the 16.7 ms pairs
right after a hitch, solid.

OFXR's own methods want the same rule, and more. The hybrid (FidelityFX's
flow with the game's vectors, the deeper pipeline, 2X) under the same wide
forced-hitch recipe (`hitch_ofxr_hybrid`): on two 50 ms pairs (the booth
and car behind her, and side-on) two half-transparent copies of her side
by side, at A's and B's places, body included - a full double image
rather than a translucent face - and the booth sweeping past the camera on
a third is a mess. So the rule now sits in the frame synthesizer's
`record_pair`, ahead of every method: the pair's frame time is the time
between the two frames' arrival in the synthesizer (`RollingSource::
captured_at`, set when the source is acquired, so the flow-only methods
are covered without a DLSS guide), against the median of the eight pairs
before, and over 1.55 times it and at least six milliseconds over it (under
half a display period no doubled step can show, and a two-hundred-frame
menu jitters by more than its ratio) the pair's synthetic slots are filled
with B and B goes to its current destination as usual (`record_repeat_pair`,
the native path's skipped-pair ending factored out); the flight log
writes `synthesis_pair` 777 with the pair's time, the median and the
serial. `XRFG_HITCH_SKIP=0` turns it off; the native path's own check on
DLSS's frame time stays behind it as a second line.

The first build keyed on arrival alone and the wide recipe on the hybrid
(`hitch_ofxr_skip`, 84 records) showed it one frame early: the 50 ms pair
was skipped and shown as B, but two normal pairs before stalls were skipped
too, and two 42 to 44 ms pairs after them were generated with her double
image. An engine samples its delta time at the start of a frame, so a
stall during frame N shows in N+1's step, while N is the frame that
arrives late; the DLSS frame time the native check reads is the step the
game measured, which is why it lands on the right pair (on the user's
configuration the same run had every hitch pair shown as B through that
second check, plus a normal pair skipped before each by the first). Now
the layer's rule takes B's frame time wherever a guide carries it
(`frame_time_known`), and without a guide takes the arrival gap and marks
the pair after a late frame (`skip_next_pair`; 778 in the log against 777
for a step from the game's own time). The same recipe on that build
confirmed it on both. OFXR's hybrid (`hitch_ofxr_shift`): 49 records of
777 and 3 of 778; every hitch pair (50.0, 47.0 and 50.0 ms) shown as B, the
pairs before each stall generated normally, and two pairs under the cut
(38.2 ms at about 1.53 times, and 34.4) generated with her body solid and
only a faint edge ghost of the car behind her, no double image. The user's
configuration, native DLSS FG (`hitch_dlss_shift`): 50 of 777 and 5 of
778; both 50 ms pairs shown as B, every other pair generated, the ones
right before stalls included, so the extra held frame of the first build
is gone.

Bunched arrivals. On a game without a DLSS frame time the rule has only
the arrival, and the first game of the Unreal batch to run on it in
earnest showed the arrival's weakness: Hellblade II under UEVR on the
Meta XR Simulator, 44 real and 44 generated a second on the presenter,
took 78 skips in 60 seconds (release 31's layer), and of the 46 after
its loading 39 were a long gap followed by a short one - 32 ms then 12,
with a 22 ms median - frames that had arrived in a bunch and stepped
evenly in content, each skip a repeated frame where nothing had hitched
(the shown-rate line counted them: 1.0-1.1 "other" frames a second). A
stall is different in just this respect: the pair after the late frame
keeps its normal gap, since the engine resumed at its own pace. So the
pair after a late arrival is now skipped only when its own gap is not
short, under three quarters of the median, and the flight log writes
`synthesis_pair` 779 (step, median, serial) for the pairs let through.
The same run on this layer, 98 seconds: 29 skips (778) and 189 pairs let
through (779); after loading, 19 and 144, the repeats down to 0.3 a
second. The remaining skips are the game's own stalls: the gaps between
its end-frame calls, 22.2 ms like clockwork at 45 frames a second, open
to 44, 47, 55 and 78 ms before them, the next gap 18 to 20, so the two
steps together run 12 ms over two periods at the median and up to 56
over; the pairs let through sit on a 28 to 50 ms gap followed by one of
12 to 15, two periods in all. A handful sit on the line (a 28 ms gap and
then 22, a 29 and then 26), where the arrival alone cannot say. Games
whose guides carry a frame time (777) are untouched; the forced-hitch
recipe above runs on those.

The feature's size, and the camera's motion at 67%. Per-eye features at
67% with the camera off were clean on six of six forced hitch pairs
(`hitch_pereye67_noskip`, `hitch_pereye67_b`) where 100% ghosted most, so
the per-eye weakness on a doubled step is the feature's size: the step
fits NGX's flow range in the smaller feature. But the camera's motion at
67% (`hitch_cam1_67`, both skips on) ghosts ordinary turns: her hair and
the back of her head translucent with the car through them side-on, her
face and arm faded three-quarter back against bushes, her back faded from
behind with the road through it, while front views and a dark background
stay solid; the hitch pairs show B as they should. The camera at 100% was
clean on normal turns before the hitch work, and the pack's subtraction,
the GPU fit and the matrices are all in UV or the eye's pixels, the same
at any scale. One candidate was tried and taken back: taking the nearest
surface's depth among a texel's four neighbours for the camera term and
the packed depth at a reduced scale, since the colour there is a bilinear
average and the depth was the centre texel's, so a silhouette's camera
flow could be the background's. Live (`hitch_cam1_67_fix`) the front views
were clean and the rest still ghosted. So the camera's motion is for 100%
only until the cause is found; the replay cannot see it (whole-frame
numbers put the camera's penalty at +0.5 at 100% and +0.4 at 67% on the
same hitch triplets, and the rot1 turn triplets carry no camera motion at
all), so the live recipe with crops is the instrument.

The fit was measuring what was left over. The decision logs of those two
67% runs (`XRFG_TEST_NATIVE_DLSSG_DECISIONS=1`) put it in plain sight:
in the second (`hitch_cam1_67_fix`) the fit of one eye went 3.8 degrees,
0.02, 3.8, 3.8, 0.04, 0.03, 3.8, 3.8 pair by pair, the turn and then
nothing, and in the first (`hitch_cam1_67`) it flipped between 7.5 and
3.9 degrees with residuals of 7 to 13 pixels where a still scene fits to
0.07. The cause is in the order of the pack and the fit: the pack writes
a pair's vectors less the camera flow of the pair before's fit (for NGX,
with the matrices carrying that flow), and `FitNativeDlssGCamera` then
read `Motion[g]` as written, so it fitted the motion left after the
subtraction, and `poll_fits` handed that on as the camera's motion
entire. A steady turn therefore alternated: a pair with no fit packed
whole vectors and fitted the whole turn; the next subtracted it and
fitted about nothing; the next subtracted nothing and fitted the turn
again. On the pairs with the near-zero fit NGX got whole vectors with
`cameraMotionIncluded` false and near-identity matrices, that is the
camera-off reading of the scene, and in the first run, where the
subtraction did not cancel (the 7.5 degree fits), the vectors handed on
were wrong outright. `fit_point` now adds the same camera flow back, from
`Depth[g]` and the pack's constants, before the point is fitted, so the
fit measures the whole motion whatever the pack took out. The replay
harness polls the generating pair's own fit after the record
(`poll_camera_fits`; "replay native gpu fit handed" against "own"): on
`hitch_cam1_67` seq1-7 the pair's own fit now equals the fit it was
handed to three decimals (6.545 against 6.547 degrees, the translation
and residual the same), at 67% and at 100% alike. The whole-frame error
of the replay does not move, since each replayed pair takes the handed
fit either way; what changes is every pair after the first in a live
run. Live, with the fix (`hitch_cam1_67_fitfix`, `hitch_cam1_100_fitfix`,
the same recipe): the steady turn fits to 3.787, 3.786, 3.788 degrees
pair after pair, residuals 0.09 to 0.12 px at 67% and 0.19 at 100% over
a thousand turning fits each, no alternation. The 67% verdict stands
regardless: hitch pairs show B, and the normal pairs still ghost (a side
view with stairs behind her heavily translucent, body and head; a
three-quarter back view with head and back translucent; hair or face
faded in three more), front views clean. At 100% the normal pairs are
mostly solid, with a faint, doubled ponytail and a hair edge in two back
views. So the fit was one defect, and whatever remains is particular to
the camera split at the reduced feature. One fact for the next attempt:
with a DLSS-Quality game the guide grid at 100% is coarse (two thirds of
the feature) and at 67% the feature's own size, so the two scales pack
the same 2469-wide motion and depth and differ only in the colour
feature (3703 against 2469); the difference is in how NGX separates
camera from object at the smaller feature, not in the guides handed to
it.

A note for the test rig, not the layer: Deep Rock Galactic under UEVR on
the Meta XR Simulator died twice in xrCreateInstance (0xc0000409, the
dumps in `%LOCALAPPDATA%\CrashDumps`: `std::terminate` inside the
simulator's SwiftletInputPlugin, reached through the layer's downstream
call). The game's own OpenXR plugin creates an instance about twenty
seconds before UEVR's; when the launcher's per-process runtime override is
already in place by then it succeeds on the simulator and stays alive, and
UEVR's second instance is one more than the simulator's plugin takes. When
the engine's attempt comes first it fails fast (-2, no runtime) and the
session is fine, which is how every other launch today went. Launch UEVR
games with `-nohmd` (`-GameArgs '-dx12 -nohmd'`): the engine opens no
instance, UEVR's is the only one, and native DLSS FG on that path ran 44.9
+ 44.9 a second on the presenter with no quarantine. The six native games
of the regression round were then run with native DLSS FG selected
(release 30's layer, 100%, the camera off): Hubris, Kayak VR, Moss Book II,
Moss, RoboQuest VR and FlatOut 4 VR all generated natively on the
presenter at 45 real and 45 generated a second, no quarantine, no error,
FlatOut's D3D11 bridge included. A second batch of native VR titles went the
same way: Alien: Rogue Incursion VR, Into the Radius VR, The 7th Guest VR
and Reach at 45 + 45 on the presenter (the first and the last two through
the pipelined-wait promotion, Into the Radius through the Meta rule), Metro
Awakening at 45 real frames and 45 pairs a second inline - a split frame
loop, which stays inline on Meta's runtimes by design - and none with a
quarantine. Into the Radius 2 created its session and then submitted no
layers at all for the three minutes it was watched (a loading or launcher
state the unattended run never left), so it was not measured.

In play, not in menus. Every unattended batch above measured what a game
shows a minute after launch, which is its title screen or main menu;
Deep Rock Galactic was the exception by accident, since it loads straight
into a 3D scene. Menus validate the arming, the presenter against inline,
the pacing and the quarantines, and nothing about generation on real
vectors and depth, disocclusion, moving UI, in-play frame-time variance
or the camera fit on real turns. From 2026-10-11 the runs reach play
first. The tools (all under `build\GalacticRacer-native-DLSSG-test-...`,
grown from the Stellar Blade recipe): keys posted to the game's
`UnrealWindow` (`sb_gameplay/keys.py`), a UEVR plugin that fills XInput
pad 0 from a text file for walking and turning
(`stick_driver6.dll`, then `padconnect.py` so Unreal polls the pad), a real
mouse click through `SetCursorPos` and `SendInput` where a menu button
takes nothing else (Hellblade II's first-launch pages, RoboCop's menus,
whose cursor the game scales by 2.25 against its window), the layer's
diagnostics (`query_ofxr.py`: method, guides used) and a screenshot of the
simulator's eye view against the game's flat window. What the first round
found, on the release 32 layer with the user's settings (native DLSS Frame
Generation, 100%, camera off, 2X, hybrid), Meta XR Simulator at 90 Hz:

| Game | Reached | Shown a second | Hitch records (777 guided / 778 arrival skip / 779 let through) |
|---|---|---|---|
| Deep Rock Galactic | a mission: the drop pod, then a cave, walking and turning for 96 + 70 s (pitch dark, so no visual check) | 45.0 + 45.0, no repeats | 0 / 3 / 9, then 0 / 1 / 4 |
| Stellar Blade Demo (OpenXR Simulator, the forced-hitch recipe) | the rain-city street, third person, the camera orbiting, 117 injected 25 ms stalls | 26.6 + 26.6 and 36.8 repeats (the game holds 26.5 there) | 61 / 2 / 2; every pair over 1.55 times the median (48.6, 50.0, 50.0, 44.7, 39.7 ms) shown as B, the steady 24 ms pairs solid, a 41.0 ms pair that passed under a storm-raised median generated with a faint double edge on her hair |
| Hellblade II | the opening in-engine cinematic (UEVR shows it letterboxed); a second run that waited eleven minutes for the playable section found the game's output gone white about eight minutes in, UEVR warning every frame that the previous one was not rendered (its stereo hook, not the layer), the eye view frozen on a half-written frame while the game kept submitting 31 identical frames a second | 44.9 + 44.8 in the cinematic; 31 + 31 with 14 repeats in the white state | 0 / 49 / 197 in 107 s of cinematic: the 49 follow 28-45 ms frames with 28-35 ms after them, the game's own slow frames, not bunching; 297 / 0 in 100 s of the white state, the submission jitter of a game rendering nothing |
| Clair Obscur: Expedition 33 | not reached: hung on the engine splash for 110 s and exited (UEVR's known incompatibility); the earlier 45 + 45 was its title scene | | |
| RoboCop: Rogue City | first the opening in-engine cinematic (the news studio) after Play, New Main Campaign, Normal, which the game caps at 30 frames a second with a 52 to 63 ms frame every second or so; then, nine minutes in, play on the street, walking and turning for 100 s | cinematic 33.5 + 33.4 (67.6 shown, the game under half the refresh); play 43.9 + 44.0, no repeats | cinematic 0 / 96 / 0 in 103 s, each skip after one of those doubled frames with the next frame normal, so the rule reads them as the stalls they are; play 0 / 17 / 0 in 100 s |

No quarantine and no error in any of them; the prepare reason stayed 0
throughout play. The arrival rule's remaining skips in Hellblade II's
cinematic are the game's own 28 to 45 ms frames (a shot cut, streaming),
with the next frame at 28 to 35 ms. Whether a 1.3 to 1.5 times step is
better generated or shown as B was then measured with the same Stellar
Blade recipe and the suspend shortened to 12 ms
(`sb_gameplay/hitch_run_stall.sh`, `STALL_S=0.012`, capture
`hitch_r32_stall12`): pairs of 30.5, 32.0, 34.0 and 33.8 ms against a 24
ms median, all generated, her body and face solid in every one and only
a faint doubled edge along the ponytail on the two longest, against the
single 50 ms pair of the run shown as B. So the cut stays at 1.55: below
it a generated frame costs a hair's edge, a repeated one a visible stutter.

The three methods on the same play. The recipe was then run with the
tray on each method in turn (captures `hitch_r32_user`, `hitch_r32_ofxr`,
`hitch_r32_extrap`; the same street, the same orbit, the same stalls), and
the head crops of the steady 22 to 27 ms pairs tell them apart at once.
Native DLSS Frame Generation: clean, her face and hair sharp against the
moving background. OFXR's hybrid: a mild double along her hair and the
sword's hilt on a steady pair, and on a 36 ms pair under the cut (1.4
times) a strong second copy of her head and torso beside the first, the
same double image the forced hitches showed at 2x. Extrapolation: her
face and hair smeared and doubled on the side views, the background
stretched where she uncovers it, the front view mostly fine; its hitch
pairs show B as the others do. All three held the presenter with no
quarantine (the game runs 26 to 37 frames a second on that simulator, so
all three repeat frames). For a close third-person character under a
turning camera, then, native generation is the method; the hybrid's
double on ordinary pairs is the next thing to look at in OFXR's own path,
and extrapolation is for latency, not for this.

#### The deeper pipeline: its phase, and an automatic depth

With the deeper pipeline the measured latency was not one number but two:
62 ms in some stretches and 70 ms in others, at the same 119.7 frames a
second. A whole race's flight log (Galactic Racer at 2316x2316 per eye)
showed why. A pair's frames reach the presenter at the same point either
way, but the game's slack - about 6.5 ms a pair - can be taken by either of
two waits in xrEndFrame: the capacity wait at admission, before synthesis,
or the once-per-pair hold after the hand-over. Both arrangements are stable,
since the hold releases the game at a presenter frame and the game's own work
then takes the same time. Where admission takes the slack, the finished frame
waits out a whole period before synthesis starts: the game rendered for a
pose a period older. The game fell into that arrangement after a few seconds
of SteamVR halving the rate and stayed in it, 57% of that race's pairs.

`observe_admission_wait` watches the admission wait: when eight pairs in a
row wait more than half a display period there, the next pair hold runs one
presenter frame longer, once, which moves the game's next frame a period
later and the slack to after the hand-over. A move that leaves the waits
where they were is retried only after a run twice as long (up to 64 times
eight), and nothing is counted while SteamVR runs at half the rate. At the
true rate, isolated late pairs are common and runs of up to about a dozen end
by themselves, while a slip that stays runs on until moved. Judged first over
fixed windows of 32 pairs (nine in ten late), slips ran 20-59 pairs and the
slow share was 1.2-4.3% of a race's full-rate pairs; with a run of twelve,
1.3% with the longest run 13; with eight, 1.0% with the longest 9. The
length was chosen on the natural runs of a race with no moves at all (24
slips; runs that ended by themselves up to 14 long), counting a move made on
a run that would have ended as costing that run again to move back: over
three races 6, 8, 10 and 12 came to about 354, 392, 456 and 503 slow pairs,
with 13, 8, 5 and 2 needless moves. What is left is below what can be measured: across 37
clean full-rate rounds in four races, a real frame went down 62.47 ms after
the game's wait with a standard deviation of 0.17 ms, while the 1.0% left
slow adds 0.083 ms to that average, about half of it single late pairs that
are scheduling noise rather than a slip. Six instead of eight would take
off a further 0.013 ms, a thirteenth of the noise, for five more needless
moves, each of which shifts when the game samples its pose by a period. Every round at the true rate measured 62 ms,
and no frame was lost to a move.
Moving the promise with the phase was tried and removed: the measurement only
moved it back. `xrfg_layer_rephase` checks the move and the back-off with
`XRFG_TEST_ADMISSION_WAIT_MS`, which adds to every measured admission wait;
`xrfg_layer_promise_shown_time` checks that a steady session is never moved.

While SteamVR runs at half the rate the promise measurement also stops
(`observe_promise_lateness` gets the runtime's period): frames then go down
two periods apart, and the measurement had moved the promise between one
period and three every 2.4 s for as long as that lasted.

An automatic depth was built and measured and then removed. It started deep,
tried the shallow pipeline every so often, kept it while it repeated no more
than half a percent of periods more than the deep one, and went back deep at
one percent more, counting the display periods SteamVR's wait stepped over
as repeats (the shallow pipeline loses frames that way, 80-102 shown a second
in Galactic Racer, with no repeat submitted). Over whole races, two each way,
the deep pipeline showed 102.6 and 104.3 frames a second and the automatic
depth 99.2 and 90.1, at 54 ms against 62 ms where it ran shallow: the shallow
stretches dropped frames, and dropped frames are what make SteamVR halve the
rate. That is the trade the tray's option already offers, so the option stays
as it is, on by default.

#### djules75's 0.2.13.2 fixes, brought in

Release 26 cherry-picks djules75's V439 to V443 as their own commits: no
prime hold on SteamVR (`presenter_holds_primes`), the D3D11 bridge's release
run at the game's xrEndFrame (`SwapchainState::pending_bridge_release`,
`release_pending_bridge_images`), a history capture that waits up to a
display period for a busy slot (`D3D12HistoryCaptureWait`,
`history_capture_wait`), Virtual Desktop's presenter given back past the
split-loop latch (`presenter_restore_after_bypass`), and the overlay-swapchain
test of the quarantine this fork had already removed. One adaptation: with
the release deferred, both eyes' releases run after both eyes' DLSS
evaluations are published, so "the evaluation released with the image"
(release 15's per-eye pairing) could take the other eye's; the publication
count is read at the game's release (`pending_bridge_release_publication`)
and handed to `release_swapchain_image_locked`, which uses it for the
resolve and for `eye_release_publication`. Their version bumps are not
taken: this fork stays V439, the runtime folder's name. Checked: 113 of 113
tests; on the Meta XR Simulator hello_xr on the D3D11 bridge (1628 deferred
releases a swapchain, generation at 2X, no history waits), hello_xr D3D12
with the panel, and Lies of P under UEVR (D3D12, 1349 DLSS evaluations in
1362 frames). A D3D11 game with a swapchain per eye and DLSS, the case the
adaptation is for, was not at hand.

#### Automatic 2X or 3X

A game under half the refresh rate at 2X repeats frames (Stellar Blade at
37 frames a second on a 90 Hz simulator: 16.5 repeats a second, 44% of its
game frames covering three display periods; on a 120 Hz Steam Frame worse),
and extrapolation, which turns the deeper pipeline off, has no slack to
hide one late frame. `[ofxr] frames_auto=1` (the tray's **Automatic** under
*Frames shown per game frame*, the panel's **Per frame** row) has the layer
choose the shape live, through the same switch the tray's 3X uses
(`apply_live_frame_multiplier`), so it needs `triple_switchable`. The
measures, kept on the xrEndFrame thread: the game's frame interval,
xrEndFrame to xrEndFrame, and its slack, the time it spent in the admission
wait and the pair hold, both as exponential averages over about ten frames;
the presenter's frames and repeats over the last two seconds
(`auto_presenter_frames`, `auto_presenter_repeats`), counted but not
judged, since a starved presenter repeats on one runtime and waits on
another (the fake runtime of the tests shows no repeat at all). The rule
(`automatic_frame_multiplier`): at 2X, after three seconds in the shape,
an interval over 2.15 display periods with under a quarter period of
slack at every poll of the two-second window goes to 3X; at 3X, after
four seconds, slack over 1.15 periods a frame at every poll goes back to
2X; a frame's interval counts as four periods at most; nothing while
SteamVR halves the rate, and each switch resets the measures. The first
build judged the averages as they stood at the poll, and a start-up stall
in Stellar Blade on the OpenXR Simulator (2.0 periods and a period of
slack a poll earlier) sent it to 3X at 14 s and back at 18 s, and a console
command's hiccup did the same at 243 s; replayed over the polls of that
log, the sustained rule fires on neither.

The slack alone cannot say that a game could hold 2X again. Stellar Blade
at the user's UEVR scale 1.233 (3703x3703 per eye on the OpenXR Simulator,
38 frames a second) went to 3X correctly, read 1.9 periods of slack there,
came back after the dwell, and flapped every four seconds, 35 switches in
140 s: paced to a third of the rate, a GPU-bound game no longer waits on
its own GPU inside the engine, so its CPU thread reaches the pair hold
early, and the slack measures CPU headroom. The game's GPU is therefore
probed on its own queue (`D3D12QueueProbe`, `src/d3d12/d3d12_queue_probe.cpp`):
once a display period the presenter thread submits a timestamp query, and
the query's GPU time against the CPU time of its submission, through the
queue's clock calibration (refreshed every second), says whether the queue
was ahead of the GPU by more than 0.3 ms, busy, or not, idle. The share of
busy probes over the window, times the game's frame interval, is its GPU
time a frame; 2X comes back only when that is under 1.85 periods as well as
the slack over 1.15. D3D12 sessions only, since a bridged session's queue
is the layer's own; without a probe the slack decides alone after twenty
seconds at 3X. The 1700 record's a carries the busy share in a thousand in
its high half (65535 when unknown). The same orbit again with the probe
(log pid 63936): loading into the city the interval rose through 2.5 to
2.9 periods with the slack gone and the GPU share climbing, 3X came at 16
s; one return at 20 s while the share's window still held the light frames
before the city (36%), 3X again at 26 s from 97% busy and 24 ms a frame,
and then 3X held for 116 s at 66-67% busy, 22.2 ms a frame, 30 real and 60
generated frames a second with no repeat at all, where run 3 had flapped
35 times; the pause menu read 53% busy, 11.7 ms a frame, and 2X came back
within six seconds and held with 45 + 45 and no repeat. The GPU condition
is now required at every poll of the window as well, which takes the one
return at 20 s with it. The same on native DLSS FG (log pid 61480): 3X in
the city at 66-68% busy with no repeats, 2X back in the menu, no flapping,
but the return came on the orbit's last second, a light view of two seconds
(the share at 0.40), and 2X then sat at 2.06 to 2.4 periods with 10 to 28%
repeats until the menu. So the window is sixteen polls: the newest eight
decide that the game is short - an interval over 2.15 periods, or more
than one shown frame in twelve a repeat, with no slack - and all sixteen
that it has room to spare, four seconds of light frames before 2X comes
back. Inline sessions have no hold for the slack to show in: the game's
threads block in the runtime's wait instead, its own forwarded one and the
layer's cycle's, and `with_runtime_entry_for_wait` counts that time as the
slack while no presenter runs. That wait hands out whole display slots,
though, so inline the slack is not room. Deep Rock Galactic under UEVR on
the Meta simulator at UEVR scale 1.9 (log pid 40076, an inline session)
sat at 2X for three minutes with an interval of 3.00 periods and 0.95 of
slack: a 23 ms frame rounded up to three slots, two shown frames in three,
and the slack never under the quarter period the short rule asked for.
Inline, short is now the frame's own time - the interval less the slack,
or the GPU's or the xrEndFrame thread's time if longer - over 1.9
periods, which is what 2.15 less 0.25 says with a presenter. Run again
with that rule (log pid 7592) 3X came at 12 s and held, but the interval
went from 33.4 to 44.5 ms: inline every runtime submission runs on the
game's thread - the simulator's xrEndFrame took 4.9 ms a frame at
3192x3344 (1.8 at 2064x2208) and the wait for the next slot follows it -
so the third frame added a fourth slot, three shown frames in four at 22.5
a second against two in three at 30. So inline the choice checks itself:
the 2X interval is kept when it leaves for 3X, and when the 3X interval has
run over it by 0.6 of a period at every poll of two seconds, 2X comes back
(700 with c=4) and 3X is not tried again while the 2X interval stays within
half a period of the one it was taken at. And on Meta's runtime and its
simulator a frame loop on one thread now takes the presenter from its first
generating frame, as on Virtual Desktop (`frame_loop_takes_presenter`): the
presenter hands the submissions to its own thread while the game renders,
which is the mode the rounding cannot reach, and where the automatic choice
was measured. `xrfg_frame_loop_meta_single` and `xrfg_frame_loop_meta_split`
cover the rule and `xrfg_layer_auto_frames_inline_paced` the check, on the
fake runtime with its waits paced to the period grid. Live on that build
Deep Rock Galactic asked for the presenter on its first generating frame
and had it 23 ms later (601 then 602, 9 s in). At UEVR scale 1.0 (log pid
55000) it then ran 45 a second at 2.00 periods with a period of slack in
the hold, the GPU at 50%, 90 submissions a second and no repeat, where the
inline session at the same scale had run 39.7 a second at 2.27 periods,
every third or fourth frame rounded up to a slot. At scale 1.9 (log pid
7552) it ran 36 a second at 2.5 periods with 0.7 of slack and the GPU at
72% against 30 inline, and stayed at 2X: there the simulator's own
xrEndFrame costs 6.7 ms a submission (internal_end_frame) and its wait
5 ms, so the presenter's two submissions a pair take 23.5 ms, longer than
the pair, and 3X could not have helped; the slack rule read that
correctly. The inline check itself was then run live with the session
held inline (`XRFG_TEST_INLINE_ONLY=1`, a test-only switch that keeps a
session this rule would promote on the inline path; log pid 66048, scale
1.9): 3X at 12.7 s by the frame's-own-time rule (interval 3.09 periods,
0.79 of slack, the frame 2.30), 2X back at 17.0 s with c=4 once the 3X
interval had run a slot over, and 2X held with the latch through the 44 s
watched, the interval back at 3.00 periods.

The six native games of the earlier round were run again on the Meta XR
Simulator at 90 Hz with the rule in (OFXR hybrid, Automatic on, 2064x2208
a side): Hubris, Kayak VR, Moss Book II, Moss, RoboQuest VR and FlatOut 4
VR. The five Unreal titles have a split frame loop (600) and had the
presenter already through the pipelined-wait promotion, so the rule did
not touch them (602 without a 601), and they ran as before, 45 real and 45
generated a second with no repeat and no quarantine (a first Hubris pass
read 35.7 while another session's CPU work ran beside it; alone, 44.9 at
2.00 periods). FlatOut 4 VR, a single-thread loop on the D3D11 bridge, is
the one the rule reached: inline in the earlier round at 28.4 game frames
a second with its pairs bunched (`last 10 s (inline)`), it now asks for the
presenter on its first generating frame (601, then 602) and runs 44.9 +
44.9 a second, 14 repeats in a minute, no quarantine. A D3D11 presenter
has a history (DCS World's split loop faulted the driver), so FlatOut was
soaked for 318 s on it: 44.6 + 44.5 a second throughout, 38 repeats in
27,228 shown frames, no quarantine, no presenter stop. Since the forwarded
wait can overlap a render thread's work in a pipelined game, room to spare
inline also needs
the xrEndFrame thread's CPU share of its wall time (`GetThreadTimes` a
poll) under 1.85 periods a frame, beside the probe's GPU time; the probe
runs on a thread of its own once a display period, uniform in time
whatever the game's phase, so a presenter is no longer needed. The D3D11
and Vulkan bridges have no probe and keep the chosen shape (a presenter
session on them comes back by slack alone after twenty seconds).
`xrfg_layer_auto_frames_inline` runs the test on the fake Pimax runtime,
which stays inline; its wait never blocks, so only the way up can be seen
there: 3X after 4.2 s of slow frames, and no second switch. Every poll
writes `presenter_transition` 1700 (a the GPU's busy share in a thousand
in the high half, 65535 when unknown, and the repeats in a thousand shown
in the low; b the xrEndFrame thread's CPU share in a thousand in the high
word and the slack in microseconds in the low; c the shape in the high
word and the interval in microseconds in the low), and a switch writes
700 with c=3. `xrfg_layer_auto_frames` paces the fake application
with a sleep: 25 ms a frame against a 10 ms period went to 3X after 3.0
s, and 2 ms a frame came back to 2X 4.2 s later.

#### Extrapolation in Stellar Blade: pacing, placement and prediction

A report of "a lot of stuttering and ghosting" in the ASW-like mode (a 120
Hz Steam Frame, Stellar Blade, FidelityFX + extrapolate + 3X) was taken to
the OpenXR Simulator at 90 Hz, 2644x2644 per eye, the same steady orbit in
the rain-city area, the flight recorder on (log pid 35584, captures
`build/captures/sb/extrap_2x` and `extrap_3x`). What the headset was shown,
period by period, from `presenter_content` (scratchpad `stutter.py`), and
where each synthetic was placed (`mode_score.py extrap`, with the period as
a parameter):

| | 2X | 3X |
|---|---|---|
| Game frames a second (the layer paces the game to refresh / frames) | 44.8 | 29.2 |
| Display periods per game frame | 2 on 99.2% | 3 on 99.9% |
| Repeats | 0.4% | 2.6%, all in one 1.3 s capture stall |
| Synthesis fraction, median | 1.499 | 1.667 |
| Placement error against the rate, fixed share / layer | 0.002 / 0.017 | 0.003 / 0.015 |

Phase correlation on the captures (eye 0, whole frame): at 2X the real
frames move 18 pixels a step and the generated frame sits 22 pixels on from
the real frame before it, where 27 would be exact (one to five pixels
short); at 3X the real frames move 38 and the captured second synthetic sits
62 to 64 on, where 63 is exact. Full-resolution crops of the character and
the phone sign show no doubling in either. So the mode paces and places as
designed where the game holds the rate. It cannot where the game does not:
extrapolation turns the deeper pipeline off, so a late frame repeats at
once, and on the 120 Hz headset 2X asks for 60 frames a second and 3X for
40 from a game that gives 37 to 45. For comparison, the native 2X session on
the simulator at 13:35 (36.8 frames a second on 90 Hz) repeated 16.5 frames
a second, 44% of its game frames covering three periods. Replayed a whole
frame ahead (`XRFG_TEST_REPLAY_EXTRAPOLATE=1`), the mesh from the game's
vectors erred 9.85 on r24_turn (10.75 across its 50 ms hitch, 9.06 on even
pairs) and 8.18 on user_rot5, against the interpolating hybrid's 6.87 and
8.20 and repeating the frame's 17.4 and 21.2: the prediction's own error is
not what the report describes. An automatic multiplier, 3X when the game is
under half the refresh rate, would remove most of the repeats; it is not
built.

#### Meta's mesh warps, measured

The warps were drawn in the synthesizer as Meta draws them: a grid over each
view, a vertex every cell of the real frame, each moved on along its motion
to its display time and rasterised over a plain copy of the real frame
(which shows only where the grid pulls away from the view's edge), against a
depth target. From the game's vectors, as Application SpaceWarp in the Meta
XR Simulator: each vertex takes the motion and depth of the nearest of four
taps half a cell around it, the nearest surface wins where the grid folds,
and triangles stretch over what moving content uncovers. From FidelityFX's
flow, as Asynchronous SpaceWarp on PC: no depth, the smallest displacement
wins. On the 42 triplets, a whole frame ahead, timed warm:

| Variant | Error | SSIM | Sharpness | Gradient error | GPU a pair |
|---|---|---|---|---|---|
| Gather from the vectors (five candidates) | 12.63 | 0.684 | 0.898 | 11.17 | 0.76 ms |
| Mesh from the vectors, 4 / 8 px cells | 12.17 / 12.05 | 0.676 / 0.681 | 0.906 / 0.874 | 11.29 / 11.01 | 0.40 / 0.24 ms |
| **Mesh from the vectors, 16 px (kept)** | **12.00** | **0.683** | 0.853 | **10.89** | **0.17 ms** |
| Mesh from the vectors, 24 / 32 px | 11.96 / 12.04 | 0.682 / 0.679 | 0.845 / 0.841 | 10.91 / 11.01 | 0.15 / 0.14 ms |
| 8 px with the sample refined along the motion, 1 / 2 steps | 12.08 / 12.07 | 0.679 / 0.680 | 0.877 | 11.05 / 11.04 | 0.30 / 0.35 ms |
| 16 px with the gather at motion edges over it | 12.40 | 0.672 | 0.939 | 11.61 | 0.73 ms |
| Gather from the flow | 12.41 | 0.660 | 0.945 | 12.16 | 1.21 ms |
| Mesh from the flow, 8 / 16 px (16 kept) | 11.90 / 11.90 | 0.673 / 0.673 | 0.846 / 0.844 | 11.51 / 11.49 | 0.54 / 0.47 ms |
| 16 px, the largest displacement winning (the Steam Frame's rule) | 11.91 | 0.673 | 0.841 | 11.49 | 0.46 ms |
| 16 px, each vertex taking the largest motion of four taps | 11.92 | 0.671 | 0.835 | 11.59 | 0.47 ms |
| 16 px flow mesh with the gather at motion edges | 12.32 | 0.659 | 0.935 | 12.08 | 1.25 ms |
| Gather from both (`extrapolate=2` before) | 11.88 | 0.688 | 0.888 | 10.98 | 1.49 ms |
| **The vectors' mesh asking the flow where it misses (kept)** | **11.53** | 0.683 | 0.872 | 11.02 | 1.03 ms |
| The same with a quarter-resolution flow (kept) | 11.56 | 0.682 | 0.869 | 11.03 | 0.86 ms |
| Flow mesh with a quarter-resolution flow | 12.61 | 0.642 | 0.854 | 12.26 | 0.30 ms |

The mesh is softer where its triangles stretch, but every other measure
favours it, at the edges too: the gather's per-pixel search over the mesh at
motion edges made it sharper and less accurate. On the synthetic sliding
rectangle, the gather's best case, the mesh errs 3.6 against 0 at the
rectangle's perfectly sharp edges (`test_dlss_extrapolation` holds it under a
fifth of repeating the frame).

#### How the Steam Frame does it

The headset's own SteamVR (2.18.2) was read off the device and decompiled
(`build/steamframe-re/REPORT.md` holds the full account). Streamed frames
carry colour only, no depth or vectors, and by default each is shown through
a rotation-only timewarp at display rate with a per-row pose for the rolling
scan-out: that hides the stream's 45-55 ms of latency, and is likely what
people praise. Its Motion Smoothing is off by default for the Frame; when
on, the Snapdragon's vision hardware computes dense optical flow at 512x512
per eye after the previous frame is turned to the new head pose, vectors are
zeroed in 6-degree cells whose motion is inconsistent from frame to frame and
where the image did not change, and a 512x512 grid mesh is pushed along them,
the largest displacement winning without depth. Measured here, its overlap
rule and a largest-motion dilation made no improvement to the flow mesh
(above). It also confirms that the headset adds only a rotation warp to what
is streamed, so the frames OFXR sends must be made for their real display
time, which `promise_shown_time` does.

The open points of that account were settled from the same binaries
(`build/steamframe-re/REPORT_ADDENDUM.md`). The per-frame flag that gates the
headset's motion estimation means "this refresh shows a newly submitted
frame", so estimation runs only on new frames and generation only on repeats.
Its translation terms are the app's own locomotion, from
`XrCompositionLayerSpaceWarpInfoFB`'s `appSpaceDeltaPose`, applied beyond
about 0.8 m and faded out over the outer 5% of the view; for a stream with no
vectors they are equal and do nothing. The vision hardware's 184-byte
configuration is passed along but never read: it runs on fixed constants.
And the plane its always-on reprojection uses sits at the far plane, which
makes it rotation-only unless gaze-dependent reprojection is turned on. None
of it changes what OFXR should send.

#### Flow at a quarter resolution

| Mode | Half: error / SSIM / GPU | Quarter: error / SSIM / GPU |
|---|---|---|
| FidelityFX interpolation | 7.86 / 0.779 / 0.58 ms | 8.13 / 0.768 / 0.42 ms |
| Hybrid (vectors and flow) | 7.18 / 0.815 / 1.47 ms | 7.19 / 0.815 / 1.31 ms |
| Flow mesh extrapolation | 11.90 / 0.673 / 0.47 ms | 12.61 / 0.642 / 0.30 ms |
| Combined mesh extrapolation | 11.53 / 0.683 / 1.03 ms | 11.56 / 0.682 / 0.86 ms |

The quarter is kept where the flow only patches the vectors. Its packed input
averages four bilinear taps, so each packed pixel stands for its 4x4 rather
than a quarter of them.

Whatever scale is chosen, the flow's per-eye input is held to at most 2304
pixels tall: the scale steps down (75% to 50% to 25%) until it fits. Ordinary
eyes keep their scale (75% of Galactic Racer's 3004 is 2253), but SteamVR can
hand a supersampling game far larger ones. Trombone Champ: Unflattened asked
for 6514x6514 per eye; NVIDIA's flow at 75% took 9.8 and 8.2 ms for the two
eyes, a pair 19-21 ms, and only 28.6 of 59.7 game frames a second got their
synthetic. Capped (a quarter, 1628 pixels), each eye's flow took 1.2 ms and
the pair 3.8-4.7 ms, and every game frame got its synthetic (119.5 shown a
second).

#### Under head turns

Every recording above was made with the headset still. Users of Galactic
Racer reported native DLSS FG juddering and "fighting the camera" when they
turned their heads, most of all in menus, where the game's camera stands
still and only the head moves. `XRFG_TEST_HEAD_SWAY_DEG=<degrees>` (and
`XRFG_TEST_HEAD_SWAY_PERIOD_S`, default 4) in the game's environment turns
every head pose the game is given about the vertical by that amplitude times
a sine of the display time, so the game renders head turns with no one in the
headset; only then does the layer intercept xrCreateReferenceSpace and
xrLocateSpace. With a 20-degree sway every 3 s (up to 42 degrees a second),
Galactic Racer ran under UEVR on the Meta XR Simulator: eight runs of its
title menu, where only the head moves, and ten of a Jakku race. Replayed
(`sway_replay.py`), error where the scene moved and SSIM:

| Method | Menu | Race |
|---|---|---|
| OFXR hybrid, quarter flow (default) | 2.15 / 0.989 | 4.70 / 0.839 |
| OFXR hybrid, half flow | 2.15 / 0.989 | 4.59 / 0.846 |
| OFXR DLSS vectors | 2.15 / 0.989 | 5.46 / 0.822 |
| OFXR FidelityFX half flow | 2.20 / 0.985 | 5.04 / 0.824 |
| OFXR NVIDIA medium flow | 2.20 / 0.989 | 4.59 / 0.849 |
| Native DLSS FG 100% / 67% | 3.04 / 3.32, 0.981 | 6.93 / 6.77, 0.738 / 0.770 |
| Repeating the frame | 24.4 / 0.494 | 14.3 / 0.499 |

The hybrid is best or level under head turns: in the menu the vectors are all
it needs, and in the race its flow patches what they miss and it matches
NVIDIA's flow, which costs several times as much live (4.7-5.6 ms a pair).
Half-resolution flow gains 2% in the turning race, nearly all of it in one
of the ten runs (12.1 to 11.2; the rest within 2.5%), and nothing with a still
head (7.18 against 7.19 on the 42 triplets), for 7% more GPU time (450 against
482 us a pair at 1440x1584, replayed warm), so quarter stays. Native
generation trails every OFXR method in both: its frame is softer, from the
per-pair reset and the alignment's resampling. Since synthetics are now shown
in their own midway camera, NGX was also tried without the alignment, given A
as it is and the game's whole vectors so that it carries the turn itself and
its frame lies midway (`XRFG_TEST_REPLAY_NATIVE_UNALIGNED=1`): the menu's
error rose from 3.04 to 7.04, so the alignment stays.

The game's vectors carry the head turn exactly: on distant pixels of the race
runs where the car ran straight, they matched the turn between the views to
within 0.2 pixels (`vector_rotation.py`). The replay at first said otherwise
(menu: flow 2.20, hybrid 2.57, vectors 2.90), because it stretches frame 2's
vectors to reach frame 0 by the game's frame times while a head turn follows
display time; where frames were unevenly paced (in one race run the turn's
two steps were 1.35 and 0.91 degrees while the game's times were 16.1 and
12.7 ms) the stretched vectors missed part of the turn that flow measured
whole. `XRFG_TEST_REPLAY_CHAIN_VECTORS=head` stretches only the game's own
motion and takes the head turn from frame 2 to frame 0 from the views; the
42 triplets have no head turn, so it leaves them as they were. `=1` chains
frame 1's vectors instead, but at moving edges it samples them on the wrong
object (42 triplets: hybrid 7.47 against 7.19). An earlier finding that
giving the hybrid's flow more of the ties helped the turning menu was
answering the stretched vectors, and is void. The replay had also generated
native frames in the newer real frame's camera while scoring them against
the middle frame's, which under a head turn read as bad as repeating a frame
(24-27); native now gets the middle frame's camera through
`synthetic_camera_to_current`, as OFXR's methods do
(`XRFG_TEST_REPLAY_NATIVE_B_CAMERA=1` restores the old scoring).

#### On frames no tuning used

The 42 triplets chose these settings, so they were checked on 17 that played
no part: 14 from later Galactic Racer sessions and 3 from Hubris's menu
(`holdout.py`; error where the scene moved, SSIM, gradient error):

| Galactic Racer, 14 triplets | Error | SSIM | Gradient error |
|---|---|---|---|
| Extrapolation, gather / **mesh**, vectors | 9.71 / **9.25** | 0.739 / 0.739 | 10.58 / **10.44** |
| Extrapolation, gather / **mesh**, FidelityFX flow | 9.70 / **9.21** | 0.722 / **0.736** | 11.36 / **10.90** |
| Extrapolation, both: gather half / **mesh quarter** | 9.57 / **9.21** | 0.740 / 0.738 | 10.40 / **10.36** |
| Repeating the frame | 15.50 | 0.609 | 13.19 |
| Interpolation, hybrid half / **quarter** | 5.25 / 5.26 | 0.830 / 0.830 | 7.15 / 7.14 |
| Interpolation, vectors / FidelityFX / NVIDIA medium | 5.79 / 6.32 / 5.32 | 0.823 / 0.794 / 0.823 | 7.69 / 7.68 / 7.54 |

The mesh and the quarter-resolution flow held on frames they were not chosen
on. Hubris's menu moves only 0.1% of the image, so its three triplets say
little; there the flow predicted the logo best (16.3 against 21.6 for the
vectors, which do not describe it).

The test harness starts NVIDIA's `nvngx_update.exe` (five per process) with
every NGX initialisation, and they linger for minutes. Sweeps of a few
hundred replays exhausted the commit limit with over a thousand of them; kill
them between runs.

Hubris's menu, where only a glowing logo moves (0.2% of
the image) and its vectors do not describe it, ordered them FidelityFX flow
8.7, native 10.5-11.5, NVIDIA flow 13.6 and OFXR + DLSS vectors 16.9, against
a blend's 15.9. So the synthetic results overstate OFXR + DLSS vectors:
real vectors are not exact, and native generation, which also costs four to
six times as much, holds up better where they fail.

OFXR + DLSS vectors measured 3.95 overall and 25.9 at the edges until its
occlusion handling changed. It solves, per output pixel, for the point of B
whose vector passes through the pixel at the generated instant, and blends
A's and B's samples along it. Where they disagree it used to fade to a blend
of A and B at the same pixel, which showed both frames' edges at once. Now:

- Where the samples disagree, B's warped sample stands in: background a
  trailing edge uncovers is visible only in B. This alone took the error to
  2.02 and the edges to 13.3. A's warped sample instead measured 1.91 but 23.7
  at the edges, and dropping the fallback altogether 1.95 and 18.1.
- The confidence slope for game motion is 3 rather than 6, because its
  disagreements are mostly resampling at sharp detail: 1.92 and 12.9. Slopes
  of 2, 1.5 and 12 measured 1.91/13.2, 1.91/13.8 and 2.12/13.7.
- Background a leading edge is covering shows only in A. There the solve has
  no fixed point: B shows the occluder at the pixel, so the solve starts
  inside it and alternates outside and in. After an odd number of steps it
  ends outside, on the covered background's displacement, and A is sampled
  one-sidedly along it wherever the solve is still half a pixel or more from
  converging: 1.90 and 11.7. The step count stays at three; four ended inside
  and measured 12.7 at the edges, five measured as three.
- That left the trailing edge's other half. There B shows background the
  square has uncovered, but at the generated instant the faster square still
  covers it. The solve finds the background - also a fixed point - and the
  square is the other solution. So, where the samples disagree by more than
  0.1 and a vector 16 pixels away (up, down, left or right) differs by more
  than a pixel, the solve is rerun from starts on another surface. The
  starts are 8 directions at 4, 8, 16 and 32 pixels, nearest first, with at
  most two solves. A solution whose own A and B samples agree within 0.05 is
  visible in both frames, so in front, and it wins: 1.81 and 7.35. The new
  `test_dlss_motion_vector_occlusion_edges` checked this on exact integer
  motion: 0.16 at the edges, against 24.6 for the old same-pixel blend and
  16.5 before the search. Before the gate, triggers of 0.05 and 0.2 measured
  7.28 and 7.69 at the edges, and agreement thresholds of 0.025 and 0.1
  measured 7.47 and 7.35. With the gate, capping the solves at one, three or
  six all measured 7.35.
- A HUD drawn after DLSS has no vectors of its own: it carries the vectors
  of the scene behind it. With B's warped sample as the fallback, a static
  striped overlay over a moving background erred 100.8, against 31.6 for the
  old same-pixel blend, which kept it wherever the warped samples disagreed.
  Now a pixel that is unchanged on screen between the frames, along with its
  neighbours two pixels away, and that its own vector does not explain
  (the vector's A sample differs by more than 0.1), is kept as it is: 6.5.
  Correct vectors explain a moving surface however flat or striped it is, so
  this never fires on the benchmark scene, whose figures are unchanged.
- All of that first treated every disagreement as an occlusion. A fade or a
  flash is not one: the warped samples are one surface whose shading
  changed, and their motion-compensated blend is the frame between. On a
  moving scene that darkens to 60%, B's sample made the fade step at half
  the rate: 22.5, against 45.7 for the old same-pixel blend, which does not
  follow the motion. Occlusions happen only at motion edges, so the
  occlusion handling now runs only where a vector 16 pixels away differs by
  more than a pixel, and only above a disagreement of 0.1 (0.02 and 0.05
  measured 7.61 and 7.60 at the edges, against 7.55). Elsewhere the
  motion-compensated blend is shown: 0.08 on the fade, and 1.74 overall and
  7.55 at the edges on the benchmark scene; 3X, 1.38 and 1.37. The overlay
  check then had to ask the vector across its 5-pixel patch, not only at
  the centre, because a repeating HUD pattern can match its own shift at one
  pixel and no longer has B's sample to fall back on: 6.9. It samples the
  patch's predictions only when the centre is explained and the patch is not
  flat, which kept its cost. `test_dlss_motion_vector_occlusion_edges` checks
  the occlusion (0.68), the overlay and the fade.

The optical-flow methods keep their same-pixel blend. B's warped sample as
their fallback took the scene's edge error from 45.2 to 34.0 (FidelityFX
half), 45.6 to 35.3 (full) and 51.3 to 45.3 (NVIDIA), but it also makes
content that changes in place step instead of cross-fading, failing the
tests that keep A, the generated frame and B distinct. Gated, like game
motion, on a flow 16 pixels away differing by more than a pixel, or by more
than four, it still failed the cropped-view rotation test (4.8 and 4.4
against a limit of 1.0). Estimated flow is wrong where it would act, while
the blend, which follows the tracked camera, is already exact for a static
world.

Rejected: keeping the same-pixel blend wherever the pixel, or a 5-point patch
around it, is unchanged between the frames, without asking the vector. Flat
stripes pass that test, and it measured 2.6, or 4.5 as a check ahead of the
warp. Nearest-tap vectors across motion discontinuities,
instead of bilinear, measured 13.4 at the edges. An ungated search over 32
starts cost 6.8 ms per pair at 3004x3004 on `XRFG_TEST_FG_BENCH`, whose
turning-head frames disagree with their vectors almost everywhere. Gated by
the motion edge, with single-texel vector probes, that benchmark measures
0.50 ms at the median against 0.45 before (p10 0.45 against 0.42), and its
uniform vectors never start a search. The overlay check adds 0.01-0.02 ms
there. In Hubris, whose menu scene is mostly still, two rounds measured 0.325
and 0.326 ms with the search, against 0.323 before. The overlay check took
that to 0.397 ms when it read a filtered vector and sampled A at every pixel
unchanged on screen. It now asks the guide texel's vector first, and samples
A only where that vector moves the pixel: 0.365 ms, and 0.374 ms with the
fade handling. Checking after the solve's first step instead, whose vector it
could reuse, measured 0.58-0.63 ms on the moving benchmark.

At 3X, against the true frames a third and two thirds of the way from A, 67%
errs 4.9 and 4.6 where full resolution errs 3.4 and 3.0: each generated frame
restores detail from its own point along the motion. OFXR + DLSS vectors errs
1.38 and 1.37 at 3X, against 4.88 and 2.96 before its occlusion handling.

67% beats 70% and 75% because it puts the feature on the game's two-thirds
guide grid; 70%, 64% and 60% measured 7.4, 6.3 and 8.4 with a quarter
tolerance and bilinear upsampling, against 67%'s 6.1. 85% is no use: it beats
full resolution overall, through the restored detail, but it loses the
two-thirds guide grid and costs more than 100% (2.03 ms still and 2.43
turning). Of tolerances from 1/32 to 1, a half measured the least error,
overall and at the edges.

These also measured no better:
- **Nearest-surface guides at 50%.** Picking the nearest surface when the
  guide grid is coarser than the game's guides made no difference.
- **Foveated packing.** A smooth cubic gives full resolution at the centre,
  falling towards the edges, within the same pixel count. It measured worse
  at every scale, at the centre too for 75% and 50%: the warp makes uniform
  motion non-uniform, which NGX and the restore handle worse.
- **A detail swap at full resolution.** Where a 3x3 tent of NGX's frame
  agrees with one of B at the motion-compensated point, NGX's detail above the
  tent is replaced with B's. It took the error from 4.25 to 4.13 but the edges
  from 8.7 to 9.4, for 18 more reads a pixel.

`XRFG_NATIVE_DLSSG_SCALE` and `XRFG_NATIVE_DLSSG_DETAIL` (the tolerance's
reciprocal; 0 turns the restore off) override both for experiments.

Running the pack and NGX on a compute queue, which NVIDIA can overlap with the
game's graphics where queues of the same type time-slice, and composing on the
synthesis queue once a fence passes, was also tried. NGX accepts a compute list
and the output was identical, but the split never helped. Live in Galactic
Racer on the Steam Frame, interleaved with the current path, it added 0.2 to
0.7 ms to the median pair at every load tried in the hub scene. With the scene
heavy enough to hold the game at about 90 of 120 frames a second (UEVR
resolution scale 1.19), the game's frame rate and the number of pairs delivered
were unchanged, and the 90th percentile rose from about 5.4 to 8.2 ms. Racing
the Arcade time trial at speed, the median pair went from 5.1-5.3 to 6.3 ms,
the 90th percentile from 5.8 to 7.9 ms, and the game lost about two frames a
second. Generation therefore stays on the one high-priority direct queue.

sRGB swapchains are encoded by the pack shader into 8-bit private textures
(the reseed's into 10-bit ones, or 8-bit where the adapter lacks typed UAV
stores for 10-bit), and decoded again when the generated image is written;
unchanged pixels round-trip exactly. Resize retirement polls the previous completion
fence, and frame submission does not wait on the CPU. Generated pixels remain
inside the submitted rectangles. NGX's disable-interpolation output is checked
on the GPU before presenting a generated image.

For a live diagnostic run, set `XRFG_TEST_NATIVE_DLSSG_DECISIONS=1` before
launching the game. This adds asynchronous readback of those GPU decisions to
`ofxr-native-decisions-pid*-instance*.log` beside the layer DLL. Each completed
record identifies the guide publication, eye count, output count and flags.
A zero low byte selects generated pixels; a nonzero low byte selects the
current frame. Inspect the file contents while the game runs: Windows can
report a stale directory-entry file size until the writer closes it. This
diagnostic is off by default and is not a frame-rate benchmark.

NGX uses fixed interpolation fractions: one generated image at 1/2, or two at
1/3 and 2/3 for OFXR's 3X setting, subject to the native feature's capabilities.
The layer uses those fractions in native mode, and the same shares in OFXR's
own algorithm, which measured closer to where the images are shown than
fractions from the game's stamps (see "Where each synthetic is placed"). 3X
needs NGX
multi-frame generation; on adapters without it, native mode stays at 2X, and
a 3X request that reaches the feature anyway reports
`multi_frame_unsupported` (status 9).
The optical-flow preset, scale and bidirectional controls configure the
original OFXR algorithm; they do not tune NVIDIA's neural feature, whose own
resolution is `dlssg_resolution`.

Native mode skips the original optical-flow contexts, scratch textures and
extra command lists. Fully covered outputs also skip the preliminary current
frame copy; cropped outputs retain it to preserve pixels outside the viewports.
The real-frame copy is recorded separately and can be deferred until after the
generated image is submitted. Packing the aligned previous-frame seed omits
motion-vector depth dilation. These changes preserve the input resolution and
camera alignment. Completed native pairs resolve GPU timestamps for the total
synthesis span; the per-stage optical-flow timings do not describe NGX internals.

The game's FPS counter measures source frames. OFXR's generated frames are
submitted through OpenXR and do not increment that counter. At 120 Hz, 2X mode
paces source frames toward 60 FPS, subject to rendering and generation cost.
Compare on/off in the same scene and resolution, with diagnostic recording off
and an active headset. SteamVR standby and its dashboard can throttle the source
to roughly 100 ms waits; discard those samples. Compositor presents also include
reprojection, so they alone do not establish the rate of unique generated images.

The hardware test checks translated stereo quality against a same-pixel blend,
3X output order, cropped view bounds, rotational camera isolation, command-list
reuse, real-frame copies, explicit resets, and missing-depth fallback. The
shared tests also verify typed depth snapshots preserve values while allowing
shader reads. These small synthetic tests do not establish headset-resolution
cost, headset comfort, or behavior in a particular game. Full camera translation
reprojection and separate baked-in HUD/UI guides remain future work; the bridge
still has its existing limitation for camera translation.

## The status panel

The panel in the headset (README, "The status panel") is three pieces, one
per layer of the code, and only the last one touches the game.

**The model** (`include/xrfg/status_panel_model.hpp`,
`src/core/status_panel_model.cpp`, in `xrfg_core`) is pure: the
`[overlay] panel` setting, the flip gesture, the panel's text and its pixels.

- The gesture is xrFPS's
  ([elliotttate/xrFPS](https://github.com/elliotttate/xrFPS),
  `src/overlay.cpp`, `update_gesture`): a grip whose own +Y, located in a
  space whose +Y is up, has turned more than 120 degrees from straight up is
  upside down (`grip_upside_down`: the up axis's height below
  cos 120 = -0.5); a quarter
  second of that shows the panel, half a second the right way up hides it,
  and a break in either restarts the delay (`FlipGesture`). The panel stays
  on the hand that brought it up while that hand stays turned. Placement is
  xrFPS's "adaptive underside": the grip's pose turned half a turn about its
  own Z, which stands the panel upright and facing back along the
  controller, and lifted along the grip's -Y - up, while it is upside down -
  by half the panel's height and 5 cm, where xrFPS lifts its smaller HUD
  11 cm (`panel_pose_on_grip`).
- The text compares three configurations. `asked` is the tray's choice as
  the ini says now. `session` is what the session set out to run: the method
  the embedded control read when the process started (the tray's method
  reaches a running game only through the provider API, so a tray change
  waits for the next game start), the hybrid and the mesh as last re-read at
  a control change, extrapolation and the depth as read at xrCreateSession,
  and 3X as the tray has it, since a switchable session follows it live.
  `running` is the session's synthesizer options after every fallback. A
  fallback is `session` against `running` (amber or red, and FALLBACK or NOT
  GENERATING in the corner); a pending change is `asked` against `session`
  (grey). Whether the game's vectors went into the frames is read from the
  guide statistics' counters since the previous repaint, not from the last
  status alone, which flips between used and rejected pair by pair; a
  window with no pair either way (the first after the panel was hidden)
  falls back on the last status. Native DLSS FG counts as generating only
  for pairs that used the guides: a pair NGX skipped still goes out, as a
  copy of the real frame, and the counter would call it generated.
- The latency line is the README's account of the pipeline (interpolation
  shows each real frame a display period later, two at 3X, the deep pipeline
  one more, extrapolation none) plus the promise correction
  (`observe_promise_lateness`), not a measurement.
- The Guides line's reset share and the Pace line come from what the layer
  already counts: `D3D12NativeDlssG::reseeded_pairs`, through
  `D3D12FrameSynthesizer::native_reseeded_pairs` and summed over the eyes'
  synthesizers at each repaint, and the rates. Pace is a line of its own
  rather than a reason, since a short game is no fallback: it shows when the
  game's rate is under nine tenths of the refresh rate divided by the frames
  a game frame makes (Stellar Blade at 37 a second on a 120 Hz Steam Frame at
  2X, where one submission in six was a repeat), and names what to try.
- Editing (README, "Changing settings from the panel") is three pieces too.
  The model (`panel_editables`, `PanelNavigator`, `PanelCursor`,
  `panel_cursor_apply`) is pure: the editables are derived from the input's
  `asked` method and the session's flags (3X not through the D3D11 interop
  nor on single-frame native, the flow fixed without NVIDIA, the camera and
  the pipeline marked for the next start); the navigator steps on crossing
  0.6 of the stick, re-arms under 0.3, repeats after 500 ms every 300 ms,
  takes a new direction without a release as a new press, and takes the
  trigger once it has been held 400 ms, once per press (a pull in play with
  the controller turned over must not change a setting; the tray names each
  adopted change in its balloon and in a `headset-change` line of
  tray-lifecycle.log); the cursor's first command only shows
  it, up and down wrap and drop a staged choice, left and right stage, the
  trigger applies a staged choice and a stage lives six seconds. The text
  carries the block when `controls_bound`, the row the cursor stands on in
  chevrons on a bar (`Canvas::bar`), a staged choice in the accent tone; the
  panel's texture is 768 high for it and is still cut to its lines. The
  layer (`create_panel_input`) makes a second action set,
  `ofxr_panel_input` at priority 1000, with `navigate` (vector2) and
  `confirm` (boolean) on both hands, and binds them per interaction profile
  it knows (Touch, Touch Plus and Pro, Index, Vive wand and Focus 3, WMR,
  HP Reverb, Pico 4 and Neo 3: the thumbstick or trackpad and the trigger's
  value or click), with a thumbstick-and-trigger guess for any other.
  `layer_suggest_interaction_profile_bindings` is tiered: the game's
  bindings with the grips and the controls, then with the grips alone, then
  as the game made them (record `status_panel` 2, a the tier). The attach
  takes both sets or falls back to the grip set (3, c whether the controls
  were taken). `layer_sync_actions` adds the controls' set with the hand's
  subaction only while the overlay reports the panel on that hand
  (`interactive_hand`), in the exclusive mode without the game's sets, and
  reads the two actions there, where changedSinceLastSync is exact, into
  `panel_input_sample` under `panel_input_mutex`. On the end-frame thread
  `update_panel_cursor` feeds the navigator every frame, moves the cursor
  through the editables cached at the last repaint, and applies a change
  (`apply_panel_change`: the method, resolution, flow, scale and both ways
  through `embedded::request`; 3X, the hybrid and extrapolation, the
  camera and the pipeline as ini keys, with `embedded::bump_revision` where
  a control change re-reads them and `triple_poll_at` cleared for the 3X
  poll; pause through `ManualArmControl::set_pause` on the tray's event),
  recorded as `status_panel` 6; the cursor is dropped with the panel that
  showed it (`panel_visibility_generation`). The tray reads back: a
  two-second timer, the menu opening and every rewrite of the runtime ini
  first run `adopt_runtime_changes`, which takes the frame-generation keys
  of a runtime ini that differs from the one it last wrote or adopted into
  its settings (`adopt_runtime_ini`, the keys and no other), saves them,
  and reads the pause event's state; so the tray's own rewrite never
  reverts a headset change. Tests: the model in `xrfg_status_panel_tests`;
  the bindings, attach and sync through the fake runtime in
  `xrfg_layer_panel_input` (seven bindings on Touch, the Index trigger as a
  click, the simple controller keeping the grips and losing the controls,
  three sets attached, two synced without a hand); the adoption in
  `xrfg_standalone_launcher_tests`.
- A tray restart adopts running sessions. The layer's stop is terminal for an
  XrSession (`manual_stop_applied`), so a tray that stopped a game's session
  while restarting left it without generation until the game restarted; a
  tray killed together with its watchdog, or started again after a crash, did
  exactly that: `retire_manifest` signalled every orphaned manifest's arm
  event from `cleanup_owned_registrations` at start-up. Retiring a manifest
  now only unregisters and removes it, and reports it (`retired`); the tray,
  at start-up and before arming, opens each orphan's arm and pause events
  (`open_arm_signal`, `open_pause_signal`: a live session's handle keeps the
  kernel object alive, so an event that cannot be opened has no session) and
  holds them as adopted (lifecycle log `adopt-arm`). Disarm and Exit signal
  them with the tray's own (`stop-adopted`), and the pause sets and resets
  them too. A tray that crashes therefore leaves running games generating
  until they end or a tray is started and disarmed, while new games load
  nothing meanwhile, since the watchdog still retires the registration.
  `xrfg_standalone_launcher_tests` checks that retiring leaves a held event
  unsignalled and that the adopted handle stops it.
- The pixels are font8x8's public-domain glyphs (`THIRD_PARTY.md`), grown by
  Scale2x to 16 and 32 pixels once per process, on a 1024-wide texture with a
  rounded, bordered background. Premultiplied alpha, like the counter's;
  colours are chosen in sRGB and converted to linear light for a UNORM
  swapchain. The panel is cut to its lines: the rasterizer reports the rows it
  drew, the quad's `imageRect` shows only those and only those are uploaded,
  so the quad's height follows the number of lines at a fixed text size.

**The overlay** (`src/layer/openxr_fps_overlay.cpp`) owns a second quad
beside the counter, each with its own swapchain and upload state (`Surface`);
the counter's behaviour, format choice and failure handling are unchanged.
The panel's swapchain is made the first time the panel is wanted, sRGB first.
`application_frame` repaints it at most four times a second, on the
application's end-frame thread like the counter, while it is up or while a
controller has just been turned over, so it appears already painted;
`status_wanted` tells the layer when to gather a `StatusPanelInput`, so
nothing is gathered while the panel is hidden. The overlay's lock is held by
a presenter through the runtime's xrEndFrame, so `status_wanted` reads an
atomic rather than the lock, and the raster, the one piece of the overlay's
work long enough to matter (0.64 ms for a 1024x512 panel of eight lines,
Release build, on the development machine), runs between two short holds of
it. Where it goes is decided in
`end_frame`, once per submission and on whichever thread submits: in gesture
mode both grips are located in a LOCAL space of the overlay's own at that
submission's display time, only a pose with the TRACKED bits counts (a
runtime keeps VALID on a lost controller with its last pose, as xrFPS
found), and the panel follows the hand at the display's rate rather than
the game's. While it shows it is the one quad added, in place of the
counter. `always` puts it in the VIEW space, 1 m away and 30 cm down, tilted
to face the eye.

**The layer** (`src/layer/openxr_layer.cpp`) supplies the controllers and the
status. An API layer has no input of its own: an action set reaches the
runtime only through the application's own suggest, attach and sync calls,
and only before the application attaches. So, as xrFPS does, with
`panel=gesture` read at xrCreateInstance the layer makes an action set
(`ofxr_status_panel`, priority 0) with one pose action on both grips
(`create_panel_gesture`), and adds it to the application's suggested
bindings, attach and every sync. Each addition is undone on the spot when
the runtime refuses it - the application's call is made again exactly as it
was - so a gamepad profile, which has no grip, gets the application's own
bindings, and the game's input never depends on the layer's. With the panel
always on or off the three calls are not intercepted at all. The attach
makes a grip space a hand and hands them to the overlay; they are destroyed
with the session, after the overlay. `status_panel_input` gathers the
status at the application's xrEndFrame when the overlay asks; the session
keeps what nothing else did: the reason the last frame passed through
(`bypass_generation`), whether the depth or 3X fell back to the runtime's
swapchain limit (`fall_back_to_shallow_pipeline`), the guide counters at the
previous repaint, NGX's frame count, asked once, and each synthesizer's GPU
time a pair, smoothed, from the timings the flight recorder already reads
(`log_completed_nvidia_gpu_timings`). Those exist only while the recorder
runs, since synthesis is timed only then, and are summed over the
synthesizers measured in the last two seconds: a game with a swapchain per
eye has one per eye.

The flight log's `status_panel` records say whether the action set was made
(1), what each binding suggestion did (2), whether the attach took it and
how many grip spaces were made (3), and when the panel was shown and hidden
(4, 5).

Tests: `xrfg_status_panel_tests` (the setting, the gesture's hysteresis and
hands, the placement, every fallback and pending line, the pixels; with an
argument it writes a preview PPM); `xrfg_openxr_fps_overlay_d3d12` and
`_d3d11` (the panel always on, off and on a turned controller through a fake
runtime, uploaded on WARP and read back, its quad's space, pose, size and
cut, lost controllers and the hide delay); `xrfg_layer_panel_input` and
`xrfg_layer_panel_input_off` (the input calls through the fake runtime, with
and without the gesture); `xrfg_fps_overlay_tests` (generated and repeated
frames counted apart); `xrfg_standalone_launcher_tests` (the tray setting).
Not covered without a headset: how readable the panel is at its size and
distance, which way a runtime's quad faces in practice, whether a real
runtime takes the extra action set for every controller profile a game
suggests, and its cost on a game's render thread.

## Continuous integration

`.github/workflows/build.yml` runs the same steps on a clean `windows-2022`
runner for every push to `main`, every pull request and every tag: it fetches
the FidelityFX SDK at the pinned commit and `openvr.h` at the pinned SHA-256
(a mismatch fails the job), builds, runs `ctest` on WARP, and uploads the file
set the release zips carry, named after the version stamp and the commit. The
FidelityFX libraries are cached between runs, so only the first run after a
pin change pays for that build. Tag builds are the input to code signing
through the SignPath Foundation; the `sign` job is inert until the repository
variables it names are set.

## OpenVR header

**Required for the layer.** The overlay reports what the headset actually
received, not what the layer submitted, and SteamVR only exposes that through
OpenVR — `IVRCompositor::GetCumulativeStats`, the same source fpsVR reads. No
OpenXR call reports it. The standalone `xrfg_steamvr_delivery_probe` uses the
same header.

The Vulkan headers the Vulkan interop compiles against are vendored under
`external/Vulkan-Headers` (Khronos Vulkan-Headers `v1.3.296`, listed in
`THIRD_PARTY.md`); nothing further to install, and no Vulkan library is
linked.

Place a single header at `external/openvr/openvr.h`:

```powershell
curl -sSL -o external/openvr/openvr.h `
  https://raw.githubusercontent.com/ValveSoftware/openvr/v2.5.1/headers/openvr.h
```

| File | Tag | SHA-256 |
| --- | --- | --- |
| `openvr.h` | `v2.5.1` | `94E5545370159C85F87CD6E15DD3739F7C919FC7A6E869F5E4ED463533A07ED0` |

Like the FidelityFX SDK, the checkout is intentionally not committed, and the
layer build hard-fails without it. **Nothing from OpenVR is linked or
redistributed**: the layer loads SteamVR's own `openvr_api.dll` at run time,
located through `%LOCALAPPDATA%\openvr\openvrpaths.vrpath`, and resolves its
entry points with `GetProcAddress`. The layer DLL therefore carries no import on
it and runs unchanged where SteamVR is absent — the feature is inert on every
other runtime.

OpenVR is BSD-3-Clause, and because the released binary is built from that
header, Valve's notice ships in `licenses/OpenVR-BSD-3-Clause.txt`. See
`THIRD_PARTY.md`.

The probe needs no D3D12, no FidelityFX and no layer, so it still builds on a
checkout that cannot build the layer:

```powershell
cmake -S . -B build-probe -G "Visual Studio 17 2022" -A x64 `
  -DXRFG_BUILD_LAYER=OFF -DXRFG_BUILD_STANDALONE=OFF -DXRFG_BUILD_TESTS=OFF
cmake --build build-probe --config Release --target xrfg_steamvr_delivery_probe
```

It is **not registered as a test**: it needs a live SteamVR session with an
application running, so it is run by hand, like `xrfg_nvidia_optical_flow_probe`.

It writes to `%LOCALAPPDATA%\OFXR Bridge\DeliveryProbe\` - beside the flight
logs rather than in them - one line per second, stamped with the wall clock.
The flight log's `ms=` is elapsed since session start and its filename carries
the wall clock of that start, so the two join offline on that column.
