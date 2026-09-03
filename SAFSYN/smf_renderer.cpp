#include "smf_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <stop_token>
#include <vector>

namespace safsyn
{
namespace
{
void render_error(std::vector<SmfDiagnostic>& diagnostics, const char* message)
{
	diagnostics.push_back({SmfDiagnosticSeverity::Error, UINT32_MAX, 0, message});
}

bool render_options_valid(const SmfRenderOptions& options) noexcept
{
	return options.sample_rate != 0 && options.voice_capacity != 0 &&
		options.block_frames != 0 && options.block_frames <= 1'048'576U &&
		options.render_threads != 0 && options.render_threads <= 64 &&
		mastering_settings_valid(options.mastering) &&
		(!options.drain_tail || options.maximum_tail_frames != 0);
}

bool render_lengths(uint64_t duration_frames, const SmfRenderOptions& options,
	uint64_t& natural_frames, uint64_t& expected_frames) noexcept
{
	const uint64_t requested_tail = options.drain_tail
		? options.maximum_tail_frames : options.tail_frames;
	if (duration_frames > (std::numeric_limits<uint64_t>::max)() - requested_tail)
		return false;
	natural_frames = duration_frames + requested_tail;
	expected_frames = options.maximum_frames == 0
		? natural_frames : (std::min)(natural_frames, options.maximum_frames);
	return true;
}

struct smf_event_source
{
	const SmfFile& file;
	ScheduledSmfStream stream;
	bool failed = false;

