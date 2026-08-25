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

void append_u16(std::vector<uint8_t>& data, uint16_t value)
{
	data.push_back(static_cast<uint8_t>(value & 0xff));
	data.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void append_u32(std::vector<uint8_t>& data, uint32_t value)
{
	for (int shift = 0; shift < 32; shift += 8)
		data.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
}

void append_name(std::vector<uint8_t>& data, const char* name)
{
	std::array<char, 20> field{};
	const size_t length = (std::min)(std::char_traits<char>::length(name), field.size());
	std::copy_n(name, length, field.begin());
	data.insert(data.end(), field.begin(), field.end());
}

void append_chunk(std::vector<uint8_t>& destination, const char id[5],
	const std::vector<uint8_t>& payload)
{
	destination.insert(destination.end(), id, id + 4);
	append_u32(destination, static_cast<uint32_t>(payload.size()));
	destination.insert(destination.end(), payload.begin(), payload.end());
	if (payload.size() & 1)
		destination.push_back(0);
}

void append_phdr(std::vector<uint8_t>& data, const char* name, uint16_t program,
	uint16_t bank, uint16_t bag_index)
{
	append_name(data, name);
	append_u16(data, program);
	append_u16(data, bank);
	append_u16(data, bag_index);
	append_u32(data, 0);
	append_u32(data, 0);
	append_u32(data, 0);
}

void append_bag(std::vector<uint8_t>& data, uint16_t generator_index)
{
	append_u16(data, generator_index);
	append_u16(data, 0);
}

void append_gen(std::vector<uint8_t>& data, uint16_t oper, int16_t amount)
{
	append_u16(data, oper);
	append_u16(data, static_cast<uint16_t>(amount));
}

void append_range_gen(std::vector<uint8_t>& data, uint16_t oper, uint8_t lo, uint8_t hi)
{
	append_u16(data, oper);
	append_u16(data, static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8)));
}

void append_inst(std::vector<uint8_t>& data, const char* name, uint16_t bag_index)
{
	append_name(data, name);
	append_u16(data, bag_index);
}

void append_shdr(std::vector<uint8_t>& data, const char* name, uint32_t start,
	uint32_t end, uint16_t link, uint16_t type)
{
	append_name(data, name);
	append_u32(data, start);
	append_u32(data, end);
	append_u32(data, start);
	append_u32(data, end);
	append_u32(data, 1000);
	data.push_back(60);
	data.push_back(0);
	append_u16(data, link);
	append_u16(data, type);
}

