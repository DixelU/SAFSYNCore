#include "core.h"

#include <chrono>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

safsyn::Soundfont make_bank()
{
	safsyn::Soundfont bank;
	bank.sfz_pcm.emplace_back(1024, 16384);
	safsyn::SampleRegion region;
	region.pcm = bank.sfz_pcm.back().data();
	region.pcm_len = static_cast<uint32_t>(bank.sfz_pcm.back().size());
	region.sample_rate = 48000;
	region.loop_mode = safsyn::LoopMode::Forward;
	region.loop_end = region.pcm_len;
	region.attack = 0.0f;
	region.decay = 0.0f;
	region.sustain = 1.0f;
	region.release = 0.05f;
	bank.regions.push_back(region);
	return bank;
}

void start_distinct(safsyn::SynthEngine& engine, uint64_t ordinal, uint8_t channel)
{
	engine.control_change(channel, 1, static_cast<uint8_t>(ordinal & 0x7f));
	engine.note_on(channel, static_cast<uint8_t>(ordinal & 0x7f),
		static_cast<uint8_t>(1 + ((ordinal >> 7) % 127)));
}

template<class Function>
double time_ms(Function&& function)
{
	const auto started = Clock::now();
	function();
	return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
}

// Warmed, repeated measurements of the event-to-event intervals that drive
// worker handoff costs. Keep this harness usable against the original engine.
int interval_matrix(int argc, char** argv)
{
	const size_t threads = argc > 2 ? std::stoull(argv[2]) : 4;
	const size_t repetitions = argc > 3 ? std::stoull(argv[3]) : 7;
	const size_t capacity = argc > 4 ? std::stoull(argv[4]) : 4096;
	if (!threads || threads > 64 || !repetitions || !capacity) return 1;
	std::cout << "mode,cohorts,threads,interval,median_ms_per_1024_frames,min_ms,max_ms,checksum\n";
	for (const auto mode : {"mono", "stereo", "analytic"})
	{
		auto bank = make_bank();
		const bool stereo = std::string_view(mode) == "stereo";
		auto& pcm = bank.sfz_pcm.back();
		pcm.resize(1024 * (stereo ? 2 : 1));
		for (size_t i = 0; i < pcm.size(); ++i)
			pcm[i] = static_cast<int16_t>(std::sin(i * 0.071) * 16384);
		bank.regions[0].pcm = pcm.data();
		bank.regions[0].channels = stereo ? 2 : 1;
		for (uint32_t interval : {1u, 9u, 64u, 256u, 1024u})
		{
			safsyn::SynthEngine engine(48000, capacity);
			engine.set_voice_model(safsyn::VoiceModel::Cohorts, capacity);
			engine.set_render_threads(threads);
			if (std::string_view(mode) == "analytic")
			{
				safsyn::PhaseSettings phase;
				phase.mode = safsyn::PhaseMode::Analytic;
				engine.set_phase_settings(phase);
			}
			engine.set_soundfont(&bank);
			for (uint64_t ordinal = 0; ordinal < capacity; ++ordinal)
				start_distinct(engine, ordinal, 7);
			std::vector<float> audio(2048);
			auto run = [&] {
				for (uint32_t cursor = 0; cursor < 1024; cursor += interval)
					engine.render_audio(audio.data() + cursor * 2,
						(std::min)(interval, 1024 - cursor));
			};
			run();
			run();
			std::vector<double> times;
			double checksum = 0;
			for (size_t trial = 0; trial < repetitions; ++trial)
			{
				times.push_back(time_ms(run));
				for (float sample : audio) checksum += sample;
			}
			std::sort(times.begin(), times.end());
			std::cout << mode << ',' << capacity << ',' << engine.render_threads()
				<< ',' << interval << ',' << std::fixed << std::setprecision(6)
				<< times[times.size() / 2] << ',' << times.front() << ','
				<< times.back() << ',' << checksum << '\n';
		}
	}
	return 0;
}
}

