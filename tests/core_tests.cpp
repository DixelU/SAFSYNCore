#include "core.h"
#include "wav_writer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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

void write_u16(std::ostream& stream, uint16_t value)
{
	stream.put(static_cast<char>(value & 0xff));
	stream.put(static_cast<char>((value >> 8) & 0xff));
}

void write_u32(std::ostream& stream, uint32_t value)
{
	for (int shift = 0; shift < 32; shift += 8)
		stream.put(static_cast<char>((value >> shift) & 0xff));
}

void write_pcm16_wav(const std::filesystem::path& path, const std::vector<int16_t>& pcm,
	uint32_t sample_rate)
{
	std::ofstream stream(path, std::ios::binary);
	const uint32_t data_size = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
	stream.write("RIFF", 4); write_u32(stream, 36 + data_size); stream.write("WAVE", 4);
	stream.write("fmt ", 4); write_u32(stream, 16); write_u16(stream, 1); write_u16(stream, 1);
	write_u32(stream, sample_rate); write_u32(stream, sample_rate * 2); write_u16(stream, 2);
	write_u16(stream, 16); stream.write("data", 4); write_u32(stream, data_size);
	stream.write(reinterpret_cast<const char*>(pcm.data()), data_size);
}

safsyn::Soundfont make_constant_bank(uint32_t sample_rate = 1000)
{
	safsyn::Soundfont bank;
	bank.sfz_pcm.emplace_back(128, 16384);
	safsyn::SampleRegion region;
	region.pcm = bank.sfz_pcm.back().data();
	region.pcm_len = static_cast<uint32_t>(bank.sfz_pcm.back().size());
	region.sample_rate = sample_rate;
	region.loop_mode = safsyn::LoopMode::Forward;
	region.loop_end = region.pcm_len;
	region.attack = 0.0f;
	region.decay = 0.0f;
	region.sustain = 1.0f;
	region.release = 0.004f;
	bank.regions.push_back(region);
	return bank;
}

bool any_nonzero(const std::vector<float>& audio)
{
	return std::any_of(audio.begin(), audio.end(), [](float sample) { return sample != 0.0f; });
}

void test_independent_instances()
{
	auto bank = make_constant_bank();
	safsyn::SynthEngine first(1000, 8);
	safsyn::SynthEngine second(1000, 8);
	first.set_soundfont(&bank);
	second.set_soundfont(&bank);
	first.note_on(0, 60, 127);
	std::vector<float> first_audio(16), second_audio(16);
	first.render_audio(first_audio.data(), 8);
	second.render_audio(second_audio.data(), 8);
	check(any_nonzero(first_audio), "first synth renders its note");
	check(!any_nonzero(second_audio), "second synth remains silent");
	check(first.active_voice_count() == 1 && second.active_voice_count() == 0,
		"synth voice state is instance-local");
}

void test_release_and_sustain()
{
	auto bank = make_constant_bank();
	safsyn::SynthEngine engine(1000, 8);
	engine.set_soundfont(&bank);
	std::array<float, 8> audio{};
	engine.note_on(0, 60, 127);
	engine.render_audio(audio.data(), 1);
	engine.control_change(0, 64, 127);
	engine.note_off(0, 60);
	engine.render_audio(audio.data(), 1);
	check(engine.active_voice_count() == 1, "sustain pedal defers release");
	engine.control_change(0, 64, 0);
	engine.render_audio(audio.data(), 4);
	check(engine.active_voice_count() == 0, "pedal-up completes the release envelope");
}

void test_determinism_and_block_invariance()
{
	auto bank = make_constant_bank();
	safsyn::SynthEngine whole(1000, 8), split(1000, 8);
	whole.set_soundfont(&bank);
	split.set_soundfont(&bank);
	whole.note_on(0, 64, 93);
	split.note_on(0, 64, 93);
	std::vector<float> a(128), b(128);
	whole.render_audio(a.data(), 64);
	split.render_audio(b.data(), 17);
	split.render_audio(b.data() + 34, 47);
	check(a == b, "coherent render is bit-identical across block boundaries");
}

