#include "mastering.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

int main()
{
	constexpr uint32_t frames = 960000;
	constexpr uint32_t block_frames = 256;
	constexpr int repetitions = 7;
	std::cout << "pattern,gain_db,limiter,lookahead_ms,frames,repetitions,median_ms,audio_checksum,limited_frames,minimum_gain\n";
	for (const char* pattern : {"random", "descending", "ties"})
	{
		std::vector<float> input(static_cast<size_t>(frames) * 2);
		uint32_t random = 42;
		for (uint32_t frame = 0; frame < frames; ++frame)
		{
			random = random * 1664525u + 1013904223u;
			const float sample = pattern[0] == 'r' ?
				static_cast<float>(random >> 8) / 2796202.5f - 3.0f :
				pattern[0] == 'd' ? 1.5f - static_cast<float>(frame % 8192) / 8192.0f : 0.75f;
			input[static_cast<size_t>(frame) * 2] = sample;
			input[static_cast<size_t>(frame) * 2 + 1] = -sample * 0.5f;
		}
		input[0] = 0.0f;
		input[1] = -0.0f;
		for (double lookahead_ms : {-2.0, -1.0, 0.0, 5.0, 100.0})
		{
			safsyn::MasteringSettings settings;
			settings.output_gain_db = lookahead_ms == -1.0 ? 3.0 : 0.0;
			settings.limiter_enabled = lookahead_ms >= 0.0;
			settings.limiter_lookahead_ms = (std::max)(0.0, lookahead_ms);
			std::vector<double> elapsed;
			uint64_t checksum = 0;
			safsyn::MasteringStats stats;
			for (int repetition = -1; repetition < repetitions; ++repetition)
			{
				safsyn::StereoMasteringProcessor processor(48000, settings);
				std::vector<float> output;
				output.reserve(input.size());
				const auto start = std::chrono::steady_clock::now();
				for (uint32_t cursor = 0; cursor < frames; cursor += block_frames)
					processor.process(input.data() + static_cast<size_t>(cursor) * 2,
						(std::min)(block_frames, frames - cursor), output);
				processor.finish(output);
				const auto end = std::chrono::steady_clock::now();
				if (repetition >= 0)
					elapsed.push_back(std::chrono::duration<double, std::milli>(end - start).count());
				uint64_t hash = 1469598103934665603ull;
				for (float sample : output)
				{
					hash ^= std::bit_cast<uint32_t>(sample);
					hash *= 1099511628211ull;
				}
				if (repetition >= 0 && (hash != checksum || output.size() != input.size()))
					return 1;
				checksum = hash;
				stats = processor.stats();
			}
			std::sort(elapsed.begin(), elapsed.end());
			std::cout << pattern << ',' << settings.output_gain_db << ',' << settings.limiter_enabled << ','
				<< settings.limiter_lookahead_ms << ',' << frames << ',' << repetitions << ','
				<< std::fixed << std::setprecision(3) << elapsed[elapsed.size() / 2] << ','
				<< checksum << ',' << stats.limited_frames << ','
				<< std::setprecision(17) << stats.minimum_limiter_gain << '\n';
		}
	}
}
