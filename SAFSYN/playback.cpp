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
// Audio ring fill at which note shedding starts and at which it is total.
constexpr double shed_start_fill = 0.5;
constexpr double shed_full_fill = 0.125;

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
		options.live_event_grid_frames > options.block_frames ||
		static_cast<unsigned>(options.phase.mode) > static_cast<unsigned>(PhaseMode::Analytic) ||
		!mastering_settings_valid(options.mastering))
		throw std::invalid_argument("invalid live playback options");
	const bool analytic = options.phase.mode == PhaseMode::Analytic;
	const bool pool = analytic && !options.phase.continuous;
	if ((analytic && (!std::isfinite(options.phase.strength) || options.phase.strength < 0 || options.phase.strength > 1)) ||
		(analytic && (!std::isfinite(options.phase.preserve_attack_ms) || options.phase.preserve_attack_ms < 0 || options.phase.preserve_attack_ms > 10000)) ||
		(pool && (options.phase.pool_size < 1 || options.phase.pool_size > 64)))
		throw std::invalid_argument("invalid playback phase settings");
	if (!analytic) options.phase = {};
	if (!pool) options.phase.pool_size = 1;
	if (options.render_threads == 0)
	{
		const auto cpus = std::thread::hardware_concurrency();
		options.render_threads = (std::min)(size_t{64}, size_t{cpus > 2 ? cpus - 2 : 1});
	}
	return options;
}
}

struct BufferedSynth::Impl
{
	struct MidiEvent
	{
		uint64_t generation;
		uint32_t message;
		bool master;
		std::optional<uint64_t> tick;
		MidiClock::time_point due;
	};
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
	// The consumer's latest stream position for live event timing: consumed
	// frames minus seconds since `epoch` times the rate. Resets count reads
	// that came up short, after which the stream is no longer where it was.
	const MidiClock::time_point epoch = MidiClock::now();
	std::atomic<double> audio_clock{0.0};
	std::atomic<uint64_t> audio_clock_reports{0}, audio_clock_resets{0};

	double seconds_since_epoch(MidiClock::time_point time) const noexcept
	{
		return std::chrono::duration<double>(time - epoch).count();
	}

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
			detail::NoteGate gate;
			std::optional<uint64_t> batch_tick;
			auto dispatch = [&](size_t& count) {
				engine.consume_short_messages(batch.data(), count, batch_tick);
				count = 0;
			};
			std::optional<ScheduledSmfStream> stream;
			ScheduledSmfEvent next;
			bool has_event = false, tail = false;
			uint64_t cursor = 0, tail_end = 0, applied_generation = generation.load();

