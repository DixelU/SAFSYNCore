#include "core.h"
#include "smf.h"
#include "smf_renderer.h"
#include "wav_writer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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

void append_be16(std::vector<uint8_t>& out, uint16_t value)
{
	out.push_back(static_cast<uint8_t>(value >> 8));
	out.push_back(static_cast<uint8_t>(value));
}

void append_be32(std::vector<uint8_t>& out, uint32_t value)
{
	out.push_back(static_cast<uint8_t>(value >> 24));
	out.push_back(static_cast<uint8_t>(value >> 16));
	out.push_back(static_cast<uint8_t>(value >> 8));
	out.push_back(static_cast<uint8_t>(value));
}

void append_vlq(std::vector<uint8_t>& out, uint32_t value)
{
	uint8_t bytes[4] = {};
	size_t count = 0;
	bytes[count++] = static_cast<uint8_t>(value & 0x7fU);
	while ((value >>= 7) != 0)
		bytes[count++] = static_cast<uint8_t>((value & 0x7fU) | 0x80U);
	while (count != 0)
		out.push_back(bytes[--count]);
}

void append_eot(std::vector<uint8_t>& track, uint32_t delta = 0)
{
	append_vlq(track, delta);
	track.insert(track.end(), {0xff, 0x2f, 0x00});
}

std::vector<uint8_t> make_smf(uint16_t format, uint16_t division,
	const std::vector<std::vector<uint8_t>>& tracks)
{
	std::vector<uint8_t> file = {'M', 'T', 'h', 'd'};
	append_be32(file, 6);
	append_be16(file, format);
	append_be16(file, static_cast<uint16_t>(tracks.size()));
	append_be16(file, division);
	for (const auto& track : tracks)
	{
		file.insert(file.end(), {'M', 'T', 'r', 'k'});
		append_be32(file, static_cast<uint32_t>(track.size()));
		file.insert(file.end(), track.begin(), track.end());
	}
	return file;
}

std::vector<uint8_t> running_status_fixture()
{
	std::vector<uint8_t> track;
	track.insert(track.end(), {0x00, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20});
	track.insert(track.end(), {0x00, 0xb0, 0x00, 0x01});
	track.insert(track.end(), {0x00, 0x20, 0x02}); // running CC status
	track.insert(track.end(), {0x00, 0xc0, 0x05});
	track.insert(track.end(), {0x00, 0x90, 0x3c, 0x64});
	track.insert(track.end(), {0x00, 0x40, 0x6e}); // running note-on status
	append_vlq(track, 480);
	track.insert(track.end(), {0x3c, 0x00});
	track.insert(track.end(), {0x00, 0x40, 0x00});
	append_eot(track);
	return make_smf(0, 480, {track});
}

