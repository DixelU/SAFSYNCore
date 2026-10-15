#include "core.h"

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_set>

namespace
{
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point start)
{
	return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void make_synthetic(safsyn::Soundfont& bank, bool large)
{
	// Include odd/even Bluestein lengths, direct radix-2 loops, stereo, unlooped
	// samples, and multiple samples sharing the same transform dimensions.
	for (uint32_t index = 0; index < (large ? 8U : 24U); ++index)
	{
		const uint32_t frames = (large ? 1048576 : 32768) + (index % 4) * 8191;
		const uint8_t channels = index % 3 == 0 ? 2 : 1;
		bank.sfz_pcm.emplace_back(static_cast<size_t>(frames) * channels);
		auto& pcm = bank.sfz_pcm.back();
		for (size_t offset = 0; offset < pcm.size(); ++offset)
			pcm[offset] = static_cast<int16_t>(static_cast<int32_t>(
				(offset * 1664525ULL + index * 1013904223ULL) % 60001) - 30000);
		safsyn::SampleRegion region;
		region.pcm = pcm.data();
		region.pcm_len = frames;
		region.channels = channels;
		region.logical_sample_id = index + 1;
		region.sample_rate = 48000;
		if (index % 4 != 0)
		{
			region.loop_mode = safsyn::LoopMode::Forward;
			region.loop_start = index % 4 == 1 ? 0 : 101;
			region.loop_end = index % 4 == 1 ? 32768 : frames - 17;
		}
		bank.regions.push_back(region);
	}
}

uint64_t cache_hash(safsyn::PhaseProcessor& processor, const safsyn::Soundfont& bank)
{
	uint64_t hash = 14695981039346656037ULL;
	std::unordered_set<const float*> seen;
	for (size_t id = 0; id < bank.regions.size(); ++id)
	{
		const auto& region = bank.regions[id];
		const auto state = processor.assign(region, id, 37, 0, 60);
		for (const auto* data : {state.quadrature_left, state.quadrature_right})
		{
			if (!data || !seen.insert(data).second) continue;
			for (uint32_t index = 0; index < region.pcm_len; ++index)
			{
				hash ^= std::bit_cast<uint32_t>(data[index]);
				hash *= 1099511628211ULL;
			}
		}
		for (const float value : {state.cosine, state.sine, state.scale_left, state.scale_right})
		{
			hash ^= std::bit_cast<uint32_t>(value);
			hash *= 1099511628211ULL;
		}
	}
	return hash;
}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 2 || argc > 4)
	{
		std::cerr << "usage: safsyn-preparation-benchmark <bank.sf2|bank.sfz|--synthetic|--synthetic-large> [threads=0] [repeats=3]\n";
		return 2;
	}
	try
	{
		const std::string path = argv[1];
		const uint32_t threads = argc > 2 ? static_cast<uint32_t>(std::stoul(argv[2])) : 0;
		const unsigned repeats = argc > 3 ? static_cast<unsigned>(std::stoul(argv[3])) : 3;
		safsyn::Soundfont bank;
		const auto load_start = Clock::now();
		if (path == "--synthetic" || path == "--synthetic-large")
			make_synthetic(bank, path == "--synthetic-large");
		else if (!(path.ends_with(".sfz") ? safsyn::load_sfz(path.c_str(), bank) :
			safsyn::load_sf2(path.c_str(), bank)))
		{
			std::cerr << "Could not load soundfont\n";
			return 1;
		}
		std::cerr << "load_ms=" << milliseconds(load_start) << ", regions=" << bank.regions.size() << '\n';
		std::cout << "threads,repeat,prepare_ms,warm_ms,unique_samples,cache_bytes,quadrature_hash\n";
		for (unsigned repeat = 0; repeat < repeats; ++repeat)
		{
			safsyn::PhaseProcessor processor;
			safsyn::PhaseSettings settings;
			settings.mode = safsyn::PhaseMode::Analytic;
			processor.configure(settings);
			safsyn::PhasePreparationOptions options;
			options.threads = threads;
			const auto started = Clock::now();
			if (!processor.prepare(bank.regions, options)) return 1;
			const double prepare_ms = milliseconds(started);
			const auto warm_started = Clock::now();
			if (!processor.prepare(bank.regions, options)) return 1;
			const double warm_ms = milliseconds(warm_started);
			const auto stats = processor.stats();
			std::cout << threads << ',' << repeat << ',' << std::fixed << std::setprecision(3)
				<< prepare_ms << ',' << warm_ms << ',' << stats.analytic_samples << ','
				<< stats.cache_bytes << ',' << cache_hash(processor, bank) << '\n';
		}
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
