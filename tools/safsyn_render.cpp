#include "core.h"
#include "smf.h"
#include "smf_renderer.h"
#include "wav_writer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
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

struct Script
{
	std::vector<Event> events;
	uint64_t total_frames = 0;
	double dispatch_hz = 1.0;
	std::string name;
};

safsyn::Soundfont make_demo_soundfont(uint32_t sample_rate, bool loop)
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
	region.logical_sample_id = 1;
	region.pcm = pcm.data();
	region.pcm_len = frames;
	region.sample_rate = sample_rate;
	region.root_key = 69;
	region.loop_mode = loop ? safsyn::LoopMode::Forward : safsyn::LoopMode::None;
	region.loop_start = 0;
	region.loop_end = frames;
	region.attack = 0.005f;
	region.decay = 0.08f;
	region.sustain = 0.72f;
	region.release = 0.18f;
	soundfont.regions.push_back(region);
	return soundfont;
}

Script make_chord_script(uint32_t sample_rate)
{
	auto at = [sample_rate](double seconds) {
		return static_cast<uint64_t>(std::llround(seconds * sample_rate));
	};
	Script script;
	script.events = {
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
	script.total_frames = static_cast<uint64_t>(sample_rate) * 4;
	script.dispatch_hz = 1.0;
	script.name = "chords";
	return script;
}

Script make_repeated_script(uint32_t sample_rate, double repeat_hz, uint32_t count)
{
	Script script;
	script.events.reserve(count);
	for (uint32_t index = 0; index < count; ++index)
		script.events.push_back({static_cast<uint64_t>(std::llround(
			index * static_cast<double>(sample_rate) / repeat_hz)), true, 69, 110});
	const uint64_t tail = static_cast<uint64_t>(std::llround(sample_rate * 1.25));
	script.total_frames = (script.events.empty() ? 0 : script.events.back().frame) + tail;
	script.dispatch_hz = repeat_hz;
	script.name = "repeated";
	return script;
}

safsyn::PhaseMode parse_phase_mode(const std::string& value)
{
	if (value == "coherent") return safsyn::PhaseMode::Coherent;
	if (value == "polarity") return safsyn::PhaseMode::RandomPolarity;
	if (value == "analytic") return safsyn::PhaseMode::Analytic;
	if (value == "smooth-field") return safsyn::PhaseMode::SmoothField;
	if (value == "independent-bins") return safsyn::PhaseMode::IndependentBins;
	throw std::invalid_argument("unknown phase mode");
}

const char* phase_mode_name(safsyn::PhaseMode mode)
{
	switch (mode)
	{
	case safsyn::PhaseMode::Coherent: return "coherent";
	case safsyn::PhaseMode::RandomPolarity: return "polarity";
	case safsyn::PhaseMode::Analytic: return "analytic";
	case safsyn::PhaseMode::SmoothField: return "smooth-field";
	case safsyn::PhaseMode::IndependentBins: return "independent-bins";
	}
	return "unknown";
}

std::string lowercase_extension(const std::string& path)
{
	std::string extension = std::filesystem::path(path).extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return extension;
}

bool seconds_to_frames(double seconds, uint32_t sample_rate, uint64_t& frames)
{
	if (!std::isfinite(seconds) || seconds < 0.0)
		return false;
	const long double exact = static_cast<long double>(seconds) * sample_rate;
	if (exact > static_cast<long double>((std::numeric_limits<uint64_t>::max)()) - 0.5L)
		return false;
	frames = static_cast<uint64_t>(std::floor(exact + 0.5L));
	return true;
}

void print_smf_diagnostics(const std::vector<safsyn::SmfDiagnostic>& diagnostics)
{
	for (const auto& diagnostic : diagnostics)
	{
		std::cerr << (diagnostic.severity == safsyn::SmfDiagnosticSeverity::Error
			? "error" : "warning");
		if (diagnostic.track != UINT32_MAX)
			std::cerr << " track=" << diagnostic.track;
		std::cerr << " offset=" << diagnostic.byte_offset << ": " << diagnostic.message << '\n';
	}
}

void print_smf_analysis(const safsyn::SmfAnalysis& analysis, uint32_t sample_rate,
	double scan_ms, uint64_t maximum_frames)
{
	const uint64_t bounded_frames = maximum_frames == 0 ? analysis.estimated_output_frames :
		(std::min)(analysis.estimated_output_frames, maximum_frames);
	const long double bounded_bytes = static_cast<long double>(bounded_frames) * 8.0L +
		(bounded_frames * 8ULL >
			static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()) - 36ULL ? 80.0L : 44.0L);
	const double throughput = scan_ms > 0.0
		? analysis.total_events * 1000.0 / scan_ms : 0.0;
	std::cout << std::setprecision(9)
		<< "SMF analysis"
		<< " format=" << analysis.header.format
		<< " tracks=" << analysis.header.track_count
		<< " division=";
	if (analysis.header.smpte)
		std::cout << "SMPTE(" << static_cast<int>(analysis.header.smpte_code) << ','
			<< static_cast<unsigned>(analysis.header.ticks_per_frame) << ')';
	else
		std::cout << "PPQN(" << analysis.header.ppqn << ')';
	std::cout << " duration_frames=" << analysis.duration_frames
		<< " duration_seconds=" << static_cast<double>(analysis.duration_frames) / sample_rate
		<< " events=" << analysis.total_events
		<< " channel_events=" << analysis.channel_events
		<< " note_ons=" << analysis.note_ons
		<< " note_offs=" << analysis.note_offs
		<< " tempo_changes=" << analysis.tempo_changes
		<< " sysex_events=" << analysis.sysex_events
		<< " master_volume_sysex=" << analysis.universal_master_volume_events
		<< " max_events_tick=" << analysis.maximum_events_same_tick
		<< " max_events_tick_at=" << analysis.maximum_events_tick_location
		<< " max_events_sample=" << analysis.maximum_events_same_sample
		<< " max_events_sample_at=" << analysis.maximum_events_sample_location
		<< " note_on_groups=" << analysis.note_on_groups
		<< " largest_note_on_group=" << analysis.largest_identical_note_on_group
		<< " largest_note_on_group_at=" << analysis.largest_note_on_group_sample
		<< " note_off_groups=" << analysis.note_off_groups
		<< " largest_note_off_group=" << analysis.largest_identical_note_off_group
		<< " largest_note_off_group_at=" << analysis.largest_note_off_group_sample
		<< " estimated_peak_logical_notes=" << analysis.estimated_peak_active_logical_notes
		<< " estimated_peak_onset_cohorts=" << analysis.estimated_peak_same_onset_cohorts
		<< " estimated_onset_compression=" << analysis.estimated_same_onset_compression_ratio
		<< " parser_state_bytes=" << analysis.parser_state_bytes
		<< " input_bytes=" << analysis.input_bytes
		<< " estimated_output_frames=" << bounded_frames
		<< " estimated_output_bytes=" << static_cast<uint64_t>(bounded_bytes)
		<< " container=" << (bounded_bytes - bounded_frames * 8.0L > 44.0L ? "RF64" : "RIFF")
		<< " analysis_ms=" << scan_ms
		<< " event_throughput=" << throughput << "_events_per_second\n";
	std::cout << "bank_program_usage=";
	const size_t shown = (std::min)(analysis.bank_program_usage.size(), size_t{64});
	for (size_t index = 0; index < shown; ++index)
	{
		const auto& usage = analysis.bank_program_usage[index];
		if (index != 0) std::cout << ';';
		std::cout << "ch" << static_cast<unsigned>(usage.channel + 1) << ':' << usage.bank << '/'
			<< static_cast<unsigned>(usage.program) << '(' << usage.note_ons << ')';
	}
	if (shown < analysis.bank_program_usage.size())
		std::cout << ";+" << (analysis.bank_program_usage.size() - shown) << "_more";
	std::cout << '\n';
	auto print_histogram = [](const char* name,
		const std::vector<safsyn::SmfGroupHistogramEntry>& histogram) {
		std::cout << name << '=';
		const size_t shown_entries = (std::min)(histogram.size(), size_t{64});
		for (size_t index = 0; index < shown_entries; ++index)
		{
			if (index != 0) std::cout << ';';
			const auto& entry = histogram[index];
			std::cout << entry.group_size << 'x' << entry.groups << '(' << entry.events << ')';
		}
		if (shown_entries < histogram.size())
			std::cout << ";+" << (histogram.size() - shown_entries) << "_more";
		std::cout << '\n';
	};
	print_histogram("note_on_group_histogram", analysis.note_on_group_histogram);
	print_histogram("note_off_group_histogram", analysis.note_off_group_histogram);
	std::cout << "controller_histogram=";
	for (size_t index = 0; index < analysis.controller_usage.size(); ++index)
	{
		if (index != 0) std::cout << ';';
		const auto& usage = analysis.controller_usage[index];
		std::cout << "ch" << static_cast<unsigned>(usage.channel + 1)
			<< ":cc" << static_cast<unsigned>(usage.controller)
			<< 'x' << usage.events << '[' << static_cast<unsigned>(usage.minimum_value)
			<< ',' << static_cast<unsigned>(usage.maximum_value) << "]@"
			<< usage.first_sample << '-' << usage.last_sample;
	}
	std::cout << '\n';
	for (const auto& entry : analysis.controller_trace)
		std::cout << "controller_trace"
			<< " tick=" << entry.tick
			<< " sample=" << entry.sample
			<< " seconds=" << static_cast<double>(entry.sample) / sample_rate
			<< " track=" << entry.track
			<< " ordinal=" << entry.ordinal
			<< " channel=" << static_cast<unsigned>(entry.channel + 1)
			<< " cc=" << static_cast<unsigned>(entry.controller)
			<< " value=" << static_cast<unsigned>(entry.value)
			<< " volume14=" << entry.volume
			<< " pan14=" << entry.pan
			<< " expression14=" << entry.expression
			<< " sustain=" << (entry.sustain ? "down" : "up")
			<< " active_channel_notes_before=" << entry.active_channel_notes_before
			<< " active_channel_notes=" << entry.active_channel_notes << '\n';
}