std::vector<uint8_t> read_file(const std::filesystem::path& path)
{
	std::ifstream stream(path, std::ios::binary | std::ios::ate);
	if (!stream) return {};
	const auto size = stream.tellg();
	if (size < 0) return {};
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	stream.seekg(0);
	if (!bytes.empty())
		stream.read(reinterpret_cast<char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
	return bytes;
}

uint32_t read_le32(const std::vector<uint8_t>& bytes, size_t offset)
{
	return static_cast<uint32_t>(bytes[offset]) |
		(static_cast<uint32_t>(bytes[offset + 1]) << 8) |
		(static_cast<uint32_t>(bytes[offset + 2]) << 16) |
		(static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

uint64_t read_le64(const std::vector<uint8_t>& bytes, size_t offset)
{
	uint64_t result = 0;
	for (size_t index = 0; index < 8; ++index)
		result |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8);
	return result;
}

safsyn::Soundfont make_bank(uint32_t sample_rate)
{
	safsyn::Soundfont bank;
	bank.sfz_pcm.emplace_back(sample_rate);
	auto& pcm = bank.sfz_pcm.back();
	for (uint32_t frame = 0; frame < sample_rate; ++frame)
		pcm[frame] = static_cast<int16_t>((frame % 32 < 16 ? 1 : -1) * 8000);
	safsyn::SampleRegion region;
	region.logical_sample_id = 0x534d4654455354ULL;
	region.pcm = pcm.data();
	region.pcm_len = static_cast<uint32_t>(pcm.size());
	region.sample_rate = sample_rate;
	region.root_key = 60;
	region.loop_mode = safsyn::LoopMode::Forward;
	region.loop_start = 0;
	region.loop_end = region.pcm_len;
	region.attack = 0.0f;
	region.decay = 0.0f;
	region.sustain = 1.0f;
	region.release = 0.01f;
	bank.regions.push_back(region);
	return bank;
}

std::vector<uint8_t> render_fixture_midi(bool sustain)
{
	std::vector<uint8_t> track = {0x00, 0x90, 0x3c, 0x64};
	if (sustain)
		track.insert(track.end(), {0x00, 0xb0, 0x40, 0x7f});
	append_vlq(track, 100);
	track.insert(track.end(), {0x80, 0x3c, 0x40});
	if (sustain)
	{
		append_vlq(track, 50);
		track.insert(track.end(), {0xb0, 0x40, 0x00});
	}
	append_eot(track);
	return make_smf(0, 100, {track});
}

void test_type0_running_status_and_usage()
{
	safsyn::SmfFile file;
	check(file.load_bytes(running_status_fixture()), "type-0 running-status fixture loads");
	safsyn::SmfAnalysis analysis;
	safsyn::SmfAnalysisOptions options;
	options.sample_rate = 48000;
	check(safsyn::analyze_smf(file, options, analysis), "type-0 fixture analyzes");
	check(analysis.header.format == 0 && analysis.header.track_count == 1 &&
		analysis.header.ppqn == 480, "type-0 header and PPQN are retained");
	check(analysis.channel_events == 7 && analysis.note_ons == 2 &&
		analysis.note_offs == 2 && analysis.tempo_changes == 1,
		"running channel events and tempo are counted");
	check(analysis.duration_frames == 24000,
		"one 480-tick quarter note schedules at exactly 24000 samples");
	check(analysis.maximum_events_same_tick == 6 && analysis.maximum_events_same_sample == 6,
		"simultaneous tick and output-sample event counts are measured");
	check(analysis.bank_program_usage.size() == 1 &&
		analysis.bank_program_usage[0].bank == 130 &&
		analysis.bank_program_usage[0].program == 5 &&
		analysis.bank_program_usage[0].note_ons == 2,
		"bank MSB/LSB and program usage follow merged channel state");
}

void test_type1_merge_and_tempo_changes()
{
	std::vector<uint8_t> tempo_track = {
		0x00, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20,
		0x00, 0xff, 0x01, 0x01, 'x'};
	append_vlq(tempo_track, 480);
	tempo_track.insert(tempo_track.end(), {0xff, 0x51, 0x03, 0x03, 0xd0, 0x90});
	append_eot(tempo_track, 480);
	std::vector<uint8_t> notes = {0x00, 0x90, 0x3c, 0x64};
	append_vlq(notes, 960);
	notes.insert(notes.end(), {0x80, 0x3c, 0x40});
	append_eot(notes);
	safsyn::SmfFile file;
	check(file.load_bytes(make_smf(1, 480, {tempo_track, notes})), "type-1 fixture loads");
	safsyn::MergedSmfStream merged(file);
	std::array<safsyn::SmfEvent, 3> first{};
	check(merged.next(first[0]) && merged.next(first[1]) && merged.next(first[2]),
		"type-1 heap yields initial events");
	check(first[0].track == 0 && first[0].ordinal == 0 && first[1].track == 0 &&
		first[1].ordinal == 1 && first[2].track == 1,
		"same-tick merge is ordered by track then track-local ordinal");
	safsyn::SmfAnalysis analysis;
	safsyn::SmfAnalysisOptions options;
	options.sample_rate = 48000;
	check(safsyn::analyze_smf(file, options, analysis), "type-1 tempo fixture analyzes");
	check(analysis.tempo_changes == 2 && analysis.duration_frames == 36000,
		"tempo changes apply after their tick without floating drift");
}

void test_smpte_timing()
{
	std::vector<uint8_t> track;
	append_vlq(track, 3000);
	track.insert(track.end(), {0x90, 0x3c, 0x64});
	append_eot(track);
	safsyn::SmfFile file;
	const uint16_t division = static_cast<uint16_t>((static_cast<uint16_t>(0xe3) << 8) | 100);
	check(file.load_bytes(make_smf(0, division, {track})), "SMPTE drop-frame fixture loads");
	safsyn::SmfAnalysis analysis;
	safsyn::SmfAnalysisOptions options;
	options.sample_rate = 48000;
	check(safsyn::analyze_smf(file, options, analysis), "SMPTE fixture analyzes");
	check(analysis.header.smpte && analysis.header.smpte_code == -29 &&
		analysis.duration_frames == 48048,
		"SMPTE -29 uses the exact 30000/1001 frame rate");
}

void test_remainder_preservation()
{
	constexpr uint32_t event_count = 10000;
	std::vector<uint8_t> track = {0x01, 0xb0, 0x01, 0x00};
	for (uint32_t index = 1; index < event_count; ++index)
		track.insert(track.end(), {0x01, 0x01, 0x00});
	append_eot(track);
	safsyn::SmfFile file;
	check(file.load_bytes(make_smf(0, 1001, {track})), "long PPQN fixture loads");
	safsyn::SmfAnalysis analysis;
	safsyn::SmfAnalysisOptions options;
	options.sample_rate = 48000;
	check(safsyn::analyze_smf(file, options, analysis), "long PPQN fixture analyzes");
	const uint64_t expected = static_cast<uint64_t>(event_count) * 500000ULL * 48000ULL /
		(1001ULL * 1000000ULL);
	check(analysis.duration_frames == expected,
		"rational remainder prevents accumulated scheduling drift");
	check(analysis.channel_events == event_count && analysis.parser_state_bytes < 4096,
		"event count does not expand per-track parser state");
}

void test_skipped_meta_sysex_and_malformed_inputs()
{
	std::vector<uint8_t> track = {
		0x00, 0xf0, 0x03, 0x01, 0x02, 0xf7,
		0x00, 0xff, 0x7f, 0x02, 0xaa, 0xbb,
		0x00, 0x90, 0x3c, 0x64};
	append_eot(track);
	safsyn::SmfFile valid;
	check(valid.load_bytes(make_smf(0, 480, {track})), "SysEx/unknown-meta fixture loads");
	safsyn::SmfAnalysis analysis;
	check(safsyn::analyze_smf(valid, {}, analysis) && analysis.sysex_events == 1 &&
		analysis.meta_events == 2 && analysis.note_ons == 1,
		"SysEx and unknown meta payloads are safely skipped");

	safsyn::SmfFile type2;
	check(!type2.load_bytes(make_smf(2, 480, {{0x00, 0xff, 0x2f, 0x00}})),
		"SMF type 2 is explicitly rejected");

	std::vector<uint8_t> bad_vlq = {0x81, 0x80, 0x80, 0x80, 0x00, 0xff, 0x2f, 0x00};
	safsyn::SmfFile malformed;
	check(malformed.load_bytes(make_smf(0, 480, {bad_vlq})),
		"malformed VLQ remains a track-level diagnostic");
	check(!safsyn::analyze_smf(malformed, {}, analysis),
		"a continuation bit in the fourth VLQ byte is rejected");

	std::vector<uint8_t> bad_meta = {0x00, 0xff, 0x01, 0x05, 0xaa};
	safsyn::SmfFile truncated_event;
	check(truncated_event.load_bytes(make_smf(0, 480, {bad_meta})),
		"truncated meta fixture has structurally valid chunks");
	check(!safsyn::analyze_smf(truncated_event, {}, analysis),
		"truncated meta payload is diagnosed during streaming parse");

	auto bad_chunk = make_smf(0, 480, {{0x00}});
	bad_chunk[18] = 0x10; // declared MTrk length 16, actual payload length 1
	safsyn::SmfFile truncated_chunk;
	check(!truncated_chunk.load_bytes(std::move(bad_chunk)),
		"truncated track chunk is rejected before event parsing");
}

void test_streamed_wav_headers(const std::filesystem::path& directory)
{
	std::filesystem::create_directories(directory);
	const std::array<float, 4> samples = {0.25f, -0.25f, 0.5f, -0.5f};
	const auto riff_path = directory / "stream-riff.wav";
	safsyn::FloatWavWriter riff;
	check(riff.open(riff_path.string().c_str(), 48000, 2) &&
		riff.write(samples.data(), 1) && riff.write(samples.data() + 2, 1) && riff.close(),
		"streamed RIFF writer accepts multiple blocks and finalizes");
	const auto riff_bytes = read_file(riff_path);
	check(riff_bytes.size() == 60 && std::string(riff_bytes.begin(), riff_bytes.begin() + 4) == "RIFF" &&
		read_le32(riff_bytes, 4) == 52 && read_le32(riff_bytes, 40) == 16,
		"streamed RIFF header contains final file and data sizes");

	const auto rf64_path = directory / "stream-rf64.wav";
	safsyn::FloatWavWriter rf64;
	const uint64_t large_prediction =
		(static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()) - 36ULL) / 8ULL + 1ULL;
	check(rf64.open(rf64_path.string().c_str(), 48000, large_prediction) &&
		rf64.container() == safsyn::WavContainer::Rf64 &&
		rf64.write(samples.data(), 2) && rf64.close(),
		"large predicted output selects RF64 without allocating it");
	const auto rf64_bytes = read_file(rf64_path);
	check(rf64_bytes.size() == 96 && std::string(rf64_bytes.begin(), rf64_bytes.begin() + 4) == "RF64" &&
		read_le32(rf64_bytes, 4) == (std::numeric_limits<uint32_t>::max)() &&
		read_le64(rf64_bytes, 20) == 88 && read_le64(rf64_bytes, 28) == 16 &&
		read_le64(rf64_bytes, 36) == 2 && read_le32(rf64_bytes, 76) ==
		(std::numeric_limits<uint32_t>::max)(),
		"RF64 ds64 and sentinel sizes are finalized from actual frames");
}

void test_streamed_render_determinism(const std::filesystem::path& directory)
{
	std::filesystem::create_directories(directory);
	safsyn::SmfFile file;
	check(file.load_bytes(render_fixture_midi(false)), "render fixture loads");
	safsyn::SmfAnalysisOptions analysis_options;
	analysis_options.sample_rate = 1000;
	analysis_options.tail_frames = 100;
	safsyn::SmfAnalysis analysis;
	check(safsyn::analyze_smf(file, analysis_options, analysis), "render fixture analyzes");
	auto bank = make_bank(1000);
	for (const bool analytic : {false, true})
	{
		safsyn::SmfRenderOptions first_options;
		first_options.sample_rate = 1000;
		first_options.tail_frames = 100;
		first_options.block_frames = 1;
		if (analytic)
		{
			first_options.phase.mode = safsyn::PhaseMode::Analytic;
			first_options.phase.continuous = true;
			first_options.phase.seed = 17;
		}
		auto second_options = first_options;
		second_options.block_frames = 37;
		const auto first_path = directory / (analytic ? "analytic-a.wav" : "coherent-a.wav");
		const auto second_path = directory / (analytic ? "analytic-b.wav" : "coherent-b.wav");
		safsyn::SmfRenderResult first_result, second_result;
		check(safsyn::render_smf_stream(file, analysis, bank, first_path.string().c_str(),
			first_options, first_result) &&
			safsyn::render_smf_stream(file, analysis, bank, second_path.string().c_str(),
				second_options, second_result),
			"SMF render succeeds at two block sizes");
		check(read_file(first_path) == read_file(second_path),
			"identical SMF renders are bit-identical across block sizes");
		check(first_result.frames_written == 600 && first_result.engine.started_voices == 1 &&
			first_result.dispatched_channel_events == 2,
			"streamed renderer preserves frame and triggered-event counts");
	}

	safsyn::SmfFile sustain_file;
	check(sustain_file.load_bytes(render_fixture_midi(true)), "sustain fixture loads");
	check(safsyn::analyze_smf(sustain_file, analysis_options, analysis), "sustain fixture analyzes");
	safsyn::SmfRenderOptions sustain_options;
	sustain_options.sample_rate = 1000;
	sustain_options.tail_frames = 100;
	const auto sustain_path = directory / "sustain.wav";
	safsyn::SmfRenderResult sustain_result;
	check(safsyn::render_smf_stream(sustain_file, analysis, bank,
		sustain_path.string().c_str(), sustain_options, sustain_result) &&
		sustain_result.dispatched_channel_events == 4 && sustain_result.peak > 0.0f,
		"sustain, note-off, pedal-up, and release tail route through the engine");

	std::vector<uint8_t> unfinished_track = {0x00, 0x90, 0x3c, 0x64};
	append_eot(unfinished_track, 100);
	safsyn::SmfFile unfinished;
	check(unfinished.load_bytes(make_smf(0, 100, {unfinished_track})),
		"unfinished-note fixture loads");
	check(safsyn::analyze_smf(unfinished, analysis_options, analysis),
		"unfinished-note fixture analyzes");
	const auto unfinished_path = directory / "unfinished-note.wav";
	safsyn::SmfRenderResult unfinished_result;
	check(safsyn::render_smf_stream(unfinished, analysis, bank,
		unfinished_path.string().c_str(), sustain_options, unfinished_result) &&
		unfinished_result.active_voices_at_end == 0,
		"notes left active at end-of-track are safely released into the tail");

	safsyn::SmfFile routed;
	check(routed.load_bytes(running_status_fixture()), "bank/program render fixture loads");
	safsyn::SmfAnalysisOptions routed_analysis_options;
	routed_analysis_options.sample_rate = 48000;
	routed_analysis_options.tail_frames = 32;
	check(safsyn::analyze_smf(routed, routed_analysis_options, analysis),
		"bank/program render fixture analyzes");
	bank.regions[0].preset_bank = 130;
	bank.regions[0].preset_program = 5;
	safsyn::SmfRenderOptions routed_options;
	routed_options.sample_rate = 48000;
	routed_options.tail_frames = 32;
	const auto routed_path = directory / "bank-program.wav";
	safsyn::SmfRenderResult routed_result;
	check(safsyn::render_smf_stream(routed, analysis, bank,
		routed_path.string().c_str(), routed_options, routed_result) &&
		routed_result.engine.started_voices == 2,
		"SMF bank select and program change route through the existing engine API");
}
} // namespace

int main(int argc, char** argv)
{
	const std::filesystem::path directory = argc > 1 ? argv[1] : "smf-test-data";
	test_type0_running_status_and_usage();
	test_type1_merge_and_tempo_changes();
	test_smpte_timing();
	test_remainder_preservation();
	test_skipped_meta_sysex_and_malformed_inputs();
	test_streamed_wav_headers(directory);
	test_streamed_render_determinism(directory);
	if (failures != 0)
	{
		std::cerr << failures << " SMF test(s) failed\n";
		return 1;
	}
	std::cout << "All streaming SMF tests passed\n";
	return 0;
}
