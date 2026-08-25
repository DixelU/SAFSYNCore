#include "smf_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
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
		long double sum_squares = 0.0L;
		uint64_t cursor = 0;
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
					result.peak = (std::max)(result.peak, std::abs(value));
					sum_squares += static_cast<long double>(value) * value;
				}
				result.metric_samples += static_cast<uint64_t>(frames) * 2;
				if (!writer.write(block.data(), frames))
					return false;
				cursor += frames;
			}
			return true;
		};

		const uint64_t limit = options.maximum_frames == 0
			? (std::numeric_limits<uint64_t>::max)() : options.maximum_frames;
		ScheduledSmfStream stream(file, options.sample_rate);
		ScheduledSmfEvent scheduled;
		bool has_event = stream.next(scheduled);
		std::vector<uint32_t> sample_messages;
		while (has_event && scheduled.sample < limit)
		{
			const uint64_t event_sample = scheduled.sample;
			if (!render_to(event_sample))
			{
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
				has_event = stream.next(scheduled);
			}
			engine.consume_short_messages(sample_messages.data(), sample_messages.size());
			result.dispatched_channel_events += sample_messages.size();
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
						render_error(result.diagnostics, "failed while draining the SMF release tail");
						return false;
					}
				result.tail_ceiling_reached = engine.active_voice_count() != 0 && cursor == target;
			}
			else if (!render_to(target))
			{
				render_error(result.diagnostics, "failed while writing the SMF release tail");
				return false;
			}
			result.tail_frames_written = cursor - tail_start;
		}

		result.container = writer.container();
		if (!writer.close())
		{
			render_error(result.diagnostics, "could not finalize the streaming WAV header");
			return false;
		}
		result.frames_written = writer.frames_written();
		result.rms = result.metric_samples == 0 ? 0.0 :
			std::sqrt(static_cast<double>(sum_squares / result.metric_samples));
		result.engine = engine.stats();
		result.active_voices_at_end = engine.active_voice_count();
		result.active_cohorts_at_end = engine.active_cohort_count();
		result.phase = engine.phase_cache_stats();
		result.diagnostics = stream.diagnostics();
		return true;
	}
	catch (...)
	{
		render_error(result.diagnostics, "exception during streamed SMF render");
		return false;
	}
}

} // namespace safsyn