void write_sf2_fixture(const std::filesystem::path& path)
{
	// Generator IDs used by this deliberately small fixture.
	constexpr uint16_t pan = 17;
	constexpr uint16_t instrument = 41;
	constexpr uint16_t key_range = 43;
	constexpr uint16_t velocity_range = 44;
	constexpr uint16_t fine_tune = 52;
	constexpr uint16_t sample_id = 53;

	std::vector<uint8_t> smpl;
	const std::array<int16_t, 16> samples = {
		1000, 2000, 3000, 4000, -1000, -2000, -3000, -4000,
		6000, 6000, 6000, 6000, 12000, 12000, 12000, 12000};
	for (int16_t sample : samples)
		append_u16(smpl, static_cast<uint16_t>(sample));

	std::vector<uint8_t> phdr;
	append_phdr(phdr, "Layered", 0, 0, 0);
	append_phdr(phdr, "Other", 1, 0, 1);
	append_phdr(phdr, "EOP", 0, 0, 2);
	std::vector<uint8_t> pbag;
	append_bag(pbag, 0); append_bag(pbag, 3); append_bag(pbag, 4);
	std::vector<uint8_t> pgen;
	append_range_gen(pgen, key_range, 50, 70);
	append_gen(pgen, fine_tune, 10);
	append_gen(pgen, instrument, 0);
	append_gen(pgen, instrument, 1);

	std::vector<uint8_t> inst;
	append_inst(inst, "Layers", 0);
	append_inst(inst, "OtherInst", 3);
	append_inst(inst, "EOI", 4);
	std::vector<uint8_t> ibag;
	append_bag(ibag, 0); append_bag(ibag, 5); append_bag(ibag, 10);
	append_bag(ibag, 14); append_bag(ibag, 15);
	std::vector<uint8_t> igen;
	append_range_gen(igen, key_range, 60, 80);
	append_range_gen(igen, velocity_range, 0, 63);
	append_gen(igen, pan, -500);
	append_gen(igen, fine_tune, 20);
	append_gen(igen, sample_id, 0);
	append_range_gen(igen, key_range, 60, 80);
	append_range_gen(igen, velocity_range, 0, 63);
	append_gen(igen, pan, 500);
	append_gen(igen, fine_tune, 20);
	append_gen(igen, sample_id, 1);
	append_range_gen(igen, key_range, 60, 80);
	append_range_gen(igen, velocity_range, 64, 127);
	append_gen(igen, fine_tune, 20);
	append_gen(igen, sample_id, 2);
	append_gen(igen, sample_id, 3);

	std::vector<uint8_t> shdr;
	append_shdr(shdr, "Left", 0, 4, 1, 4);
	append_shdr(shdr, "Right", 4, 8, 0, 2);
	append_shdr(shdr, "High", 8, 12, 0, 1);
	append_shdr(shdr, "Other", 12, 16, 0, 1);
	append_shdr(shdr, "EOS", 16, 16, 0, 1);

	std::vector<uint8_t> sdta = {'s', 'd', 't', 'a'};
	append_chunk(sdta, "smpl", smpl);
	std::vector<uint8_t> pdta = {'p', 'd', 't', 'a'};
	append_chunk(pdta, "phdr", phdr);
	append_chunk(pdta, "pbag", pbag);
	append_chunk(pdta, "pgen", pgen);
	append_chunk(pdta, "inst", inst);
	append_chunk(pdta, "ibag", ibag);
	append_chunk(pdta, "igen", igen);
	append_chunk(pdta, "shdr", shdr);

	std::vector<uint8_t> riff = {'s', 'f', 'b', 'k'};
	append_chunk(riff, "LIST", sdta);
	append_chunk(riff, "LIST", pdta);
	std::ofstream stream(path, std::ios::binary);
	stream.write("RIFF", 4);
	write_u32(stream, static_cast<uint32_t>(riff.size()));
	stream.write(reinterpret_cast<const char*>(riff.data()),
		static_cast<std::streamsize>(riff.size()));
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

void test_channel_preset_selection_and_stress_mode()
{
	safsyn::Soundfont bank;
	bank.sfz_pcm.emplace_back(32, 16384);
	safsyn::SampleRegion region;
	region.pcm = bank.sfz_pcm.back().data();
	region.pcm_len = 32;
	region.sample_rate = 1000;
	region.attack = 0.0f;
	region.decay = 0.0f;
	region.sustain = 1.0f;
	region.release = 0.0f;
	bank.regions.push_back(region);
	region.preset_program = 1;
	bank.regions.push_back(region);
	region.preset_program = 0;
	region.preset_bank = 130;
	bank.regions.push_back(region);

	safsyn::SynthEngine engine(1000, 8);
	engine.set_soundfont(&bank);
	engine.note_on(0, 60, 100);
	check(engine.stats().started_voices == 1, "default bank/program excludes unrelated presets");
	engine.reset();
	engine.consume_short_message(0x000001c0); // channel 0, program 1
	engine.note_on(0, 60, 100);
	check(engine.stats().started_voices == 1, "packed MIDI program change selects another preset");
	engine.reset();
	engine.control_change(0, 0, 1);
	engine.control_change(0, 32, 2);
	engine.note_on(0, 60, 100);
	check(engine.stats().started_voices == 1, "MIDI bank MSB/LSB selects bank 130");
	engine.reset();
	engine.set_all_regions_mode(true);
	engine.note_on(0, 60, 100);
	check(engine.stats().started_voices == 3,
		"explicit all-regions mode retains the flattened stress behavior");
}

void test_sf2_preset_loader(const std::filesystem::path& directory)
{
	std::filesystem::create_directories(directory);
	const auto sf2_path = directory / "preset-fixture.sf2";
	write_sf2_fixture(sf2_path);
	safsyn::Soundfont bank;
	check(safsyn::load_sf2(sf2_path.string().c_str(), bank), "SF2 preset fixture loads");
	check(bank.presets.size() == 2 && bank.presets[0].bank == 0 &&
		bank.presets[0].program == 0 && bank.presets[1].program == 1,
		"SF2 phdr bank/program identity is preserved");
	check(bank.regions.size() == 3,
		"linked stereo headers collapse to one logical region");
	check(bank.stress_regions.size() == 3 && bank.stress_regions[0].channels == 1,
		"SF2 loader retains a separate mono seed-loader stress view");
	if (bank.regions.size() != 3)
		return;

	const auto low = std::find_if(bank.regions.begin(), bank.regions.end(),
		[](const safsyn::SampleRegion& item) {
			return item.preset_program == 0 && item.lo_vel == 0;
		});
	const auto high = std::find_if(bank.regions.begin(), bank.regions.end(),
		[](const safsyn::SampleRegion& item) {
			return item.preset_program == 0 && item.lo_vel == 64;
		});
	check(low != bank.regions.end() && high != bank.regions.end(),
		"SF2 velocity layers remain distinct");
	if (low == bank.regions.end() || high == bank.regions.end())
		return;
	check(low->lo_key == 60 && low->hi_key == 70,
		"preset and instrument key ranges are intersected");
	check(low->fine_tune == 30,
		"preset and instrument tuning generators are combined");
	check(low->channels == 2 && low->pcm_len == 4 && low->pcm_right &&
		low->pcm[0] == 1000 && low->pcm_right[0] == -1000,
		"linked left/right samples are reconstructed as one planar stereo region");

	safsyn::SynthEngine selected(1000, 8);
	selected.set_soundfont(&bank);
	selected.note_on(0, 59, 40);
	check(selected.stats().started_voices == 0, "notes outside the intersected key range do not trigger");
	selected.note_on(0, 60, 40);
	check(selected.stats().started_voices == 1,
		"low velocity triggers one stereo layer from the selected preset");
	std::array<float, 2> stereo{};
	selected.render_audio(stereo.data(), 1);
	check(stereo[0] > 0.0f && stereo[1] < 0.0f,
		"one logical SF2 stereo voice renders independent left and right samples");

	safsyn::SynthEngine high_velocity(1000, 8);
	high_velocity.set_soundfont(&bank);
	high_velocity.note_on(0, 60, 100);
	check(high_velocity.stats().started_voices == 1,
		"high velocity selects only the expected layer");

	safsyn::SynthEngine other_preset(1000, 8);
	other_preset.set_soundfont(&bank);
	other_preset.program_change(0, 1);
	other_preset.note_on(0, 60, 100);
	check(other_preset.stats().started_voices == 1,
		"program change triggers the other preset without layering preset zero");

	safsyn::SynthEngine first(1000, 8), second(1000, 8);
	first.set_soundfont(&bank);
	second.set_soundfont(&bank);
	first.note_on(0, 60, 40);
	second.note_on(0, 60, 40);
	std::vector<float> a(16), b(16);
	first.render_audio(a.data(), 8);
	second.render_audio(b.data(), 3);
	second.render_audio(b.data() + 6, 5);
	check(a == b, "selected-preset SF2 renders are deterministic across block sizes");
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
	test_channel_preset_selection_and_stress_mode();
	test_sf2_preset_loader(test_directory);
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