	smf_event_source(const SmfFile& source, uint32_t sample_rate)
		: file(source), stream(source, sample_rate) {}
};

bool next_smf_event(TimedMidiEvent& output, void* user_data) noexcept
{
	auto& source = *static_cast<smf_event_source*>(user_data);
	try
	{
		ScheduledSmfEvent scheduled;
		if (!source.stream.next(scheduled))
		{
			source.failed = !source.stream.good();
			return false;
		}

		output = {};
		output.frame = scheduled.sample;
		if (scheduled.event.kind == SmfEventKind::Channel)
		{
			output.kind = TimedMidiEventKind::ShortMessage;
			output.short_message = scheduled.event.status |
				(static_cast<uint32_t>(scheduled.event.data1) << 8) |
				(static_cast<uint32_t>(scheduled.event.data2) << 16);
		}
		else if (scheduled.event.kind == SmfEventKind::SystemExclusive)
		{
			uint16_t master_volume = 0;
			if (decode_universal_master_volume(source.file,
				scheduled.event, master_volume))
			{
				output.kind = TimedMidiEventKind::MasterVolume;
				output.master_volume = master_volume;
			}
			else
				output.kind = TimedMidiEventKind::SystemExclusive;
		}
		return true;
	}
	catch (...)
	{
		source.failed = true;
		return false;
	}
}

struct wav_sink
{
	FloatWavWriter* writer = nullptr;
};

bool write_wav_pcm(const float* audio, uint32_t frames, uint64_t,
	void* user_data) noexcept
{
	auto& sink = *static_cast<wav_sink*>(user_data);
	return frames == 0 || sink.writer->write(audio, frames);
}
}

bool render_timed_midi_pcm(uint64_t duration_frames,
	const Soundfont& soundfont,
	TimedMidiNextCallback next_event,
	void* next_event_user_data,
	const SmfRenderOptions& options,
	SmfRenderResult& result,
	StereoPcmWriteCallback write_pcm,
	void* write_pcm_user_data) noexcept
{
	result = {};
	uint64_t natural_frames = 0;
	uint64_t expected_frames = 0;
	if (!next_event || !write_pcm || !render_options_valid(options))
	{
		render_error(result.diagnostics, "invalid MIDI PCM render options");
		return false;
	}
	if (!render_lengths(duration_frames, options, natural_frames, expected_frames))
	{
		render_error(result.diagnostics, "MIDI PCM render length overflow");
		return false;
	}

	try
	{
		SynthEngine engine(options.sample_rate, options.voice_capacity);
		engine.set_voice_model(options.voice_model, options.maximum_cohorts);
		engine.set_render_threads(options.render_threads);
		result.render_threads = engine.render_threads();
		engine.set_soundfont(&soundfont);
		engine.set_phase_settings(options.phase);
		engine.set_all_regions_mode(options.all_regions);
		for (uint8_t channel = 0; channel < 16; ++channel)
		{
			engine.control_change(channel, 0,
				static_cast<uint8_t>(options.initial_bank >> 7));
			engine.control_change(channel, 32,
				static_cast<uint8_t>(options.initial_bank & 0x7f));
			engine.program_change(channel, options.initial_program);
		}

		std::vector<float> block(static_cast<size_t>(options.block_frames) * 2);
		std::vector<float> mastered;
		mastered.reserve(static_cast<size_t>(options.block_frames) * 2);
		std::optional<StereoMasteringProcessor> mastering;
		if (options.mastering.active())
			mastering.emplace(options.sample_rate, options.mastering);

		long double raw_sum_squares = 0.0L;
		long double output_sum_squares = 0.0L;
		uint64_t cursor = 0;
		const uint64_t progress_frame_interval = (std::max)(uint64_t{1},
			static_cast<uint64_t>(options.sample_rate) / 100ULL);
		uint64_t next_progress_frame = 0;
		uint64_t next_progress_event = 16'384;
		SmfRenderProgressStage progress_stage = SmfRenderProgressStage::Preparing;

		auto report_progress = [&](SmfRenderProgressStage stage,
			uint64_t completed, uint64_t total) -> bool {
			if (!options.progress_callback)
				return true;
			return options.progress_callback({stage, completed, total,
				result.scheduled_events, engine.active_voice_count(),
				engine.active_cohort_count(), result.raw_peak},
				options.progress_user_data);
		};

		auto capture_engine_state = [&]() {
			result.rms = result.metric_samples == 0 ? 0.0 :
				std::sqrt(static_cast<double>(output_sum_squares / result.metric_samples));
			result.raw_rms = result.raw_metric_samples == 0 ? 0.0 :
				std::sqrt(static_cast<double>(raw_sum_squares / result.raw_metric_samples));
			result.engine = engine.stats();
			result.active_voices_at_end = engine.active_voice_count();
			result.active_cohorts_at_end = engine.active_cohort_count();
			result.phase = engine.phase_cache_stats();
			if (mastering)
				result.mastering = mastering->stats();
		};

		auto finish_cancelled = [&]() -> bool {
			result.cancelled = true;
			capture_engine_state();
			return false;
		};

		std::stop_source preparation_stop;
		bool preparation_cancelled = false;
		PhasePreparationOptions preparation;
		preparation.stop = preparation_stop.get_token();
		preparation.progress = [&](const PhasePreparationProgress& progress) {
			(void)progress;
			if (!report_progress(SmfRenderProgressStage::Preparing, 0, expected_frames))
			{
				preparation_cancelled = true;
				preparation_stop.request_stop();
			}
		};
		if (!report_progress(SmfRenderProgressStage::Preparing, 0, expected_frames))
			return finish_cancelled();
		if (!engine.prepare_playback(options.block_frames, preparation))
		{
			if (preparation_cancelled)
				return finish_cancelled();
			render_error(result.diagnostics, "could not prepare SYNCore playback");
			capture_engine_state();
			return false;
		}

		auto write_output = [&](const float* audio, uint32_t frames) -> bool {
			if (frames == 0)
				return true;
			if (!write_pcm(audio, frames, result.frames_written, write_pcm_user_data))
				return false;
			for (size_t index = 0; index < static_cast<size_t>(frames) * 2; ++index)
			{
				const float value = audio[index];
				result.peak = (std::max)(result.peak, std::abs(value));
				output_sum_squares += static_cast<long double>(value) * value;
			}
			result.metric_samples += static_cast<uint64_t>(frames) * 2;
			result.frames_written += frames;
			return true;
		};

		auto render_to = [&](uint64_t target) -> bool {
			while (cursor < target)
			{
				const uint32_t frames = static_cast<uint32_t>((std::min)(
					target - cursor, static_cast<uint64_t>(options.block_frames)));
				const auto started = std::chrono::steady_clock::now();
				engine.render_audio(block.data(), frames);
				result.render_ms += std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - started).count();
				for (size_t index = 0; index < static_cast<size_t>(frames) * 2; ++index)
				{
					const float value = block[index];
					result.raw_peak = (std::max)(result.raw_peak, std::abs(value));
					raw_sum_squares += static_cast<long double>(value) * value;
				}
				result.raw_metric_samples += static_cast<uint64_t>(frames) * 2;
				if (mastering)
				{
					mastered.clear();
					mastering->process(block.data(), frames, mastered);
					if (!write_output(mastered.data(),
						static_cast<uint32_t>(mastered.size() / 2)))
						return false;
				}
				else if (!write_output(block.data(), frames))
					return false;
				cursor += frames;
				if (cursor >= next_progress_frame &&
					!report_progress(progress_stage, cursor, expected_frames))
				{
					result.cancelled = true;
					return false;
				}
				if (cursor >= next_progress_frame)
					next_progress_frame = cursor >
						(std::numeric_limits<uint64_t>::max)() - progress_frame_interval
						? (std::numeric_limits<uint64_t>::max)()
						: cursor + progress_frame_interval;
			}
			return true;
		};

		const uint64_t limit = options.maximum_frames == 0
			? (std::numeric_limits<uint64_t>::max)() : options.maximum_frames;
		progress_stage = SmfRenderProgressStage::RenderingEvents;
		if (!report_progress(progress_stage, 0, expected_frames))
			return finish_cancelled();

		TimedMidiEvent scheduled;
		bool has_event = next_event(scheduled, next_event_user_data);
		std::vector<uint32_t> sample_messages;
		auto dispatch_messages = [&]() {
			if (sample_messages.empty())
				return;
			engine.consume_short_messages(sample_messages.data(), sample_messages.size());
			result.dispatched_channel_events += sample_messages.size();
			sample_messages.clear();
		};

		while (has_event && scheduled.frame <= duration_frames &&
			scheduled.frame < limit)
		{
			const uint64_t event_frame = scheduled.frame;
			if (event_frame < cursor)
			{
				render_error(result.diagnostics,
					"timed MIDI source returned events out of order");
				capture_engine_state();
				return false;
			}
			if (!render_to(event_frame))
			{
				if (result.cancelled)
					return finish_cancelled();
				render_error(result.diagnostics, "failed while streaming PCM audio");
				capture_engine_state();
				return false;
			}

			sample_messages.clear();
			while (has_event && scheduled.frame == event_frame && scheduled.frame < limit)
			{
				++result.scheduled_events;
				switch (scheduled.kind)
				{
				case TimedMidiEventKind::ShortMessage:
					sample_messages.push_back(scheduled.short_message);
					break;
				case TimedMidiEventKind::SystemExclusive:
					dispatch_messages();
					++result.dispatched_sysex_events;
					break;
				case TimedMidiEventKind::MasterVolume:
					dispatch_messages();
					++result.dispatched_sysex_events;
					++result.master_volume_events;
					engine.set_master_volume(scheduled.master_volume);
					break;
				case TimedMidiEventKind::Ignored:
					break;
				}
				has_event = next_event(scheduled, next_event_user_data);
			}
			dispatch_messages();
			if (result.scheduled_events >= next_progress_event)
			{
				if (!report_progress(progress_stage, cursor, expected_frames))
					return finish_cancelled();
				next_progress_event = result.scheduled_events >
					(std::numeric_limits<uint64_t>::max)() - 16'384ULL
					? (std::numeric_limits<uint64_t>::max)()
					: result.scheduled_events + 16'384ULL;
			}
		}

		if (has_event && scheduled.frame > duration_frames)
		{
			render_error(result.diagnostics,
				"timed MIDI source returned an event beyond its declared duration");
			capture_engine_state();
			return false;
		}
		if (has_event)
		{
			result.truncated = true;
			if (!render_to((std::min)(limit, duration_frames)))
			{
				if (result.cancelled)
					return finish_cancelled();
				render_error(result.diagnostics, "failed while writing bounded PCM excerpt");
				capture_engine_state();
				return false;
			}
		}
		else
		{
			const uint64_t program_end = (std::min)(duration_frames, limit);
			if (!render_to(program_end))
			{
				if (result.cancelled)
					return finish_cancelled();
				render_error(result.diagnostics,
					"failed while rendering to the declared MIDI duration");
				capture_engine_state();
				return false;
			}
			for (uint8_t channel = 0; channel < 16; ++channel)
			{
				engine.control_change(channel, 64, 0);
				engine.control_change(channel, 123, 0);
			}
			const uint64_t tail_start = cursor;
			progress_stage = SmfRenderProgressStage::RenderingTail;
			if (!report_progress(progress_stage, cursor, expected_frames))
				return finish_cancelled();
			next_progress_frame = cursor >
				(std::numeric_limits<uint64_t>::max)() - progress_frame_interval
				? (std::numeric_limits<uint64_t>::max)()
				: cursor + progress_frame_interval;
			uint64_t target = natural_frames;
			if (options.maximum_frames != 0 && target > options.maximum_frames)
			{
				target = options.maximum_frames;
				result.truncated = true;
			}
			if (options.drain_tail)
			{
				while (cursor < target && engine.active_voice_count() != 0)
				{
					if (!render_to(cursor + 1))
					{
						if (result.cancelled)
							return finish_cancelled();
						render_error(result.diagnostics,
							"failed while draining the MIDI release tail");
						capture_engine_state();
						return false;
					}
				}
				result.tail_ceiling_reached =
					engine.active_voice_count() != 0 && cursor == target;
			}
			else if (!render_to(target))
			{
				if (result.cancelled)
					return finish_cancelled();
				render_error(result.diagnostics,
					"failed while writing the MIDI release tail");
				capture_engine_state();
				return false;
			}
			result.tail_frames_written = cursor - tail_start;
		}

		progress_stage = SmfRenderProgressStage::Finalizing;
		const bool cancelled_during_finalization =
			!report_progress(progress_stage, cursor, expected_frames);
		if (mastering)
		{
			mastered.clear();
			mastering->finish(mastered);
			if (!write_output(mastered.data(),
				static_cast<uint32_t>(mastered.size() / 2)))
			{
				render_error(result.diagnostics,
					"failed while flushing mastered PCM audio");
				capture_engine_state();
				return false;
			}
		}
		capture_engine_state();
		if (cancelled_during_finalization)
		{
			result.cancelled = true;
			return false;
		}
		report_progress(SmfRenderProgressStage::Complete,
			result.frames_written, result.frames_written);
		return true;
	}
	catch (...)
	{
		render_error(result.diagnostics, "exception during timed MIDI PCM render");
		return false;
	}
}

