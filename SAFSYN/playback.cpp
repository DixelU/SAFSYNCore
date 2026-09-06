#include "playback.h"
#include "playback_queue.h"

#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace safsyn
{
namespace
{
PlaybackOptions validated(PlaybackOptions options)
{
	if (options.sample_rate < 8000 || options.sample_rate > 192000 ||
		options.block_frames < 16 || options.block_frames > 8192 ||
		options.buffer_frames < options.block_frames * 2 ||
		options.buffer_frames > options.sample_rate * 30 ||
		options.maximum_cohorts == 0 || options.maximum_cohorts > 1048576 ||
		options.render_threads > 64 || options.midi_queue_capacity < 2 ||
		options.midi_queue_capacity > 4194304 || options.initial_bank > 16383 ||
		options.initial_program > 127 || options.maximum_tail_seconds > 60 ||
		static_cast<unsigned>(options.phase.mode) > static_cast<unsigned>(PhaseMode::IndependentBins) ||
		!mastering_settings_valid(options.mastering))
		throw std::invalid_argument("invalid live playback options");
	const bool active = options.phase.mode != PhaseMode::Coherent;
	const bool transform = active && options.phase.mode != PhaseMode::RandomPolarity;
	const bool pool = transform && !(options.phase.mode == PhaseMode::Analytic && options.phase.continuous);
	if ((transform && (!std::isfinite(options.phase.strength) || options.phase.strength < 0 || options.phase.strength > 1)) ||
		(active && (!std::isfinite(options.phase.preserve_attack_ms) || options.phase.preserve_attack_ms < 0 || options.phase.preserve_attack_ms > 10000)) ||
		(pool && (options.phase.pool_size < 1 || options.phase.pool_size > 64)) ||
		(options.phase.mode == PhaseMode::SmoothField && (!std::isfinite(options.phase.correlation_hz) ||
			options.phase.correlation_hz < 0.001f || options.phase.correlation_hz > 1000000)))
		throw std::invalid_argument("invalid playback phase settings");
	if (!active) options.phase = {};
	if (!transform) options.phase.strength = 1;
	if (!pool) options.phase.pool_size = 1;
	if (options.phase.mode != PhaseMode::SmoothField) options.phase.correlation_hz = 250;
	if (options.render_threads == 0)
	{
		const auto cpus = std::thread::hardware_concurrency();
		options.render_threads = (std::min)(size_t{16}, size_t{cpus > 2 ? cpus - 2 : 1});
	}
	return options;
}
}

struct BufferedSynth::Impl
{
	struct MidiEvent { uint64_t generation; uint32_t message; bool master; };
	std::shared_ptr<const Soundfont> bank;
	std::shared_ptr<const SmfFile> midi;
	PlaybackOptions options;
	detail::AudioRing audio;
	detail::MidiQueue<MidiEvent> events;
	std::thread producer;
	std::atomic<bool> stopping{false}, ready{false}, finished{false};
	std::stop_source preparation_stop;
	std::atomic<uint64_t> generation{0}, rejected{0}, consumed{0}, underruns{0}, missing{0};
	mutable std::mutex stats_mutex;
	PlaybackStats snapshot;
	std::mutex wait_mutex;
	std::condition_variable wake;
	bool started = false;

	Impl(std::shared_ptr<const Soundfont> source, const PlaybackOptions& settings,
		std::shared_ptr<const SmfFile> file) : bank(std::move(source)), midi(std::move(file)),
		options(validated(settings)), audio(options.buffer_frames), events(options.midi_queue_capacity)
	{
		if (!bank || bank->regions.empty() || (midi && !midi->valid()))
			throw std::invalid_argument("playback needs a valid sound bank and MIDI file");
	}
	void defaults(SynthEngine& engine)
	{
		for (uint8_t channel = 0; channel < 16; ++channel)
		{
			engine.control_change(channel, 0, static_cast<uint8_t>(options.initial_bank >> 7));
			engine.control_change(channel, 32, static_cast<uint8_t>(options.initial_bank & 127));
			engine.program_change(channel, options.initial_program);
		}
	}
	void publish(const PlaybackStats& current)
	{
		std::lock_guard lock(stats_mutex);
		snapshot = current;
	}
	bool push_audio(const float* samples, uint32_t frames)
	{
		uint32_t cursor = 0;
		while (cursor < frames && !stopping.load())
		{
			const auto count = static_cast<uint32_t>((std::min)(size_t{frames - cursor}, audio.capacity()));
			if (audio.write(samples + cursor * 2, count)) cursor += count;
			else
			{
				ready.store(true, std::memory_order_release);
				std::unique_lock lock(wait_mutex);
				wake.wait_for(lock, std::chrono::milliseconds(1), [&] { return stopping.load(); });
			}
		}
		if (audio.size() >= audio.capacity() - options.block_frames)
			ready.store(true, std::memory_order_release);
		return !stopping.load();
	}
	void run() noexcept
	{
		PlaybackStats current;
		try
		{
			current.preparing = true;
			publish(current);
			const auto preparation_started = std::chrono::steady_clock::now();
			SynthEngine engine(options.sample_rate, 1);
			engine.set_voice_model(VoiceModel::Cohorts, options.maximum_cohorts);
			engine.set_render_threads(options.render_threads);
			engine.set_soundfont(bank.get());
			engine.set_phase_settings(options.phase);
			defaults(engine);
			current.render_threads = engine.render_threads();
			PhasePreparationOptions prepare;
			prepare.maximum_cache_bytes = options.maximum_phase_cache_bytes;
			prepare.stop = preparation_stop.get_token();
			prepare.progress = [&](const PhasePreparationProgress& progress) {
				current.preparation = progress;
				current.phase = engine.phase_cache_stats();
				current.preparation_ms = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - preparation_started).count();
				publish(current);
			};
			if (!engine.prepare_playback(options.block_frames, prepare)) stopping.store(true);
			current.preparation_ms = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - preparation_started).count();
			current.preparing = false;
			current.phase = engine.phase_cache_stats();
			publish(current);
			StereoMasteringProcessor mastering(options.sample_rate, options.mastering);
			std::vector<float> block(options.block_frames * 2), output;
			output.reserve(options.block_frames * 2 + static_cast<size_t>(options.sample_rate / 10) * 2);
			std::array<uint32_t, 4096> batch{};
			std::optional<ScheduledSmfStream> stream;
			ScheduledSmfEvent next;
			bool has_event = false, tail = false;
			uint64_t cursor = 0, tail_end = 0, applied_generation = generation.load();
			if (midi) { stream.emplace(*midi, options.sample_rate); has_event = stream->next(next); }
			auto check_stream = [&] {
				if (stream && !stream->good())
					throw std::runtime_error(stream->diagnostics().empty() ?
						"MIDI stream failed" : stream->diagnostics().front().message);
			};
			auto sanitize = [&] {
				for (float& sample : output)
				{
					if (!std::isfinite(sample)) { sample = 0; ++current.safety_clamped_samples; }
					else if (std::abs(sample) > 1.0f)
					{
						sample = std::clamp(sample, -1.0f, 1.0f);
						++current.safety_clamped_samples;
					}
				}
			};
			while (!stopping.load())
			{
				// Do not advance the synth when the output ring is full. Live input
				// therefore has a bounded render-ahead delay, independent of SMF size.
				if (audio.size() > audio.capacity() - options.block_frames)
				{
					std::unique_lock lock(wait_mutex);
					wake.wait_for(lock, std::chrono::milliseconds(1), [&] { return stopping.load(); });
					continue;
				}
				const auto started_at = std::chrono::steady_clock::now();
				if (!midi)
				{
					const auto requested = generation.load(std::memory_order_acquire);
					if (requested != applied_generation)
					{
						engine.reset(); defaults(engine);
						mastering = StereoMasteringProcessor(options.sample_rate, options.mastering);
						applied_generation = requested;
						++current.midi_recoveries;
					}
					// Bound dispatch per block so a runaway live producer cannot starve
					// audio forever. File bursts use the separate exact scheduler below.
					size_t count = 0;
					MidiEvent event{};
					for (size_t i = 0; i < 65536 && !stopping.load() && events.pop(event); ++i)
					{
						if (event.generation != applied_generation) continue;
						if (event.master)
						{
							engine.consume_short_messages(batch.data(), count); count = 0;
							engine.set_master_volume(static_cast<uint16_t>(event.message));
						}
						else batch[count++] = event.message;
						if (count == batch.size())
						{ engine.consume_short_messages(batch.data(), count); count = 0; }
						++current.scheduled_events;
					}
					engine.consume_short_messages(batch.data(), count);
				}
				uint32_t frames = 0;
				while (frames < options.block_frames && !stopping.load())
				{
					if (midi && has_event && next.sample <= cursor)
					{
						size_t count = 0;
						while (has_event && next.sample <= cursor && !stopping.load())
						{
							++current.scheduled_events;
							if (next.event.kind == SmfEventKind::Channel)
								batch[count++] = next.event.status | (uint32_t{next.event.data1} << 8) |
									(uint32_t{next.event.data2} << 16);
							else if (next.event.kind == SmfEventKind::SystemExclusive)
							{
								engine.consume_short_messages(batch.data(), count); count = 0;
								uint16_t volume = 0;
								if (decode_universal_master_volume(*midi, next.event, volume))
									engine.set_master_volume(volume);
							}
							if (count == batch.size())
							{ engine.consume_short_messages(batch.data(), count); count = 0; }
							has_event = stream->next(next);
						}
						engine.consume_short_messages(batch.data(), count);
					}
					if (midi && !has_event && !tail)
					{
						check_stream();
						tail = true;
						tail_end = cursor + uint64_t{options.maximum_tail_seconds} * options.sample_rate;
						for (uint8_t ch = 0; ch < 16; ++ch)
						{ engine.control_change(ch, 64, 0); engine.control_change(ch, 123, 0); }
					}
					if (tail && (cursor >= tail_end || engine.active_voice_count() == 0)) break;
					uint32_t count = options.block_frames - frames;
					if (has_event) count = static_cast<uint32_t>((std::min)(uint64_t{count}, next.sample - cursor));
					if (tail) count = static_cast<uint32_t>((std::min)(uint64_t{count}, tail_end - cursor));
					engine.render_audio(block.data() + frames * 2, count);
					frames += count; cursor += count;
				}
				for (size_t i = 0; i < frames * 2; ++i)
					current.raw_peak = (std::max)(current.raw_peak, std::abs(block[i]));
				output.clear();
				mastering.process(block.data(), frames, output);
				sanitize();
				current.engine = engine.stats();
				current.phase = engine.phase_cache_stats();
				current.active_voices = engine.active_voice_count();
				current.active_cohorts = engine.active_cohort_count();
				if (frames) current.render_load = std::chrono::duration<double>(
					std::chrono::steady_clock::now() - started_at).count() * options.sample_rate / frames;
				publish(current);
				if (!output.empty() && !push_audio(output.data(), static_cast<uint32_t>(output.size() / 2))) break;
				if (tail && (cursor >= tail_end || engine.active_voice_count() == 0))
				{
					output.clear(); mastering.finish(output); sanitize();
					if (!output.empty()) push_audio(output.data(), static_cast<uint32_t>(output.size() / 2));
					break;
				}
			}
		}
		catch (const std::exception& e) { current.error = e.what(); }
		catch (...) { current.error = "unknown playback failure"; }
		current.preparing = false;
		publish(current);
		finished.store(true, std::memory_order_release);
		ready.store(true, std::memory_order_release);
	}
	MidiEnqueueResult try_enqueue(uint32_t message, bool master) noexcept
	{
		if (midi || stopping.load() || finished.load()) return MidiEnqueueResult::Unavailable;
		return events.push({generation.load(std::memory_order_acquire), message, master})
			? MidiEnqueueResult::Queued : MidiEnqueueResult::Full;
	}
	bool enqueue(uint32_t message, bool master) noexcept
	{
		const auto result = try_enqueue(message, master);
		if (result == MidiEnqueueResult::Queued) return true;
		if (result == MidiEnqueueResult::Unavailable) return false;
		rejected.fetch_add(1, std::memory_order_relaxed);
		generation.fetch_add(1, std::memory_order_release);
		return false;
	}
};