int run_smf(int argc, char** argv)
{
	try
	{
		const std::string midi_path = argv[1];
		bool analysis_only = argc >= 3 &&
			(std::string(argv[2]) == "--analyze" || std::string(argv[2]) == "--dry-run");
		std::string bank_path;
		std::string output_path;
		int option_index = 0;
		if (analysis_only)
			option_index = 3;
		else
		{
			if (argc < 4)
				return 2;
			bank_path = argv[2];
			output_path = argv[3];
			option_index = 4;
		}

		uint32_t sample_rate = 48000;
		size_t voice_capacity = 256;
		size_t maximum_cohorts = 0;
		safsyn::VoiceModel voice_model = safsyn::VoiceModel::Cohorts;
		uint32_t bank = 0;
		uint32_t program = 0;
		bool all_regions = false;
		double tail_seconds = 2.0;
		bool drain_tail = false;
		double maximum_tail_seconds = 30.0;
		double maximum_seconds = 0.0;
		bool maximum_set = false;
		double controller_trace_seconds = 0.0;
		double controller_trace_start_seconds = 0.0;
		size_t controller_trace_limit = 512;
		int controller_trace_controller = -1;
		uint32_t block_frames = 256;
		safsyn::PhaseSettings phase_settings;
		safsyn::MasteringSettings mastering_settings;
		for (int index = option_index; index < argc; ++index)
		{
			const std::string option = argv[index];
			if (option == "--analyze" || option == "--dry-run") analysis_only = true;
			else if (option == "--sample-rate" && index + 1 < argc)
				sample_rate = static_cast<uint32_t>(std::stoul(argv[++index]));
			else if (option == "--voices" && index + 1 < argc)
				voice_capacity = static_cast<size_t>(std::stoull(argv[++index]));
			else if (option == "--max-cohorts" && index + 1 < argc)
				maximum_cohorts = static_cast<size_t>(std::stoull(argv[++index]));
			else if (option == "--individual-voices")
				voice_model = safsyn::VoiceModel::Individual;
			else if (option == "--cohorts")
				voice_model = safsyn::VoiceModel::Cohorts;
			else if (option == "--bank" && index + 1 < argc)
				bank = static_cast<uint32_t>(std::stoul(argv[++index]));
			else if (option == "--program" && index + 1 < argc)
				program = static_cast<uint32_t>(std::stoul(argv[++index]));
			else if (option == "--all-regions") all_regions = true;
			else if (option == "--tail-seconds" && index + 1 < argc)
				tail_seconds = std::stod(argv[++index]);
			else if (option == "--drain-tail")
				drain_tail = true;
			else if (option == "--max-tail-seconds" && index + 1 < argc)
				maximum_tail_seconds = std::stod(argv[++index]);
			else if (option == "--max-render-seconds" && index + 1 < argc)
			{
				maximum_seconds = std::stod(argv[++index]);
				maximum_set = true;
			}
			else if (option == "--controller-trace-seconds" && index + 1 < argc)
				controller_trace_seconds = std::stod(argv[++index]);
			else if (option == "--controller-trace-start-seconds" && index + 1 < argc)
				controller_trace_start_seconds = std::stod(argv[++index]);
			else if (option == "--controller-trace-limit" && index + 1 < argc)
				controller_trace_limit = static_cast<size_t>(std::stoull(argv[++index]));
			else if (option == "--controller-trace-cc" && index + 1 < argc)
				controller_trace_controller = std::stoi(argv[++index]);
			else if (option == "--block-size" && index + 1 < argc)
				block_frames = static_cast<uint32_t>(std::stoul(argv[++index]));
			else if (option == "--output-gain-db" && index + 1 < argc)
				mastering_settings.output_gain_db = std::stod(argv[++index]);
			else if (option == "--limiter")
				mastering_settings.limiter_enabled = true;
			else if (option == "--limiter-ceiling-db" && index + 1 < argc)
				mastering_settings.limiter_ceiling_db = std::stod(argv[++index]);
			else if (option == "--limiter-lookahead-ms" && index + 1 < argc)
				mastering_settings.limiter_lookahead_ms = std::stod(argv[++index]);
			else if (option == "--limiter-release-ms" && index + 1 < argc)
				mastering_settings.limiter_release_ms = std::stod(argv[++index]);
			else if (option == "--phase-mode" && index + 1 < argc)
				phase_settings.mode = parse_phase_mode(argv[++index]);
			else if (option == "--phase-strength" && index + 1 < argc)
				phase_settings.strength = std::stof(argv[++index]);
			else if (option == "--phase-pool" && index + 1 < argc)
				phase_settings.pool_size = static_cast<uint32_t>(std::stoul(argv[++index]));
			else if (option == "--phase-continuous") phase_settings.continuous = true;
			else if (option == "--phase-seed" && index + 1 < argc)
				phase_settings.seed = std::stoull(argv[++index]);
			else if (option == "--phase-correlation-hz" && index + 1 < argc)
				phase_settings.correlation_hz = std::stof(argv[++index]);
			else if (option == "--phase-preserve-attack-ms" && index + 1 < argc)
				phase_settings.preserve_attack_ms = std::stof(argv[++index]);
			else
				return 2;
		}

		uint64_t tail_frames = 0;
		uint64_t maximum_tail_frames = 0;
		uint64_t maximum_frames = 0;
		uint64_t controller_trace_frames = 0;
		uint64_t controller_trace_start_frame = 0;
		if (sample_rate < 8000 || sample_rate > 384000 || voice_capacity == 0 ||
			bank > 16383 || program > 127 || block_frames == 0 || block_frames > 1'048'576U ||
			phase_settings.strength < 0.0f || phase_settings.strength > 1.0f ||
			phase_settings.pool_size == 0 || phase_settings.pool_size > 64 ||
			phase_settings.correlation_hz <= 0.0f || phase_settings.preserve_attack_ms < 0.0f ||
			!seconds_to_frames(tail_seconds, sample_rate, tail_frames) ||
			!seconds_to_frames(maximum_tail_seconds, sample_rate, maximum_tail_frames) ||
			!seconds_to_frames(controller_trace_seconds, sample_rate, controller_trace_frames) ||
			!seconds_to_frames(controller_trace_start_seconds, sample_rate,
				controller_trace_start_frame) ||
			(controller_trace_seconds > 0.0 && controller_trace_limit == 0) ||
			controller_trace_controller < -1 || controller_trace_controller > 127 ||
			!safsyn::mastering_settings_valid(mastering_settings) ||
			(drain_tail && maximum_tail_frames == 0) ||
			(maximum_set && (maximum_seconds <= 0.0 ||
				!seconds_to_frames(maximum_seconds, sample_rate, maximum_frames) ||
				maximum_frames == 0)))
		{
			std::cerr << "Invalid SMF renderer option value.\n";
			return 2;
		}

		safsyn::SmfFile midi;
		if (!midi.load(midi_path.c_str()))
		{
			print_smf_diagnostics(midi.diagnostics());
			return 1;
		}
		safsyn::SmfAnalysisOptions analysis_options;
		analysis_options.sample_rate = sample_rate;
		analysis_options.initial_bank = static_cast<uint16_t>(bank);
		analysis_options.initial_program = static_cast<uint8_t>(program);
		analysis_options.tail_frames = drain_tail ? maximum_tail_frames : tail_frames;
		analysis_options.controller_trace_frames = controller_trace_frames;
		analysis_options.controller_trace_start_frame = controller_trace_start_frame;
		analysis_options.controller_trace_limit = controller_trace_limit;
		analysis_options.controller_trace_controller =
			static_cast<int16_t>(controller_trace_controller);
		safsyn::SmfAnalysis analysis;
		const auto analysis_started = std::chrono::steady_clock::now();
		const bool analyzed = safsyn::analyze_smf(midi, analysis_options, analysis);
		const double analysis_ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - analysis_started).count();
		print_smf_diagnostics(analysis.diagnostics);
		if (!analyzed)
			return 1;
		print_smf_analysis(analysis, sample_rate, analysis_ms, maximum_frames);
		if (analysis_only)
			return 0;

		safsyn::Soundfont soundfont;
		const std::string extension = lowercase_extension(bank_path);
		const bool loaded = extension == ".sfz"
			? safsyn::load_sfz(bank_path.c_str(), soundfont)
			: extension == ".sf2" && safsyn::load_sf2(bank_path.c_str(), soundfont);
		if (!loaded)
		{
			std::cerr << "Could not load sound bank: " << bank_path << '\n';
			return 1;
		}

		safsyn::SmfRenderOptions render_options;
		render_options.sample_rate = sample_rate;
		render_options.voice_capacity = voice_capacity;
		render_options.voice_model = voice_model;
		render_options.maximum_cohorts = maximum_cohorts;
		render_options.initial_bank = static_cast<uint16_t>(bank);
		render_options.initial_program = static_cast<uint8_t>(program);
		render_options.tail_frames = tail_frames;
		render_options.drain_tail = drain_tail;
		render_options.maximum_tail_frames = maximum_tail_frames;
		render_options.maximum_frames = maximum_frames;
		render_options.block_frames = block_frames;
		render_options.all_regions = all_regions;
		render_options.phase = phase_settings;
		render_options.mastering = mastering_settings;
		safsyn::SmfRenderResult result;
		const auto total_started = std::chrono::steady_clock::now();
		const bool rendered = safsyn::render_smf_stream(midi, analysis, soundfont,
			output_path.c_str(), render_options, result);
		const double total_ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - total_started).count();
		print_smf_diagnostics(result.diagnostics);
		if (!rendered)
			return 1;
		const uint64_t onset_cohorts = result.engine.logical_voices_started -
			result.engine.logical_voices_merged;
		const double cohort_compression = onset_cohorts == 0 ? 0.0 :
			static_cast<double>(result.engine.logical_voices_started) / onset_cohorts;
		std::cout << std::setprecision(9)
			<< "Rendered SMF to " << output_path
			<< " frames=" << result.frames_written
			<< " scheduled_events=" << result.scheduled_events
			<< " dispatched_channel_events=" << result.dispatched_channel_events
			<< " dispatched_sysex_events=" << result.dispatched_sysex_events
			<< " master_volume_events=" << result.master_volume_events
			<< " started_voices=" << result.engine.started_voices
			<< " peak_active=" << result.engine.peak_active_voices
			<< " active_end=" << result.active_voices_at_end
			<< " stolen=" << result.engine.stolen_voices
			<< " voice_model=" << (voice_model == safsyn::VoiceModel::Cohorts ? "cohorts" : "individual")
			<< " logical_started=" << result.engine.logical_voices_started
			<< " peak_logical=" << result.engine.peak_active_logical_voices
			<< " cohorts_created=" << result.engine.cohorts_created
			<< " peak_cohorts=" << result.engine.peak_active_cohorts
			<< " active_cohorts_end=" << result.active_cohorts_at_end
			<< " logical_merged=" << result.engine.logical_voices_merged
			<< " cohort_compression=" << cohort_compression
			<< " cohort_splits=" << result.engine.cohort_splits
			<< " cohort_merges=" << result.engine.cohort_merges
			<< " cohort_capacity_steals=" << result.engine.cohort_capacity_steals
			<< " channel_scoped_steals=" << result.engine.channel_scoped_steals
			<< " channel_reserve_steals=" << result.engine.channel_reserve_steals
			<< " global_fallback_steals=" << result.engine.global_fallback_steals
			<< " avg_cohort_multiplicity=" << result.engine.average_cohort_multiplicity
			<< " max_cohort_multiplicity=" << result.engine.maximum_cohort_multiplicity
			<< " tail_frames=" << result.tail_frames_written
			<< " tail_ceiling_reached=" << (result.tail_ceiling_reached ? "yes" : "no")
			<< " raw_peak=" << result.raw_peak
			<< " raw_rms=" << result.raw_rms
			<< " peak=" << result.peak
			<< " rms=" << result.rms
			<< " output_gain_db=" << mastering_settings.output_gain_db
			<< " limiter=" << (mastering_settings.limiter_enabled ? "on" : "off")
			<< " limiter_lookahead_frames=" << result.mastering.lookahead_frames
			<< " limiter_limited_frames=" << result.mastering.limited_frames
			<< " limiter_max_reduction_db=" << result.mastering.maximum_gain_reduction_db
			<< " container=" << (result.container == safsyn::WavContainer::Rf64 ? "RF64" : "RIFF")
			<< " truncated=" << (result.truncated ? "yes" : "no")
			<< " phase_mode=" << phase_mode_name(phase_settings.mode)
			<< " phase_seed=" << phase_settings.seed
			<< " preprocessing_ms=" << result.phase.preprocessing_ms
			<< " phase_cache_bytes=" << result.phase.cache_bytes
			<< " phase_cached_samples=" << result.phase.cached_samples
			<< " phase_cached_variants=" << result.phase.cached_variants
			<< " phase_failures=" << result.phase.failures
			<< " render_audio_ms=" << result.render_ms
			<< " total_render_ms=" << total_ms << '\n';
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "SMF renderer argument error: " << error.what() << '\n';
		return 2;
	}
}

