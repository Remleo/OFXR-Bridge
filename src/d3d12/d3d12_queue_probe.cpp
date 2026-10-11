#include "xrfg/d3d12_queue_probe.hpp"

#include <windows.h>

namespace xrfg {

HRESULT D3D12QueueProbe::initialize(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept {
    try {
        if (device == nullptr || queue == nullptr) {
            return E_INVALIDARG;
        }
        device_ = device;
        queue_ = queue;
        const D3D12_COMMAND_QUEUE_DESC queue_description = queue->GetDesc();
        if (queue_description.Type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
            queue_description.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE) {
            return E_NOTIMPL;
        }
        HRESULT result = queue->GetTimestampFrequency(&gpu_frequency_);
        if (FAILED(result) || gpu_frequency_ == 0) {
            return FAILED(result) ? result : E_FAIL;
        }
        LARGE_INTEGER qpc_hz{};
        if (!QueryPerformanceFrequency(&qpc_hz) || qpc_hz.QuadPart <= 0) {
            return E_FAIL;
        }
        qpc_frequency_ = qpc_hz.QuadPart;
        if (!calibrate()) {
            return E_FAIL;
        }
        D3D12_QUERY_HEAP_DESC heap_description{};
        heap_description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        heap_description.Count = kSlots;
        heap_description.NodeMask = queue_description.NodeMask;
        result = device->CreateQueryHeap(&heap_description, IID_PPV_ARGS(heap_.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        D3D12_HEAP_PROPERTIES readback_heap{};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        readback_heap.CreationNodeMask = queue_description.NodeMask;
        readback_heap.VisibleNodeMask = queue_description.NodeMask;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = static_cast<UINT64>(kSlots) * sizeof(std::uint64_t);
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        result = device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback_.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        // Readback memory stays mapped; each slot is read only once its fence
        // has passed.
        void* mapped = nullptr;
        const D3D12_RANGE whole{0, static_cast<SIZE_T>(buffer.Width)};
        result = readback_->Map(0, &whole, &mapped);
        if (FAILED(result)) {
            return result;
        }
        answers_ = static_cast<const std::uint64_t*>(mapped);
        result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        for (Slot& slot : slots_) {
            result = device->CreateCommandAllocator(queue_description.Type,
                IID_PPV_ARGS(slot.allocator.GetAddressOf()));
            if (FAILED(result)) {
                return result;
            }
            result = device->CreateCommandList(queue_description.NodeMask, queue_description.Type,
                slot.allocator.Get(), nullptr, IID_PPV_ARGS(slot.list.GetAddressOf()));
            if (FAILED(result)) {
                return result;
            }
            result = slot.list->Close();
            if (FAILED(result)) {
                return result;
            }
        }
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

bool D3D12QueueProbe::calibrate() noexcept {
    std::uint64_t gpu = 0, qpc = 0;
    if (FAILED(queue_->GetClockCalibration(&gpu, &qpc)) || gpu == 0 || qpc == 0) {
        return false;
    }
    calibration_gpu_ = gpu;
    calibration_qpc_ = qpc;
    calibrated_qpc_ = static_cast<std::int64_t>(qpc);
    return true;
}

void D3D12QueueProbe::collect() noexcept {
    const std::uint64_t completed = fence_->GetCompletedValue();
    for (Slot& slot : slots_) {
        if (!slot.pending || slot.fence_value > completed) {
            continue;
        }
        slot.pending = false;
        const std::uint64_t answer = answers_[&slot - slots_.data()];
        if (answer < calibration_gpu_) {
            continue;
        }
        // Both as nanoseconds since the calibration point.
        const double gpu_ns = static_cast<double>(answer - calibration_gpu_) * 1.0e9 /
            static_cast<double>(gpu_frequency_);
        const double cpu_ns = static_cast<double>(slot.submitted_qpc - static_cast<std::int64_t>(calibration_qpc_)) *
            1.0e9 / static_cast<double>(qpc_frequency_);
        probes_.fetch_add(1, std::memory_order_relaxed);
        if (gpu_ns - cpu_ns > static_cast<double>(kIdleNs)) {
            busy_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void D3D12QueueProbe::probe() noexcept {
    try {
        if (!fence_) {
            return;
        }
        collect();
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        // The two clocks drift; a fresh calibration every second keeps the
        // comparison to well under the idle threshold.
        if (now.QuadPart - calibrated_qpc_ > qpc_frequency_) {
            static_cast<void>(calibrate());
        }
        Slot& slot = slots_[next_];
        if (slot.pending) {
            return;
        }
        if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr))) {
            return;
        }
        slot.list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, next_);
        slot.list->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, next_, 1, readback_.Get(),
            static_cast<UINT64>(next_) * sizeof(std::uint64_t));
        if (FAILED(slot.list->Close())) {
            return;
        }
        ID3D12CommandList* lists[] = {slot.list.Get()};
        QueryPerformanceCounter(&now);
        slot.submitted_qpc = now.QuadPart;
        queue_->ExecuteCommandLists(1, lists);
        slot.fence_value = ++fence_next_;
        if (FAILED(queue_->Signal(fence_.Get(), slot.fence_value))) {
            return;
        }
        slot.pending = true;
        next_ = (next_ + 1) % kSlots;
    } catch (...) {
    }
}

} // namespace xrfg