void test_interpolation_pitch_and_stereo()
{
	safsyn::Soundfont bank;
	bank.sfz_pcm.push_back({0, 0, 32767, -32768, 0, 0, -32768, 32767});
	safsyn::SampleRegion region;
	region.pcm = bank.sfz_pcm.back().data();
	region.pcm_len = 4;
	region.channels = 2;
	region.sample_rate = 1000;
	region.root_key = 60;
	region.attack = 0.0f;
	region.decay = 0.0f;
	region.sustain = 1.0f;
	region.release = 0.0f;
	bank.regions.push_back(region);

	safsyn::SynthEngine root_note(2000, 2), octave_up(2000, 2);
	root_note.set_soundfont(&bank);
	octave_up.set_soundfont(&bank);
	root_note.note_on(0, 60, 127);
	octave_up.note_on(0, 72, 127);
	std::array<float, 8> root_audio{}, octave_audio{};
	root_note.render_audio(root_audio.data(), 4);
	octave_up.render_audio(octave_audio.data(), 4);
	check(root_audio[2] > 0.0f && root_audio[3] < 0.0f,
		"stereo channels are interpolated independently");
	check(root_audio[4] > root_audio[2], "half-rate playback uses linear interpolation");
	check(std::abs(octave_audio[2] - root_audio[4]) < 0.00001f,
		"one octave raises the playback increment by two");
}

void test_voice_stealing_and_midi_messages()
{
	auto bank = make_constant_bank();
	safsyn::SynthEngine engine(1000, 2);
	engine.set_soundfont(&bank);
	engine.consume_short_message(0x00643c90); // note 60, velocity 100
	engine.note_on(0, 62, 100);
	engine.note_on(0, 64, 100);
	check(engine.active_voice_count() == 2, "voice capacity is enforced");
	check(engine.stats().stolen_voices == 1, "oldest voice is stolen at capacity");
	engine.consume_short_message(0x00004080); // note 64 off
	std::vector<float> release(16);
	engine.render_audio(release.data(), 8);
	check(engine.active_voice_count() == 1, "packed MIDI note-off reaches the engine");
}

void test_sfz_loader(const std::filesystem::path& directory)
{
	std::filesystem::create_directories(directory);
	const auto sample_path = directory / "fixture.wav";
	const auto sfz_path = directory / "fixture.sfz";
	write_pcm16_wav(sample_path, {0, 8192, 16384, 8192, 0, -8192, -16384, -8192}, 8000);
	{
		std::ofstream sfz(sfz_path);
		sfz << "<group> ampeg_attack=0 ampeg_release=0.01\n"
			"<region> sample=fixture.wav key=60 loop_mode=loop_continuous "
			"loop_start=0 loop_end=8\n";
	}
	safsyn::Soundfont bank;
	check(safsyn::load_sfz(sfz_path.string().c_str(), bank), "SFZ fixture loads");
	check(bank.regions.size() == 1 && bank.regions[0].root_key == 60,
		"SFZ region mapping is retained");
	if (!bank.regions.empty())
	{
		safsyn::SynthEngine engine(8000, 4);
		engine.set_soundfont(&bank);
		engine.note_on(0, 60, 127);
		std::vector<float> audio(32);
		engine.render_audio(audio.data(), 16);
		check(any_nonzero(audio), "loaded SFZ region renders audio");
	}
}

void test_float_wav(const std::filesystem::path& directory)
{
	std::filesystem::create_directories(directory);
	const auto path = directory / "float.wav";
	const std::array<float, 4> samples = {0.25f, -0.25f, 0.5f, -0.5f};
	check(safsyn::write_float_wav(path.string().c_str(), samples.data(), 2, 48000),
		"float WAV writer succeeds");
	std::ifstream stream(path, std::ios::binary);
	std::array<char, 44> header{};
	stream.read(header.data(), header.size());
	check(stream.gcount() == 44, "float WAV header has canonical size");
	check(std::string(header.data(), 4) == "RIFF" && std::string(header.data() + 8, 4) == "WAVE",
		"float WAV has RIFF/WAVE identifiers");
	check(static_cast<unsigned char>(header[20]) == 3 && static_cast<unsigned char>(header[22]) == 2,
		"float WAV uses IEEE-float stereo format");
}
}

int main(int argc, char** argv)
{
	const std::filesystem::path test_directory = argc > 1 ? argv[1] : "test-data";
	test_independent_instances();
	test_release_and_sustain();
	test_determinism_and_block_invariance();
	test_interpolation_pitch_and_stereo();
	test_voice_stealing_and_midi_messages();
	test_sfz_loader(test_directory);
	test_float_wav(test_directory);
	if (failures != 0)
	{
		std::cerr << failures << " test(s) failed\n";
		return 1;
	}
	std::cout << "All coherent engine tests passed\n";
	return 0;
}
