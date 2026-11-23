#pragma once
#include <d3d12.h>

// What the console's device and queue have for showing frames, which the host does with a
// swap chain: present.cpp.
namespace xv::runtime::modules::d3d12x {
// The host's Direct3D 12 device (the one the title's device is a proxy of), or none.
ID3D12Device *host_device();
// the state the title's own barriers left a render target in, `unknown` if it made none
D3D12_RESOURCE_STATES tracked_state(ID3D12Resource *resource, D3D12_RESOURCE_STATES unknown);
// ID3D12CommandQueue::PresentX on the host's queue
HRESULT present_x(ID3D12CommandQueue *queue, UINT planes, const void *plane_parameters,
                  const void *present_parameters);
// ID3D12Device::SetFrameIntervalX, ScheduleFrameEventX, WaitFrameEventX, GetFrameStatisticsX
HRESULT STDMETHODCALLTYPE set_frame_interval(ID3D12Device *device, void *output, UINT microseconds,
                                             UINT period, UINT flags);
HRESULT STDMETHODCALLTYPE schedule_frame_event(ID3D12Device *device, UINT type, UINT offset,
                                               void *signals, UINT flags);
HRESULT STDMETHODCALLTYPE wait_frame_event(ID3D12Device *device, UINT type, UINT timeout,
                                           void *waits, UINT flags, UINT64 *token);
HRESULT STDMETHODCALLTYPE frame_statistics(ID3D12Device *device, UINT64 token, UINT types,
                                           UINT *count, void *statistics);
} // namespace xv::runtime::modules::d3d12x