struct AudioMetrics
{
	float peak = 0.0f;
	double rms = 0.0;
	double periodicity = 0.0;
	double dispatch_db = -300.0;
};

AudioMetrics measure_audio(const std::vector<float>& audio, uint32_t sample_rate,
	double dispatch_hz)
{
	AudioMetrics metrics;
	if (audio.empty()) return metrics;
	double sum_squares = 0.0;
	std::vector<double> mono(audio.size() / 2);
	for (size_t frame = 0; frame < mono.size(); ++frame)
	{
		const float left = audio[frame * 2];
		const float right = audio[frame * 2 + 1];
		metrics.peak = (std::max)({metrics.peak, std::abs(left), std::abs(right)});
		sum_squares += static_cast<double>(left) * left + static_cast<double>(right) * right;
		mono[frame] = (static_cast<double>(left) + right) * 0.5;
	}
	metrics.rms = std::sqrt(sum_squares / audio.size());
	const size_t lag = static_cast<size_t>(std::llround(sample_rate / dispatch_hz));
	if (lag > 0 && lag < mono.size())
	{
		double cross = 0.0, left_energy = 0.0, right_energy = 0.0;
		for (size_t index = 0; index + lag < mono.size(); ++index)
		{
			cross += mono[index] * mono[index + lag];
			left_energy += mono[index] * mono[index];
			right_energy += mono[index + lag] * mono[index + lag];
		}
		const double denominator = std::sqrt(left_energy * right_energy);
		metrics.periodicity = denominator > 1e-30 ? cross / denominator : 0.0;
	}
	double real = 0.0, imaginary = 0.0, mono_energy = 0.0;
	for (size_t index = 0; index < mono.size(); ++index)
	{
		const double angle = 2.0 * 3.14159265358979323846 * dispatch_hz * index / sample_rate;
		real += mono[index] * std::cos(angle);
		imaginary -= mono[index] * std::sin(angle);
		mono_energy += mono[index] * mono[index];
	}
	const double amplitude = mono.empty() ? 0.0 :
		2.0 * std::sqrt(real * real + imaginary * imaginary) / mono.size();
	const double mono_rms = mono.empty() ? 0.0 : std::sqrt(mono_energy / mono.size());
	metrics.dispatch_db = 20.0 * std::log10((std::max)(amplitude, 1e-15) /
		(std::max)(mono_rms * std::sqrt(2.0), 1e-15));
	return metrics;
}

