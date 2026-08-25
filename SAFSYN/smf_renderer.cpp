#include "smf_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace safsyn
{
namespace
{
void render_error(std::vector<SmfDiagnostic>& diagnostics, const char* message)
{
	diagnostics.push_back({SmfDiagnosticSeverity::Error, UINT32_MAX, 0, message});
}
}

bool render_smf_stream(const SmfFile& file, const SmfAnalysis& analysis,
	const Soundfont& soundfont, const char* output_path, const SmfRenderOptions& options,
	SmfRenderResult& result) noexcept
{
	result = {};
	if (!file.valid() || !output_path || options.sample_rate == 0 ||
		analysis.sample_rate != options.sample_rate ||
		options.voice_capacity == 0 || options.block_frames == 0 ||
		options.block_frames > 1'048'576U ||
		options.render_threads == 0 || options.render_threads > 64 ||
		!mastering_settings_valid(options.mastering) ||
		(options.drain_tail && options.maximum_tail_frames == 0))
	{
		render_error(result.diagnostics, "invalid SMF render options");
		return false;
	}
	try
	{
		const uint64_t requested_tail = options.drain_tail
			? options.maximum_tail_frames : options.tail_frames;
		if (analysis.duration_frames > (std::numeric_limits<uint64_t>::max)() -
			requested_tail)
		{
			render_error(result.diagnostics, "SMF render length overflow");
			return false;
		}
		const uint64_t natural_frames = analysis.duration_frames + requested_tail;
		const uint64_t expected_frames = options.maximum_frames == 0
			? natural_frames : (std::min)(natural_frames, options.maximum_frames);
		FloatWavWriter writer;
		if (!writer.open(output_path, options.sample_rate, expected_frames))
		{
			render_error(result.diagnostics, "could not open streaming WAV output");
			return false;
		}

		SynthEngine engine(options.sample_rate, options.voice_capacity);
		engine.set_voice_model(options.voice_model, options.maximum_cohorts);
		engine.set_render_threads(options.render_threads);
		result.render_threads = engine.render_threads();
		engine.set_soundfont(&soundfont);
		engine.set_phase_settings(options.phase);
		engine.set_all_regions_mode(options.all_regions);
		for (uint8_t channel = 0; channel < 16; ++channel)
		{
			engine.control_change(channel, 0, static_cast<uint8_t>(options.initial_bank >> 7));
			engine.control_change(channel, 32, static_cast<uint8_t>(options.initial_bank & 0x7f));
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
		auto finish_cancelled = [&]() -> bool {
			result.cancelled = true;
			result.container = writer.container();
			writer.close();
			result.frames_written = writer.frames_written();
			result.engine = engine.stats();
			result.active_voices_at_end = engine.active_voice_count();
			result.active_cohorts_at_end = engine.active_cohort_count();
			result.phase = engine.phase_cache_stats();
			return false;
		};
		auto write_output = [&](const float* audio, uint32_t frames) -> bool {
			for (size_t index = 0; index < static_cast<size_t>(frames) * 2; ++index)
			{
				const float value = audio[index];
				result.peak = (std::max)(result.peak, std::abs(value));
				output_sum_squares += static_cast<long double>(value) * value;
			}
			result.metric_samples += static_cast<uint64_t>(frames) * 2;
			return frames == 0 || writer.write(audio, frames);
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
					next_progress_frame = cursor > (std::numeric_limits<uint64_t>::max)() -
						progress_frame_interval ? (std::numeric_limits<uint64_t>::max)() :
						cursor + progress_frame_interval;
			}
			return true;
		};
		if (!report_progress(SmfRenderProgressStage::Preparing, 0, expected_frames))
			return finish_cancelled();

		const uint64_t limit = options.maximum_frames == 0
			? (std::numeric_limits<uint64_t>::max)() : options.maximum_frames;
		progress_stage = SmfRenderProgressStage::RenderingEvents;
		if (!report_progress(progress_stage, 0, expected_frames))
			return finish_cancelled();
		ScheduledSmfStream stream(file, options.sample_rate);
		ScheduledSmfEvent scheduled;
		bool has_event = stream.next(scheduled);
		std::vector<uint32_t> sample_messages;
		auto dispatch_messages = [&]() {
			engine.consume_short_messages(sample_messages.data(), sample_messages.size());
			result.dispatched_channel_events += sample_messages.size();
			sample_messages.clear();
		};
		while (has_event && scheduled.sample < limit)
		{
			const uint64_t event_sample = scheduled.sample;
			if (!render_to(event_sample))
			{
				if (result.cancelled)
					return finish_cancelled();
				render_error(result.diagnostics, "failed while streaming WAV audio");
				return false;
			}
			sample_messages.clear();
			while (has_event && scheduled.sample == event_sample && scheduled.sample < limit)
			{
				++result.scheduled_events;
				if (scheduled.event.kind == SmfEventKind::Channel)
				{
					sample_messages.push_back(scheduled.event.status |
						(static_cast<uint32_t>(scheduled.event.data1) << 8) |
						(static_cast<uint32_t>(scheduled.event.data2) << 16));
				}
				else if (scheduled.event.kind == SmfEventKind::SystemExclusive)
				{
					dispatch_messages();
					++result.dispatched_sysex_events;
					uint16_t master_volume = 0;
					if (decode_universal_master_volume(file, scheduled.event, master_volume))
					{
						engine.set_master_volume(master_volume);
						++result.master_volume_events;
					}
				}
				has_event = stream.next(scheduled);
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
		if (!stream.good())
		{
			result.diagnostics = stream.diagnostics();
			return false;
		}

		if (has_event)
		{
			result.truncated = true;
			if (!render_to(limit))
			{
				if (result.cancelled)
					return finish_cancelled();
				render_error(result.diagnostics, "failed while writing bounded SMF excerpt");
				return false;
			}
		}
		else
		{
			for (uint8_t channel = 0; channel < 16; ++channel)
			{
				engine.control_change(channel, 64, 0);
				engine.control_change(channel, 123, 0);
			}
			const uint64_t tail_start = cursor;
			progress_stage = SmfRenderProgressStage::RenderingTail;
			if (!report_progress(progress_stage, cursor, expected_frames))
				return finish_cancelled();
			next_progress_frame = cursor > (std::numeric_limits<uint64_t>::max)() -
				progress_frame_interval ? (std::numeric_limits<uint64_t>::max)() :
				cursor + progress_frame_interval;
			uint64_t target = natural_frames;
			if (options.maximum_frames != 0 && target > options.maximum_frames)
			{
				target = options.maximum_frames;
				result.truncated = true;
			}
			if (options.drain_tail)
			{
				while (cursor < target && engine.active_voice_count() != 0)
					if (!render_to(cursor + 1))
					{
						if (result.cancelled)
							return finish_cancelled();
						render_error(result.diagnostics, "failed while draining the SMF release tail");
						return false;
					}
				result.tail_ceiling_reached = engine.active_voice_count() != 0 && cursor == target;
			}
			else if (!render_to(target))
			{
				if (result.cancelled)
					return finish_cancelled();
				render_error(result.diagnostics, "failed while writing the SMF release tail");
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
			if (!write_output(mastered.data(), static_cast<uint32_t>(mastered.size() / 2)))
			{
				render_error(result.diagnostics, "failed while flushing mastered WAV audio");
				return false;
			}
			result.mastering = mastering->stats();
		}
		result.container = writer.container();
		if (!writer.close())
		{
			render_error(result.diagnostics, "could not finalize the streaming WAV header");
			return false;
		}
		result.frames_written = writer.frames_written();
		result.rms = result.metric_samples == 0 ? 0.0 :
			std::sqrt(static_cast<double>(output_sum_squares / result.metric_samples));
		result.raw_rms = result.raw_metric_samples == 0 ? 0.0 :
			std::sqrt(static_cast<double>(raw_sum_squares / result.raw_metric_samples));
		result.engine = engine.stats();
		result.active_voices_at_end = engine.active_voice_count();
		result.active_cohorts_at_end = engine.active_cohort_count();
		result.phase = engine.phase_cache_stats();
		result.diagnostics = stream.diagnostics();
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
		render_error(result.diagnostics, "exception during streamed SMF render");
		return false;
	}
}

} // namespace safsyn
