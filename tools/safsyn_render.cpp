#include "core.h"
#include "wav_writer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
struct Event
{
	uint64_t frame;
	bool note_on;
	uint8_t note;
	uint8_t velocity;
};

safsyn::Soundfont make_demo_soundfont(uint32_t sample_rate)
{
	safsyn::Soundfont soundfont;
	const uint32_t frames = sample_rate;
	soundfont.sfz_pcm.emplace_back(frames);
	auto& pcm = soundfont.sfz_pcm.back();
	for (uint32_t frame = 0; frame < frames; ++frame)
	{
		const double phase = 2.0 * 3.14159265358979323846 * 440.0 * frame / sample_rate;
		pcm[frame] = static_cast<int16_t>(std::sin(phase) * 16384.0);
	}
	safsyn::SampleRegion region;
	region.pcm = pcm.data();
	region.pcm_len = frames;
	region.sample_rate = sample_rate;
	region.root_key = 69;
	region.loop_mode = safsyn::LoopMode::Forward;
	region.loop_start = 0;
	region.loop_end = frames;
	region.attack = 0.005f;
	region.decay = 0.08f;
	region.sustain = 0.72f;
	region.release = 0.18f;
	soundfont.regions.push_back(region);
	return soundfont;
}

std::vector<Event> make_script(uint32_t sample_rate)
{
	auto at = [sample_rate](double seconds) {
		return static_cast<uint64_t>(std::llround(seconds * sample_rate));
	};
	return {
		{at(0.00), true, 60, 104}, {at(0.00), true, 64, 96},
		{at(0.00), true, 67, 100}, {at(0.75), false, 60, 0},
		{at(0.75), false, 64, 0}, {at(0.75), false, 67, 0},
		{at(1.00), true, 62, 104}, {at(1.00), true, 65, 96},
		{at(1.00), true, 69, 100}, {at(1.75), false, 62, 0},
		{at(1.75), false, 65, 0}, {at(1.75), false, 69, 0},
		{at(2.00), true, 59, 104}, {at(2.00), true, 62, 96},
		{at(2.00), true, 67, 100}, {at(2.75), false, 59, 0},
		{at(2.75), false, 62, 0}, {at(2.75), false, 67, 0}
	};
}

void print_usage()
{
	std::cerr << "Usage:\n"
		"  safsyn-render --demo output.wav [--sample-rate N] [--voices N]\n"
		"  safsyn-render bank.sfz|bank.sf2 output.wav [--bank N] [--program N]\n"
		"      [--sample-rate N] [--voices N] [--all-regions]\n";
}
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		print_usage();
		return 2;
	}

	const bool demo = std::string(argv[1]) == "--demo";
	const std::string bank_path = demo ? std::string{} : argv[1];
	const std::string output_path = argv[2];
	uint32_t sample_rate = 48000;
	size_t voice_capacity = 256;
	uint32_t bank = 0;
	uint32_t program = 0;
	bool all_regions = false;
	for (int index = 3; index < argc; ++index)
	{
		const std::string option = argv[index];
		if (option == "--sample-rate" && index + 1 < argc)
			sample_rate = static_cast<uint32_t>(std::stoul(argv[++index]));
		else if (option == "--voices" && index + 1 < argc)
			voice_capacity = static_cast<size_t>(std::stoull(argv[++index]));
		else if (option == "--bank" && index + 1 < argc)
			bank = static_cast<uint32_t>(std::stoul(argv[++index]));
		else if (option == "--program" && index + 1 < argc)
			program = static_cast<uint32_t>(std::stoul(argv[++index]));
		else if (option == "--all-regions")
			all_regions = true;
		else
		{
			print_usage();
			return 2;
		}
	}
	if (sample_rate < 8000 || sample_rate > 384000 || voice_capacity == 0 ||
		bank > 16383 || program > 127)
	{
		std::cerr << "Invalid sample rate or voice capacity.\n";
		return 2;
	}

	safsyn::Soundfont soundfont;
	if (demo)
		soundfont = make_demo_soundfont(sample_rate);
	else
	{
		std::string extension = std::filesystem::path(bank_path).extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(),
			[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		const bool loaded = extension == ".sfz"
			? safsyn::load_sfz(bank_path.c_str(), soundfont)
			: extension == ".sf2" && safsyn::load_sf2(bank_path.c_str(), soundfont);
		if (!loaded)
		{
			std::cerr << "Could not load sound bank: " << bank_path << '\n';
			return 1;
		}
	}

	const auto events = make_script(sample_rate);
	const uint64_t total_frames = static_cast<uint64_t>(sample_rate) * 4;
	std::vector<float> audio(static_cast<size_t>(total_frames) * 2, 0.0f);
	safsyn::SynthEngine engine(sample_rate, voice_capacity);
	engine.set_soundfont(&soundfont);
	engine.set_all_regions_mode(all_regions);
	engine.control_change(0, 0, static_cast<uint8_t>(bank >> 7));
	engine.control_change(0, 32, static_cast<uint8_t>(bank & 0x7f));
	engine.program_change(0, static_cast<uint8_t>(program));

	size_t event_index = 0;
	uint64_t cursor = 0;
	constexpr uint32_t block_size = 256;
	while (cursor < total_frames)
	{
		while (event_index < events.size() && events[event_index].frame == cursor)
		{
			const Event& event = events[event_index++];
			if (event.note_on) engine.note_on(0, event.note, event.velocity);
			else engine.note_off(0, event.note);
		}
		const uint64_t next_event = event_index < events.size()
			? events[event_index].frame : total_frames;
		const uint64_t boundary = (std::min)(total_frames,
			(std::min)(cursor + block_size, next_event));
		const uint32_t frames = static_cast<uint32_t>(boundary - cursor);
		if (frames == 0)
			continue;
		engine.render_audio(audio.data() + static_cast<size_t>(cursor) * 2, frames);
		cursor = boundary;
	}

	if (!safsyn::write_float_wav(output_path.c_str(), audio.data(), total_frames, sample_rate))
	{
		std::cerr << "Could not write output WAV: " << output_path << '\n';
		return 1;
	}
	float peak = 0.0f;
	for (float sample : audio)
		peak = (std::max)(peak, std::abs(sample));
	const auto& stats = engine.stats();
	const size_t selected_regions = all_regions && !soundfont.stress_regions.empty()
		? soundfont.stress_regions.size()
		: static_cast<size_t>(std::count_if(
			soundfont.regions.begin(), soundfont.regions.end(),
			[&](const safsyn::SampleRegion& region) {
				return all_regions ||
					(region.preset_bank == bank && region.preset_program == program);
			}));
	std::cout << "Rendered " << stats.rendered_frames << " frames to " << output_path
		<< "\npresets=" << soundfont.presets.size()
		<< " regions=" << soundfont.regions.size()
		<< " stress_regions=" << soundfont.stress_regions.size()
		<< " selected_regions=" << selected_regions
		<< " bank=" << bank
		<< " program=" << static_cast<unsigned>(program)
		<< " all_regions=" << (all_regions ? "yes" : "no")
		<< " started_voices=" << stats.started_voices
		<< " peak_active=" << stats.peak_active_voices
		<< " stolen=" << stats.stolen_voices
		<< " peak=" << peak << '\n';
	return 0;
}
