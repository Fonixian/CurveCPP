#pragma once
#include <Include/Axodox.Graphics.h>
#include <d3d11.h>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using MetricId = uint32_t;

class Profiler {
public:
	struct Metric {
		std::string name;
		double cpu = 0.0;
		double gpu = 0.0;

	private:
		friend class Profiler;
		bool cpu_seeded = false;
		bool gpu_seeded = false;
		bool cpu_open = false;
		std::chrono::steady_clock::time_point cpu_start{};
	};

	explicit Profiler(const Axodox::Graphics::GraphicsDevice& device, uint32_t frame_latency = 3, double alpha = 0.1);

	Profiler(const Profiler&) = delete;
	Profiler& operator=(const Profiler&) = delete;

	bool enabled = true;

	void begin_frame();
	void end_frame();

	MetricId id(std::string_view name);

	void begin_gpu(MetricId metric);
	void end_gpu(MetricId metric);
	void begin_cpu(MetricId metric);
	void end_cpu(MetricId metric);

	void begin_gpu(std::string_view name) { begin_gpu(id(name)); }
	void end_gpu(std::string_view name) { end_gpu(id(name)); }
	void begin_cpu(std::string_view name) { begin_cpu(id(name)); }
	void end_cpu(std::string_view name) { end_cpu(id(name)); }

	const std::vector<Metric>& metrics() const { return metric_list; }
	const Metric* find(std::string_view name) const;

	double gpu_ms(std::string_view name) const;
	double cpu_ms(std::string_view name) const;

	double alpha() const { return smoothing; }
	void alpha(double value);

	void reset();
protected:
	struct GpuTimer {
		winrt::com_ptr<ID3D11Query> start;
		winrt::com_ptr<ID3D11Query> stop;
		bool open = false;
		bool recorded = false;
	};

	struct FrameSlot {
		winrt::com_ptr<ID3D11Query> disjoint;
		std::vector<GpuTimer> timers;
		bool pending = false;
	};

	Axodox::Graphics::GraphicsDevice device;
	std::vector<Metric> metric_list;
	std::vector<FrameSlot> slots;
	uint32_t current = 0;
	bool frame_open = false;
	double smoothing = 0.1;

	bool collect(FrameSlot& slot);
	void accumulate(double& ema, bool& seeded, double sample) const;
	GpuTimer* timer(MetricId metric);
};
