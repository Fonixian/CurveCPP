#include "profiler.h"
#include <algorithm>

using namespace Axodox::Graphics;

Profiler::Profiler(const GraphicsDevice& device, uint32_t frame_latency, double alpha)
	: device(device), smoothing(std::clamp(alpha, 0.0, 1.0)) {
	frame_latency = std::max(frame_latency, 2u);

	D3D11_QUERY_DESC disjoint_desc = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
	auto d3d_device = this->device.get();

	slots.resize(frame_latency);
	for (auto& slot : slots)
		winrt::check_hresult(d3d_device->CreateQuery(&disjoint_desc, slot.disjoint.put()));
}

MetricId Profiler::id(std::string_view name) {
	for (size_t i = 0; i < metric_list.size(); i++)
		if (metric_list[i].name == name) return static_cast<MetricId>(i);

	metric_list.emplace_back();
	metric_list.back().name = name;

	for (auto& slot : slots)
		slot.timers.resize(metric_list.size());

	return static_cast<MetricId>(metric_list.size() - 1);
}

void Profiler::begin_frame() {
	if (frame_open) end_frame();
	if (!enabled) return;

	for (size_t i = 1; i <= slots.size(); i++) {
		auto& slot = slots[(current + i) % slots.size()];
		if (!slot.pending) continue;
		if (!collect(slot)) break;
	}

	auto& slot = slots[current];

	
	if (slot.pending) return;

	for (auto& slot_timer : slot.timers) {
		slot_timer.open = false;
		slot_timer.recorded = false;
	}

	device.ImmediateContext()->get()->Begin(slot.disjoint.get());
	frame_open = true;
}

void Profiler::end_frame() {
	if (!frame_open) return;

	auto& slot = slots[current];
	device.ImmediateContext()->get()->End(slot.disjoint.get());

	slot.pending = true;
	current = (current + 1u) % static_cast<uint32_t>(slots.size());
	frame_open = false;
}

Profiler::GpuTimer* Profiler::timer(MetricId metric) {
	if (!frame_open || metric >= metric_list.size()) return nullptr;

	auto& slot = slots[current];
	if (metric >= slot.timers.size()) return nullptr;

	auto& slot_timer = slot.timers[metric];
	if (!slot_timer.start) {
		D3D11_QUERY_DESC timestamp_desc = { D3D11_QUERY_TIMESTAMP, 0 };
		auto d3d_device = device.get();

		winrt::check_hresult(d3d_device->CreateQuery(&timestamp_desc, slot_timer.start.put()));
		winrt::check_hresult(d3d_device->CreateQuery(&timestamp_desc, slot_timer.stop.put()));
	}

	return &slot_timer;
}

void Profiler::begin_gpu(MetricId metric) {
	auto* slot_timer = timer(metric);
	if (!slot_timer) return;

	device.ImmediateContext()->get()->End(slot_timer->start.get());

	slot_timer->open = true;
	slot_timer->recorded = false;
}

void Profiler::end_gpu(MetricId metric) {
	auto* slot_timer = timer(metric);
	if (!slot_timer || !slot_timer->open) return;

	device.ImmediateContext()->get()->End(slot_timer->stop.get());

	slot_timer->open = false;
	slot_timer->recorded = true;
}

void Profiler::begin_cpu(MetricId metric) {
	if (!enabled || metric >= metric_list.size()) return;

	auto& entry = metric_list[metric];
	entry.cpu_start = std::chrono::steady_clock::now();
	entry.cpu_open = true;
}

void Profiler::end_cpu(MetricId metric) {
	if (metric >= metric_list.size()) return;

	auto& entry = metric_list[metric];
	if (!entry.cpu_open) return;
	entry.cpu_open = false;

	std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - entry.cpu_start;
	accumulate(entry.cpu, entry.cpu_seeded, elapsed.count());
}

bool Profiler::collect(FrameSlot& slot) {
	auto context = device.ImmediateContext()->get();

	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
	if (context->GetData(slot.disjoint.get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
		return false;

	slot.pending = false;

	if (disjoint.Disjoint || disjoint.Frequency == 0) {
		for (auto& slot_timer : slot.timers) slot_timer.recorded = false;
		return true;
	}

	for (size_t i = 0; i < slot.timers.size() && i < metric_list.size(); i++) {
		auto& slot_timer = slot.timers[i];
		if (!slot_timer.recorded) continue;
		slot_timer.recorded = false;

		uint64_t start = 0, stop = 0;
		if (context->GetData(slot_timer.start.get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
		if (context->GetData(slot_timer.stop.get(), &stop, sizeof(stop), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
		if (stop < start) continue;

		auto& entry = metric_list[i];
		accumulate(entry.gpu, entry.gpu_seeded, static_cast<double>(stop - start) * 1000.0 / static_cast<double>(disjoint.Frequency));
	}

	return true;
}

void Profiler::accumulate(double& ema, bool& seeded, double sample) const {
	if (!seeded) {
		ema = sample;
		seeded = true;
	} else {
		ema += smoothing * (sample - ema);
	}
}

const Profiler::Metric* Profiler::find(std::string_view name) const {
	for (auto& entry : metric_list)
		if (entry.name == name) return &entry;

	return nullptr;
}

double Profiler::gpu_ms(std::string_view name) const {
	auto* entry = find(name);
	return entry ? entry->gpu : 0.0;
}

double Profiler::cpu_ms(std::string_view name) const {
	auto* entry = find(name);
	return entry ? entry->cpu : 0.0;
}

void Profiler::alpha(double value) {
	smoothing = std::clamp(value, 0.0, 1.0);
}

void Profiler::reset() {
	for (auto& entry : metric_list) {
		entry.cpu = 0.0;
		entry.gpu = 0.0;
		entry.cpu_seeded = false;
		entry.gpu_seeded = false;
		entry.cpu_open = false;
	}
}