BufferedSynth::BufferedSynth(std::shared_ptr<const Soundfont> bank, const PlaybackOptions& options,
	std::shared_ptr<const SmfFile> midi) : impl_(std::make_unique<Impl>(std::move(bank), options, std::move(midi))) {}
BufferedSynth::~BufferedSynth() { stop(); }
void BufferedSynth::start()
{
	if (impl_->started) throw std::logic_error("create a new synth for each playback session");
	impl_->started = true;
	impl_->producer = std::thread([this] { impl_->run(); });
}
void BufferedSynth::request_stop() noexcept
{
	impl_->stopping.store(true); impl_->preparation_stop.request_stop(); impl_->wake.notify_all();
}
void BufferedSynth::stop() noexcept { request_stop(); if (impl_->producer.joinable()) impl_->producer.join(); }
bool BufferedSynth::enqueue_short_message(uint32_t message) noexcept { return impl_->enqueue(message, false); }
MidiEnqueueResult BufferedSynth::try_enqueue_short_message(uint32_t message) noexcept
{ return impl_->try_enqueue(message, false); }
bool BufferedSynth::enqueue_master_volume(uint16_t value) noexcept { return impl_->enqueue(value & 16383, true); }
void BufferedSynth::panic() noexcept { impl_->generation.fetch_add(1, std::memory_order_release); }
void BufferedSynth::report_input_loss() noexcept { impl_->rejected.fetch_add(1); panic(); }
uint32_t BufferedSynth::read_audio(float* stereo, uint32_t frames) noexcept
{
	if (!ready()) { std::fill_n(stereo, size_t{frames} * 2, 0.0f); return 0; }
	const auto count = static_cast<uint32_t>(impl_->audio.read(stereo, frames));
	impl_->consumed.fetch_add(count, std::memory_order_relaxed);
	if (count < frames && !finished())
	{
		impl_->underruns.fetch_add(1, std::memory_order_relaxed);
		impl_->missing.fetch_add(frames - count, std::memory_order_relaxed);
	}
	return count;
}
bool BufferedSynth::ready() const noexcept { return impl_->ready.load(std::memory_order_acquire); }
bool BufferedSynth::finished() const noexcept { return impl_->finished.load(std::memory_order_acquire); }
bool BufferedSynth::drained() const noexcept { return finished() && impl_->audio.size() == 0; }
PlaybackStats BufferedSynth::stats() const
{
	PlaybackStats result;
	{ std::lock_guard lock(impl_->stats_mutex); result = impl_->snapshot; }
	result.consumed_frames = impl_->consumed.load();
	result.underruns = impl_->underruns.load();
	result.underrun_frames = impl_->missing.load();
	result.rejected_midi_events = impl_->rejected.load();
	result.buffered_frames = impl_->audio.size();
	result.ready = ready(); result.finished = finished();
	return result;
}

} // namespace safsyn
