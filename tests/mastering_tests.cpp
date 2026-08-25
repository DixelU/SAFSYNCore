#include "mastering.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace
{
int failures = 0;

void check(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << '\n';
		++failures;
	}
}

std::vector<float> process(const std::vector<float>& input,
	const safsyn::MasteringSettings& settings, uint32_t block_frames)
{
	safsyn::StereoMasteringProcessor processor(1000, settings);
	std::vector<float> output;
	output.reserve(input.size());
	uint32_t cursor = 0;
	const uint32_t frames = static_cast<uint32_t>(input.size() / 2);
	while (cursor < frames)
	{
		const uint32_t count = (std::min)(block_frames, frames - cursor);
		processor.process(input.data() + static_cast<size_t>(cursor) * 2, count, output);
		cursor += count;
	}
	processor.finish(output);
	return output;
}

void test_identity_and_gain()
{
	const std::vector<float> input = {0.25f, -0.5f, 1.0f, -0.125f, -0.75f, 0.75f};
	check(process(input, {}, 2) == input,
		"inactive mastering processor is bit-identical");
	safsyn::MasteringSettings gain;
	gain.output_gain_db = -6.020599913279624;
	const auto output = process(input, gain, 1);
	bool half = output.size() == input.size();
	for (size_t index = 0; index < input.size() && half; ++index)
		half = std::abs(output[index] - input[index] * 0.5f) < 1e-7f;
	check(half, "output gain is applied before optional limiting");
}

void test_lookahead_limiter()
{
	std::vector<float> input(40 * 2, 0.25f);
	input[20 * 2] = 2.0f;
	input[20 * 2 + 1] = -1.0f;
	safsyn::MasteringSettings settings;
	settings.limiter_enabled = true;
	settings.limiter_ceiling_db = -6.020599913279624;
	settings.limiter_lookahead_ms = 10.0;
	settings.limiter_release_ms = 20.0;
	const auto single = process(input, settings, 1);
	const auto blocked = process(input, settings, 13);
	check(single == blocked && single.size() == input.size(),
		"lookahead limiting is block-invariant and frame-count preserving");
	float peak = 0.0f;
	for (float sample : single)
		peak = (std::max)(peak, std::abs(sample));
	check(peak <= 0.500001f, "limiter holds stereo sample peaks under its ceiling");
	check(std::abs(single[9 * 2] - 0.25f) < 1e-7f && single[10 * 2] < 0.25f,
		"gain reduction begins exactly one lookahead window before the burst");
	check(std::abs(single[20 * 2] - 0.5f) < 1e-6f &&
		std::abs(single[20 * 2 + 1] + 0.25f) < 1e-6f,
		"one linked gain preserves the burst's stereo ratio");
	check(single[21 * 2] < 0.25f && single.back() > single[21 * 2],
		"limiter gain recovers smoothly after the burst");

	safsyn::StereoMasteringProcessor processor(1000, settings);
	std::vector<float> measured;
	processor.process(input.data(), 40, measured);
	processor.finish(measured);
	check(processor.stats().lookahead_frames == 10 &&
		processor.stats().limited_frames != 0 &&
		processor.stats().maximum_gain_reduction_db > 11.9,
		"limiter reports lookahead and gain-reduction evidence");
}

void test_settings_validation()
{
	safsyn::MasteringSettings settings;
	check(safsyn::mastering_settings_valid(settings), "default mastering settings are valid");
	settings.limiter_ceiling_db = 1.0;
	check(!safsyn::mastering_settings_valid(settings), "positive limiter ceilings are rejected");
}
}

int main()
{
	test_identity_and_gain();
	test_lookahead_limiter();
	test_settings_validation();
	if (failures != 0)
	{
		std::cerr << failures << " mastering test(s) failed\n";
		return 1;
	}
	std::cout << "All mastering tests passed\n";
	return 0;
}