			// Live events wait here, in queue order, for the frame they play at.
			struct PendingEvent
			{
				uint64_t frame;
				std::optional<uint64_t> tick;
				uint32_t message;
				bool master;
			};
			const size_t pending_limit = options.midi_queue_capacity * 8;
			detail::PendingQueue<PendingEvent> pending(pending_limit);
			const bool timed = options.live_event_grid_frames != 0;
			// The producer can be ahead of the consumer by the whole ring plus
			// the limiter lookahead, so that is the fixed part of the delay.
			detail::LiveTimeline timeline(options.sample_rate,
				static_cast<double>(audio.capacity() + mastering.stats().lookahead_frames),
				options.block_frames, static_cast<double>(audio.capacity()));
			// Every distinct event frame splits rendering, and each render call
			// visits every active cohort. The grid that events are rounded down to
			// therefore coarsens while the producer is busy, towards one render
			// call per block, and refines once it has been idle for a while.
			uint32_t grid = options.live_event_grid_frames;
			double load_average = 0.0;
			uint64_t grid_held_frames = 0;
			auto grid_frame = [&](uint64_t frame) {
				return timed ? frame - frame % grid : frame;
			};
			uint64_t clock_resets = 0;
			auto sync_generation = [&] {
				const auto requested = generation.load(std::memory_order_acquire);
				if (requested == applied_generation)
					return;
				engine.reset(); defaults(engine);
				mastering = StereoMasteringProcessor(options.sample_rate, options.mastering);
				applied_generation = requested;
				gate.clear();
				pending.clear();
				timeline.restart();
				++current.midi_recoveries;
			};
			// Moves queued live events into `pending`, each placed on the stream.
			// The pending bound turns a sender that outruns dispatch back into
			// queue backpressure.
			auto drain_live = [&] {
				sync_generation();
				if (timed && audio_clock_reports.load(std::memory_order_acquire) != 0)
				{
					const uint64_t resets = audio_clock_resets.load(std::memory_order_acquire);
					timeline.observe(audio_clock.load(std::memory_order_acquire),
						seconds_since_epoch(MidiClock::now()), resets != clock_resets);
					clock_resets = resets;
				}
				MidiEvent event{};
				while (pending.size() < pending_limit && !stopping.load() && events.pop(event))
				{
					if (event.generation != applied_generation)
					{
						// Older generations were cancelled. A newer one was requested
						// after this producer last looked.
						if (event.generation < applied_generation)
							continue;
						sync_generation();
						if (event.generation != applied_generation)
							continue;
					}
					const uint64_t frame = timed
						? timeline.place(seconds_since_epoch(event.due), cursor) : cursor;
					pending.push_back({frame, event.tick, event.message, event.master});
				}
			};
			// Bound dispatch per block so a runaway live sender cannot starve
			// audio forever; the rest plays from the next block on.
			constexpr size_t live_dispatch_limit = 65536;
			auto dispatch_live = [&](size_t& dispatched) {
				size_t count = 0;
				while (!pending.empty() && grid_frame(pending.front().frame) <= cursor &&
					dispatched < live_dispatch_limit && !stopping.load())
				{
					const PendingEvent& event = pending.front();
					if (event.tick != batch_tick)
					{
						dispatch(count);
						batch_tick = event.tick;
					}
					if (event.master)
					{
						dispatch(count);
						engine.set_master_volume(static_cast<uint16_t>(event.message));
					}
					else if (!options.shed_notes || gate.admit(event.message))
						batch[count++] = event.message;
					if (count == batch.size())
						dispatch(count);
					++current.scheduled_events;
					++dispatched;
					pending.pop_front();
				}
				dispatch(count);
			};
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
					// The upper half of the ring absorbs bursts without loss. Below it
					// the producer is behind, and shedding rises until the ring is
					// nearly empty. Initial buffering is not a deficit.
					if (options.shed_notes && ready.load(std::memory_order_relaxed))
					{
						const double fill = static_cast<double>(audio.size()) / audio.capacity();
						gate.set_pressure(static_cast<float>(
							(shed_start_fill - fill) / (shed_start_fill - shed_full_fill)));
					}
					drain_live();
				}
				size_t live_dispatched = 0;
				uint32_t frames = 0;
				while (frames < options.block_frames && !stopping.load())
				{
					if (!midi)
						dispatch_live(live_dispatched);
					if (midi && has_event && next.sample <= cursor)
					{
						size_t count = 0;
						while (has_event && next.sample <= cursor && !stopping.load())
						{
							if (next.event.tick != batch_tick)
							{
								dispatch(count);
								batch_tick = next.event.tick;
							}
							++current.scheduled_events;
							if (next.event.kind == SmfEventKind::Channel)
								batch[count++] = next.event.status | (uint32_t{next.event.data1} << 8) |
									(uint32_t{next.event.data2} << 16);
							else if (next.event.kind == SmfEventKind::SystemExclusive)
							{
								dispatch(count);
								uint16_t volume = 0;
								if (decode_universal_master_volume(*midi, next.event, volume))
									engine.set_master_volume(volume);
							}
							if (count == batch.size())
								dispatch(count);
							has_event = stream->next(next);
						}
						dispatch(count);
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
					if (!pending.empty() && grid_frame(pending.front().frame) > cursor)
						count = static_cast<uint32_t>((std::min)(uint64_t{count},
							grid_frame(pending.front().frame) - cursor));
					if (tail) count = static_cast<uint32_t>((std::min)(uint64_t{count}, tail_end - cursor));
					engine.render_audio(block.data() + frames * 2, count);
					frames += count; cursor += count;
				}
				if (!midi)
				{
					timeline.advance(frames);
					current.shed_notes = gate.shed_notes();
					current.late_midi_events = timeline.late();
					current.live_margin_ms = timeline.margin_frames() * 1000.0 / options.sample_rate;
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
				if (timed && frames)
				{
					// Falling behind real time goes straight to block-start dispatch;
					// otherwise the grid doubles or halves one step at a time.
					load_average += (current.render_load - load_average) / 8.0;
					grid_held_frames += frames;
					const uint32_t finest = options.live_event_grid_frames;
					const uint32_t coarsest = options.block_frames;
					uint32_t wanted = grid;
					if (load_average > 0.5 && current.render_load > 1.0)
						wanted = coarsest;
					else if (load_average > 0.5 && grid_held_frames >= uint64_t{coarsest} * 2)
						wanted = (std::min)(grid * 2, coarsest);
					else if (load_average < 0.25 && grid_held_frames >= options.sample_rate / 10)
						wanted = (std::max)(grid / 2, finest);
					if (wanted != grid)
					{
						grid = wanted;
						grid_held_frames = 0;
					}
					current.live_grid_frames = grid;
				}
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
	MidiEnqueueResult try_enqueue(uint32_t message, bool master,
		std::optional<uint64_t> tick = {},
		std::optional<MidiClock::time_point> due = {}) noexcept
	{
		if (midi || stopping.load() || finished.load()) return MidiEnqueueResult::Unavailable;
		return events.push({generation.load(std::memory_order_acquire), message, master, tick,
			due ? *due : MidiClock::now()})
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
MidiEnqueueResult BufferedSynth::try_enqueue_short_message(uint32_t message,
	std::optional<uint64_t> tick, std::optional<MidiClock::time_point> due) noexcept
{ return impl_->try_enqueue(message, false, tick, due); }
bool BufferedSynth::enqueue_master_volume(uint16_t value) noexcept { return impl_->enqueue(value & 16383, true); }
void BufferedSynth::panic() noexcept { impl_->generation.fetch_add(1, std::memory_order_release); }
void BufferedSynth::report_input_loss() noexcept { impl_->rejected.fetch_add(1); panic(); }
uint32_t BufferedSynth::read_audio(float* stereo, uint32_t frames) noexcept
{
	if (!ready()) { std::fill_n(stereo, size_t{frames} * 2, 0.0f); return 0; }
	const auto count = static_cast<uint32_t>(impl_->audio.read(stereo, frames));
	const uint64_t consumed = impl_->consumed.fetch_add(count, std::memory_order_relaxed) + count;
	const bool short_read = count < frames && !finished();
	if (short_read)
	{
		impl_->underruns.fetch_add(1, std::memory_order_relaxed);
		impl_->missing.fetch_add(frames - count, std::memory_order_relaxed);
		impl_->audio_clock_resets.fetch_add(1, std::memory_order_relaxed);
	}
	impl_->audio_clock.store(static_cast<double>(consumed) - impl_->options.sample_rate *
		impl_->seconds_since_epoch(MidiClock::now()), std::memory_order_relaxed);
	impl_->audio_clock_reports.fetch_add(1, std::memory_order_release);
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
