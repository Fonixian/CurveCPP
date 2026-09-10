#pragma once
#include <Include/Axodox.Graphics.h>
#include <d3d11.h>
#include <cstdint>
#include <vector>

// Two GPU-owned buffers that Axodox's buffer types cannot express, both of them here so the pattern
// and dot renderers can stop reading a count back through the CPU in the middle of a frame.
//
// Why hand-rolled rather than RWStructuredBuffer:
//
//   GpuCounter        needs its ID3D11Buffer to copy into a staging resource. GraphicsBuffer keeps
//                     that pointer protected, and its only readback path, Download(), maps with a
//                     blocking D3D11_MAP_READ - which is exactly the stall this file exists to remove.
//
//   IndirectDrawArgs  D3D11 forbids D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS together with
//                     D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, so an args buffer a compute shader can
//                     write has to be a RAW (ByteAddress) UAV, which RWStructuredBuffer never makes.

// A single uint on the GPU: bound as u<slot> for InterlockedAdd, readable as t<slot>, and mirrored
// back to the CPU a few frames late.
//
// The readback is a ring of 4-byte staging buffers, the same shape as Profiler's query ring.
// Submit() queues a GPU->GPU copy into the next slot; Fetch() maps the oldest finished slot with
// D3D11_MAP_FLAG_DO_NOT_WAIT and gives up the moment the driver answers DXGI_ERROR_WAS_STILL_DRAWING.
// Nothing here ever waits on the GPU, so value() is whatever the newest COMPLETED copy held: exact,
// but two or three frames old, and briefly absent after a reset.
//
// Because of that lag, never size a resource from value(). It is a measurement. Allocation uses
// BezierRendererBase::PatternBound(), which is computed on the CPU and needs no GPU result at all.
class GpuCounter {
public:
	explicit GpuCounter(const Axodox::Graphics::GraphicsDevice& device, uint32_t frame_latency = 3);

	GpuCounter(const GpuCounter&) = delete;
	GpuCounter& operator=(const GpuCounter&) = delete;

	// Zeroes the counter. UpdateSubresource is a one-way CPU->GPU write; it does not synchronise.
	void Reset(Axodox::Graphics::GraphicsDeviceContext* context);

	void BindUnordered(uint32_t slot, Axodox::Graphics::GraphicsDeviceContext* context);
	void BindOrdered(Axodox::Graphics::ShaderStage stage, uint32_t slot, Axodox::Graphics::GraphicsDeviceContext* context);

	// Queues a copy of the current value into the readback ring. Skipped when every slot is still in
	// flight, so a GPU that has fallen behind costs a missed sample rather than a wait.
	void Submit(Axodox::Graphics::GraphicsDeviceContext* context);

	// Takes every slot the GPU has finished with, newest wins, and stops at the first that is not
	// ready. Never blocks.
	void Fetch(Axodox::Graphics::GraphicsDeviceContext* context);

	uint32_t value() const { return last_value; }
	bool valid() const { return has_value; }

private:
	struct Slot {
		winrt::com_ptr<ID3D11Buffer> staging;
		bool pending = false;
	};

	Axodox::Graphics::GraphicsDevice device;

	winrt::com_ptr<ID3D11Buffer> counter;
	winrt::com_ptr<ID3D11UnorderedAccessView> uav;
	winrt::com_ptr<ID3D11ShaderResourceView> srv;

	std::vector<Slot> slots;
	uint32_t write_slot = 0;
	uint32_t read_slot = 0;
	uint32_t pending_count = 0;

	uint32_t last_value = 0;
	bool has_value = false;
};

// A four-uint DrawInstancedIndirect argument buffer a compute shader fills in:
// (VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation).
//
// This is what lets a draw use a count the CPU never sees. Bound as a RWByteAddressBuffer at
// u<slot> and written with a single Store4, then passed straight to DrawInstancedIndirect.
class IndirectDrawArgs {
public:
	explicit IndirectDrawArgs(const Axodox::Graphics::GraphicsDevice& device, uint32_t vertices_per_instance);

	IndirectDrawArgs(const IndirectDrawArgs&) = delete;
	IndirectDrawArgs& operator=(const IndirectDrawArgs&) = delete;

	void BindUnordered(uint32_t slot, Axodox::Graphics::GraphicsDeviceContext* context);

	ID3D11Buffer* get() const { return args.get(); }

private:
	Axodox::Graphics::GraphicsDevice device;
	winrt::com_ptr<ID3D11Buffer> args;
	winrt::com_ptr<ID3D11UnorderedAccessView> uav;
};
