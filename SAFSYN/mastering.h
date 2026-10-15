#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace safsyn
{

struct MasteringSettings
{
	double output_gain_db = 0.0;
	bool limiter_enabled = false;
	double limiter_ceiling_db = -0.3;
	double limiter_lookahead_ms = 5.0;
	double limiter_release_ms = 100.0;

	bool active() const noexcept { return limiter_enabled || output_gain_db != 0.0; }
};

struct MasteringStats
{
	uint64_t lookahead_frames = 0;
	uint64_t limited_frames = 0;
	double output_gain_linear = 1.0;
	double minimum_limiter_gain = 1.0;
	double maximum_gain_reduction_db = 0.0;
};

bool mastering_settings_valid(const MasteringSettings& settings) noexcept;

// Streaming stereo-linked sample-peak limiter. Output is delayed internally,
// not shifted in the file: finish() emits the remaining lookahead frames so
// the result has exactly the same frame count as the input.
class StereoMasteringProcessor
{
public:
	StereoMasteringProcessor(uint32_t sample_rate, const MasteringSettings& settings);

	void process(const float* interleaved_stereo, uint32_t frames,
		std::vector<float>& output);
	void finish(std::vector<float>& output);
	const MasteringStats& stats() const noexcept { return stats_; }

private:
	struct Frame
	{
		float left = 0.0f;
		float right = 0.0f;
		uint64_t index = 0;
	};

	void push_frame(float left, float right, std::vector<float>& output);
	void emit_oldest(std::vector<float>& output);
	void emit_frame(const Frame& frame, double peak, std::vector<float>& output);
	size_t ring_index(size_t first, size_t offset) const noexcept;

	MasteringSettings settings_;
	MasteringStats stats_;
	// Each window holds at most lookahead + 1 frames, including the new input.
	// Round storage to a power of two for inexpensive wrapping on x86 too.
	std::vector<Frame> pending_;
	std::vector<std::pair<uint64_t, double>> maximums_;
	size_t ring_mask_ = 0;
	size_t pending_first_ = 0;
	size_t pending_count_ = 0;
	size_t maximums_first_ = 0;
	size_t maximums_count_ = 0;
	double ceiling_ = 1.0;
	double limiter_gain_ = 1.0;
	double release_coefficient_ = 0.0;
	uint64_t next_index_ = 0;
	bool finished_ = false;
};

} // namespace safsyn