void print_usage()
{
	std::cerr << "Usage:\n"
		"  safsyn-render input.mid soundfont.sf2 output.wav [SMF options]\n"
		"  safsyn-render input.mid --analyze [SMF options]\n"
		"  safsyn-render --demo output.wav [--sample-rate N] [--voices N]\n"
		"  safsyn-render bank.sfz|bank.sf2 output.wav [--bank N] [--program N]\n"
		"      [--sample-rate N] [--voices N] [--all-regions]\n"
		"  Phase: --phase-mode coherent|polarity|analytic|smooth-field|independent-bins\n"
		"      [--phase-strength 0..1] [--phase-pool 1..64] [--phase-continuous]\n"
		"      [--phase-seed N] [--phase-correlation-hz N]\n"
		"      [--phase-preserve-attack-ms N]\n"
		"  SMF: [--tail-seconds N] [--max-render-seconds N] [--analyze|--dry-run]\n"
		"      [--block-size N] [--cohorts|--individual-voices] [--max-cohorts N]\n"
		"      [--drain-tail] [--max-tail-seconds N]\n"
		"      [--controller-trace-start-seconds N] [--controller-trace-seconds N]\n"
		"      [--controller-trace-cc 0..127] [--controller-trace-limit N]\n"
		"  Mastering: [--output-gain-db N] [--limiter] [--limiter-ceiling-db N]\n"
		"      [--limiter-lookahead-ms N] [--limiter-release-ms N]\n"
		"  Script: [--script chords|repeated] [--repeat-hz N] [--repeat-count N]\n";
}
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		print_usage();
		return 2;
	}
	if (lowercase_extension(argv[1]) == ".mid")
	{
		const int result = run_smf(argc, argv);
		if (result == 2) print_usage();
		return result;
	}

	const bool demo = std::string(argv[1]) == "--demo";
	const std::string bank_path = demo ? std::string{} : argv[1];
	const std::string output_path = argv[2];
	uint32_t sample_rate = 48000;
	size_t voice_capacity = 256;
	uint32_t bank = 0;
	uint32_t program = 0;
	bool all_regions = false;
	std::string script_name = "chords";
	double repeat_hz = 40.0;
	uint32_t repeat_count = 128;
	safsyn::PhaseSettings phase_settings;
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
		else if (option == "--script" && index + 1 < argc)
			script_name = argv[++index];
		else if (option == "--repeat-hz" && index + 1 < argc)
			repeat_hz = std::stod(argv[++index]);
		else if (option == "--repeat-count" && index + 1 < argc)
			repeat_count = static_cast<uint32_t>(std::stoul(argv[++index]));
		else if (option == "--phase-mode" && index + 1 < argc)
			phase_settings.mode = parse_phase_mode(argv[++index]);
		else if (option == "--phase-strength" && index + 1 < argc)
			phase_settings.strength = std::stof(argv[++index]);
		else if (option == "--phase-pool" && index + 1 < argc)
			phase_settings.pool_size = static_cast<uint32_t>(std::stoul(argv[++index]));
		else if (option == "--phase-continuous")
			phase_settings.continuous = true;
		else if (option == "--phase-seed" && index + 1 < argc)
			phase_settings.seed = std::stoull(argv[++index]);
		else if (option == "--phase-correlation-hz" && index + 1 < argc)
			phase_settings.correlation_hz = std::stof(argv[++index]);
		else if (option == "--phase-preserve-attack-ms" && index + 1 < argc)
			phase_settings.preserve_attack_ms = std::stof(argv[++index]);
		else
		{
			print_usage();
			return 2;
		}
	}
	if (sample_rate < 8000 || sample_rate > 384000 || voice_capacity == 0 ||
		bank > 16383 || program > 127 || (script_name != "chords" && script_name != "repeated") ||
		repeat_hz <= 0.0 || repeat_hz > 2000.0 || repeat_count == 0 ||
		phase_settings.strength < 0.0f || phase_settings.strength > 1.0f ||
		phase_settings.pool_size == 0 || phase_settings.pool_size > 64 ||
		phase_settings.correlation_hz <= 0.0f || phase_settings.preserve_attack_ms < 0.0f)
	{
		std::cerr << "Invalid renderer option value.\n";
		return 2;
	}

	safsyn::Soundfont soundfont;
	if (demo)
		soundfont = make_demo_soundfont(sample_rate, script_name == "chords");
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

	const Script script = script_name == "repeated"
		? make_repeated_script(sample_rate, repeat_hz, repeat_count)
		: make_chord_script(sample_rate);
	const auto& events = script.events;
	const uint64_t total_frames = script.total_frames;
	std::vector<float> audio(static_cast<size_t>(total_frames) * 2, 0.0f);
	safsyn::SynthEngine engine(sample_rate, voice_capacity);
	engine.set_soundfont(&soundfont);
	engine.set_phase_settings(phase_settings);
	engine.set_all_regions_mode(all_regions);
	engine.control_change(0, 0, static_cast<uint8_t>(bank >> 7));
	engine.control_change(0, 32, static_cast<uint8_t>(bank & 0x7f));
	engine.program_change(0, static_cast<uint8_t>(program));

	size_t event_index = 0;
	uint64_t cursor = 0;
	double render_ms = 0.0;
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
		const auto render_started = std::chrono::steady_clock::now();
		engine.render_audio(audio.data() + static_cast<size_t>(cursor) * 2, frames);
		render_ms += std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - render_started).count();
		cursor = boundary;
	}

	if (!safsyn::write_float_wav(output_path.c_str(), audio.data(), total_frames, sample_rate))
	{
		std::cerr << "Could not write output WAV: " << output_path << '\n';
		return 1;
	}
	const AudioMetrics metrics = measure_audio(audio, sample_rate, script.dispatch_hz);
	const auto& stats = engine.stats();
	const auto phase_stats = engine.phase_cache_stats();
	const size_t selected_regions = all_regions && !soundfont.stress_regions.empty()
		? soundfont.stress_regions.size()
		: static_cast<size_t>(std::count_if(
			soundfont.regions.begin(), soundfont.regions.end(),
			[&](const safsyn::SampleRegion& region) {
				return all_regions ||
					(region.preset_bank == bank && region.preset_program == program);
			}));
	std::cout << std::setprecision(9)
		<< "Rendered " << stats.rendered_frames << " frames to " << output_path
		<< "\npresets=" << soundfont.presets.size()
		<< " regions=" << soundfont.regions.size()
		<< " stress_regions=" << soundfont.stress_regions.size()
		<< " selected_regions=" << selected_regions
		<< " bank=" << bank
		<< " program=" << static_cast<unsigned>(program)
		<< " all_regions=" << (all_regions ? "yes" : "no")
		<< " script=" << script.name
		<< " phase_mode=" << phase_mode_name(engine.phase_settings().mode)
		<< " phase_strength=" << engine.phase_settings().strength
		<< " phase_pool=" << engine.phase_settings().pool_size
		<< " phase_continuous=" << (engine.phase_settings().continuous ? "yes" : "no")
		<< " phase_seed=" << engine.phase_settings().seed
		<< " started_voices=" << stats.started_voices
		<< " peak_active=" << stats.peak_active_voices
		<< " stolen=" << stats.stolen_voices
		<< " peak=" << metrics.peak
		<< " rms=" << metrics.rms
		<< " periodicity=" << metrics.periodicity
		<< " dispatch_db=" << metrics.dispatch_db
		<< " preprocessing_ms=" << phase_stats.preprocessing_ms
		<< " render_ms=" << render_ms
		<< " phase_cache_bytes=" << phase_stats.cache_bytes
		<< " phase_cached_samples=" << phase_stats.cached_samples
		<< " phase_cached_variants=" << phase_stats.cached_variants
		<< " phase_failures=" << phase_stats.failures << '\n';
	return 0;
}
