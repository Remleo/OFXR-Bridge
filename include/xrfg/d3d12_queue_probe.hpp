#pragma once

#include <atomic>
#include <cstdint>
#include <array>

#include <d3d12.h>
#include <wrl/client.h>

namespace xrfg {

// How busy a D3D12 queue is, sampled: each probe is one timestamp query the
// queue executes when it gets to it. Against the CPU time the probe was
// submitted at, through the queue's clock calibration, the timestamp says
// how long the queue was ahead of the GPU: a probe answered within kIdleNs
// found the GPU idle, a later one found it busy. Probed at a steady cadence
// from another thread (the layer's presenter, once a display period), the
// share of busy probes over a span is the GPU's busy share there, and that
// share of the game's frame interval is its GPU time a frame, which no
// CPU-side wait can tell for a game that is GPU-bound.
//
// probe() is for one thread at a time; counts() may be read from any.
class D3D12QueueProbe {
public:
    static constexpr std::int64_t kIdleNs = 300'000;

    [[nodiscard]] HRESULT initialize(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept;
    // Collects the probes answered since the last call, then submits one more
    // if a slot is free.
    void probe() noexcept;
    struct Counts {
        std::uint64_t probes{};
        std::uint64_t busy{};
    };
    [[nodiscard]] Counts counts() const noexcept {
        return {probes_.load(std::memory_order_relaxed), busy_.load(std::memory_order_relaxed)};
    }

private:
    static constexpr UINT kSlots = 16;
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        std::uint64_t fence_value{};
        std::int64_t submitted_qpc{};
        bool pending{};
    };
    void collect() noexcept;
    bool calibrate() noexcept;

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    const std::uint64_t* answers_{};
    std::array<Slot, kSlots> slots_{};
    UINT next_{};
    std::uint64_t fence_next_{};
    std::uint64_t gpu_frequency_{};
    std::int64_t qpc_frequency_{};
    std::uint64_t calibration_gpu_{};
    std::uint64_t calibration_qpc_{};
    std::int64_t calibrated_qpc_{};
    std::atomic<std::uint64_t> probes_{};
    std::atomic<std::uint64_t> busy_{};
};

} // namespace xrfg