int main(int argc, char** argv)
{
	if (argc > 1 && std::string_view(argv[1]) == "--matrix")
		return interval_matrix(argc, argv);
	const uint64_t iterations = argc > 1 ? std::stoull(argv[1]) : 50000;
	const size_t render_threads = argc > 2 ? static_cast<size_t>(std::stoull(argv[2])) : 12;
	constexpr size_t capacity = 4096;
	auto bank = make_bank();

	safsyn::SynthEngine stealing(48000, capacity);
	stealing.set_voice_model(safsyn::VoiceModel::Cohorts, capacity);
	stealing.set_soundfont(&bank);
	for (uint64_t ordinal = 0; ordinal < capacity; ++ordinal)
		start_distinct(stealing, ordinal, 7);
	const double steal_ms = time_ms([&] {
		for (uint64_t ordinal = capacity; ordinal < capacity + iterations; ++ordinal)
			start_distinct(stealing, ordinal, 7);
	});

	safsyn::SynthEngine pitching(48000, capacity);
	pitching.set_voice_model(safsyn::VoiceModel::Cohorts, capacity);
	pitching.set_soundfont(&bank);
	for (uint64_t ordinal = 0; ordinal < capacity; ++ordinal)
		start_distinct(pitching, ordinal, 7);
	const double pitch_ms = time_ms([&] {
		for (uint64_t ordinal = 0; ordinal < iterations; ++ordinal)
			pitching.set_pitch_bend(7, static_cast<uint16_t>(ordinal & 0x3fff));
	});

	constexpr uint32_t render_frames = 1024;
	safsyn::SynthEngine block_render(48000, capacity);
	block_render.set_voice_model(safsyn::VoiceModel::Cohorts, capacity);
	block_render.set_soundfont(&bank);
	for (uint64_t ordinal = 0; ordinal < capacity; ++ordinal)
		start_distinct(block_render, ordinal, 7);
	std::vector<float> block_audio(static_cast<size_t>(render_frames) * 2);
	const double block_ms = time_ms([&] {
		block_render.render_audio(block_audio.data(), render_frames);
	});

	safsyn::SynthEngine single_render(48000, capacity);
	single_render.set_voice_model(safsyn::VoiceModel::Cohorts, capacity);
	single_render.set_soundfont(&bank);
	for (uint64_t ordinal = 0; ordinal < capacity; ++ordinal)
		start_distinct(single_render, ordinal, 7);
	std::array<float, 2> single_audio{};
	const double single_ms = time_ms([&] {
		for (uint32_t frame = 0; frame < render_frames; ++frame)
			single_render.render_audio(single_audio.data(), 1);
	});

	safsyn::SynthEngine threaded_render(48000, capacity);
	threaded_render.set_voice_model(safsyn::VoiceModel::Cohorts, capacity);
	threaded_render.set_render_threads(render_threads);
	threaded_render.set_soundfont(&bank);
	for (uint64_t ordinal = 0; ordinal < capacity; ++ordinal)
		start_distinct(threaded_render, ordinal, 7);
	std::vector<float> threaded_audio(static_cast<size_t>(render_frames) * 2);
	const double threaded_ms = time_ms([&] {
		threaded_render.render_audio(threaded_audio.data(), render_frames);
	});

	safsyn::SynthEngine scalar_interval_render(48000, capacity);
	scalar_interval_render.set_voice_model(safsyn::VoiceModel::Cohorts, capacity);
	scalar_interval_render.set_soundfont(&bank);
	for (uint64_t ordinal = 0; ordinal < capacity; ++ordinal)
		start_distinct(scalar_interval_render, ordinal, 7);
	std::array<float, 18> scalar_interval_audio{};
	const double scalar_interval_ms = time_ms([&] {
		for (uint32_t cursor = 0; cursor < render_frames; cursor += 9)
			scalar_interval_render.render_audio(scalar_interval_audio.data(),
				(std::min)(uint32_t{9}, render_frames - cursor));
	});

	safsyn::SynthEngine interval_render(48000, capacity);
	interval_render.set_voice_model(safsyn::VoiceModel::Cohorts, capacity);
	interval_render.set_render_threads(render_threads);
	interval_render.set_soundfont(&bank);
	for (uint64_t ordinal = 0; ordinal < capacity; ++ordinal)
		start_distinct(interval_render, ordinal, 7);
	std::array<float, 18> interval_audio{};
	const double interval_ms = time_ms([&] {
		for (uint32_t cursor = 0; cursor < render_frames; cursor += 9)
			interval_render.render_audio(interval_audio.data(),
				(std::min)(uint32_t{9}, render_frames - cursor));
	});

	std::cout << std::fixed << std::setprecision(3)
		<< "capacity=" << capacity
		<< " iterations=" << iterations
		<< " steal_ms=" << steal_ms
		<< " steal_ns_per_event=" << steal_ms * 1.0e6 / iterations
		<< " pitch_ms=" << pitch_ms
		<< " pitch_ns_per_event=" << pitch_ms * 1.0e6 / iterations
		<< " block_render_ms=" << block_ms
		<< " single_frame_render_ms=" << single_ms
		<< " threaded_render_ms=" << threaded_ms
		<< " scalar_interval9_ms=" << scalar_interval_ms
		<< " threaded_interval9_ms=" << interval_ms
		<< " render_threads=" << render_threads
		<< " block_ns_per_cohort_frame=" << block_ms * 1.0e6 /
			(static_cast<double>(capacity) * render_frames)
		<< " single_ns_per_cohort_frame=" << single_ms * 1.0e6 /
			(static_cast<double>(capacity) * render_frames)
		<< " capacity_steals=" << stealing.stats().cohort_capacity_steals
		<< " candidate_visits=" << stealing.stats().steal_candidate_visits
		<< " max_probe=" << stealing.stats().maximum_steal_probe
		<< '\n';
	return 0;
}
