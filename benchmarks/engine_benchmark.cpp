#include "core.h"

#include <chrono>
#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
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
}

int main(int argc, char** argv)
{
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