bool render_smf_pcm(const SmfFile& file, const SmfAnalysis& analysis,
	const Soundfont& soundfont, const SmfRenderOptions& options,
	SmfRenderResult& result, StereoPcmWriteCallback write_pcm,
	void* write_pcm_user_data) noexcept
{
	result = {};
	if (!file.valid() || analysis.sample_rate != options.sample_rate)
	{
		render_error(result.diagnostics, "invalid SMF PCM render input");
		return false;
	}

	smf_event_source source(file, options.sample_rate);
	const bool rendered = render_timed_midi_pcm(analysis.duration_frames,
		soundfont, next_smf_event, &source, options, result,
		write_pcm, write_pcm_user_data);
	const auto& stream_diagnostics = source.stream.diagnostics();
	if (!stream_diagnostics.empty())
		result.diagnostics.insert(result.diagnostics.end(),
			stream_diagnostics.begin(), stream_diagnostics.end());
	if (source.failed)
		return false;
	return rendered;
}

bool render_smf_stream(const SmfFile& file, const SmfAnalysis& analysis,
	const Soundfont& soundfont, const char* output_path, const SmfRenderOptions& options,
	SmfRenderResult& result) noexcept
{
	result = {};
	uint64_t natural_frames = 0;
	uint64_t expected_frames = 0;
	if (!file.valid() || !output_path || analysis.sample_rate != options.sample_rate ||
		!render_options_valid(options))
	{
		render_error(result.diagnostics, "invalid SMF render options");
		return false;
	}
	if (!render_lengths(analysis.duration_frames, options,
		natural_frames, expected_frames))
	{
		render_error(result.diagnostics, "SMF render length overflow");
		return false;
	}

	try
	{
		FloatWavWriter writer;
		if (!writer.open(output_path, options.sample_rate, expected_frames))
		{
			render_error(result.diagnostics, "could not open streaming WAV output");
			return false;
		}
		wav_sink sink{&writer};
		const bool rendered = render_smf_pcm(file, analysis, soundfont,
			options, result, write_wav_pcm, &sink);
		result.container = writer.container();
		const bool finalized = writer.close();
		result.frames_written = writer.frames_written();
		if (!finalized)
		{
			render_error(result.diagnostics,
				"could not finalize the streaming WAV header");
			return false;
		}
		return rendered;
	}
	catch (...)
	{
		render_error(result.diagnostics, "exception during streamed SMF render");
		return false;
	}
}

} // namespace safsyn
