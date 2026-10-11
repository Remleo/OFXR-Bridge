#pragma once

#include "xrfg/d3d12_frame_synthesizer.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace xrfg {

// Native NGX integration. The caller drains the queue before destroying this
// object and owns the frame/guide leases until its completion fence passes.
class D3D12NativeDlssG final {
  public:
    struct Output {
        ID3D12Resource *image{};
        // The camera the output is shown from, per view, as the rotation
        // (x, y, z, w) that takes a ray of it into B's camera; the compose
        // samples NGX's frame along that ray, and A beyond B's view. None:
        // B's own camera.
        std::optional<std::array<std::array<float, 4>, 2>> camera_to_current{};
    };
    // Why the last record() returned S_FALSE.
    enum class Skip {
        none,
        guides,                  // missing, reset, discontinuous or invalid guides
        resizing,                // a resized eye waits for its previous submission
        multi_frame_unsupported, // more outputs than the adapter's feature generates
        feature_unavailable,     // NGX could not create the feature; retried later
        evaluate_failed,         // NGX rejected an evaluation; the history reseeds
        hitch,                   // the pair spans a game hitch; the real frame is shown
    };
    D3D12NativeDlssG();
    ~D3D12NativeDlssG();
    // scale is the feature's resolution in percent of each eye's, 25 to 100;
    // XRFG_NATIVE_DLSSG_SCALE overrides it. camera fits the camera's own
    // motion to each pair's vectors and depth on the GPU and hands it to the
    // next pair: the pack takes the camera's flow out of the vectors and NGX
    // is told they carry object motion alone, with the camera's motion in
    // its matrices, which is its contract. A third-person character the
    // camera orbits then stops smearing. It needs a feature per eye, so it
    // costs more GPU; XRFG_NATIVE_DLSSG_CAMERA overrides it.
    HRESULT initialize(ID3D12Device *device, ID3D12CommandQueue *queue,
                       const D3D12_RESOURCE_DESC &source, DXGI_FORMAT view_format,
                       UINT scale = 100, bool camera = false) noexcept;
    // S_FALSE means the pair was not generated and no output was written. The
    // list may still hold NGX history work, so call submitted() for any list
    // that is executed after record(), whatever it returned.
    HRESULT record(ID3D12GraphicsCommandList *list, UINT work_slot, ID3D12Resource *a,
                   ID3D12Resource *b, std::span<const D3D12ReprojectionView> a_views,
                   std::span<const D3D12ReprojectionView> b_views,
                   const DlssMotionVectorSet *a_guides, const DlssMotionVectorSet *b_guides,
                   std::span<const Output> outputs, D3D12_RESOURCE_STATES release_state) noexcept;
    void reset() noexcept;
    void submitted(ID3D12Fence *completion, std::uint64_t value) noexcept;
    [[nodiscard]] Skip last_skip() const noexcept;
    // Eye evaluations that reseeded NGX's history with the aligned previous frame.
    [[nodiscard]] std::uint64_t reseeds() const noexcept;
    // Native pairs whose NGX history was reset and reseeded with the aligned
    // previous frame: under a head turn every pair, with a still head only
    // where the guides' sequence broke.
    [[nodiscard]] std::uint64_t reseeded_pairs() const noexcept;
    // Tests: an eye's camera motion between the two frames, handed to NGX in
    // place of none - the clip-space transform taking B's camera to A's, as
    // sixteen floats in DirectXMath's row-vector, row-major order. nullptr
    // clears it. Applies to a feature's first eye, so one eye per feature.
    // rigid, given, is the same motion as a rotation (x, y, z, w) and a
    // translation (x, y, z) taking B's camera space to the aligned A's, in
    // the depth's units: the pack then takes the camera's own flow out of
    // the vectors and NGX is told they carry object motion alone, as its
    // contract has it, and adds the camera's back from the matrices.
    void set_test_camera_motion(UINT eye, const float *clip_to_previous,
                                const float *rigid = nullptr) noexcept;
    // The last camera motion the GPU fitted for an eye and accepted: its
    // angle in degrees, its translation in the depth's units, the median
    // residual in eye pixels, the inliers and the points fitted.
    struct CameraFit {
        float degrees{};
        std::array<float, 3> translation{};
        float residual_px{};
        UINT inliers{}, points{};
    };
    [[nodiscard]] std::optional<CameraFit> last_camera_fit(UINT eye) const noexcept;
    // Reads back the fits whose submissions have completed, as the next
    // record would, so a test can see the last pair's own fit.
    void poll_camera_fits() noexcept;
    // Fits accepted and refused since initialize.
    [[nodiscard]] std::uint64_t camera_fits_used() const noexcept;
    [[nodiscard]] std::uint64_t camera_fits_rejected() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Frames NGX can generate between two rendered frames on this device's
// adapter: 0 when native frame generation is unavailable, 1 for 2X only, and
// 2 or more when 3X is possible. The first call per adapter queries NGX.
[[nodiscard]] std::uint32_t native_dlssg_max_generated_frames(ID3D12Device *device) noexcept;

} // namespace xrfg
