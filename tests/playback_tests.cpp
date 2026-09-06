#include "playback.h"
#include "playback_queue.h"

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
void check(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
template<class Condition> void await(Condition condition)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!condition())
	{
		check(std::chrono::steady_clock::now() < deadline, "playback wait timed out");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
std::shared_ptr<safsyn::Soundfont> bank()
{
	auto result = std::make_shared<safsyn::Soundfont>();
	result->sfz_pcm.emplace_back(128);
	for (size_t i = 0; i < 128; ++i) result->sfz_pcm[0][i] = static_cast<int16_t>(std::sin(i * 0.1) * 100);
	safsyn::SampleRegion region;
	region.pcm = result->sfz_pcm[0].data(); region.pcm_len = 128; region.sample_rate = 48000;
	region.loop_mode = safsyn::LoopMode::Forward; region.loop_end = 128;
	region.attack = 0; region.release = 0.002f;
	result->regions.push_back(region); return result;
}
void append32(std::vector<uint8_t>& bytes, uint32_t value)
{ for (int shift : {24, 16, 8, 0}) bytes.push_back(static_cast<uint8_t>(value >> shift)); }
std::shared_ptr<safsyn::SmfFile> file(size_t duplicates, bool distinct = false)
{
	std::vector<uint8_t> bytes{'M','T','h','d',0,0,0,6,0,0,0,1,3,232,'M','T','r','k'};
	std::vector<uint8_t> track;
	for (size_t i = 0; i < duplicates; ++i)
	{
		track.push_back(0); track.push_back(static_cast<uint8_t>(0x90 | (distinct ? i / 128 : 0)));
		track.push_back(static_cast<uint8_t>(distinct ? i % 128 : 60)); track.push_back(100);
	}
	for (size_t i = 0; i < duplicates; ++i)
	{
		track.push_back(i == 0 ? 2 : 0); track.push_back(static_cast<uint8_t>(0x80 | (distinct ? i / 128 : 0)));
		track.push_back(static_cast<uint8_t>(distinct ? i % 128 : 60)); track.push_back(0);
	}
	track.insert(track.end(), {1, 0xff, 0x2f, 0});
	append32(bytes, static_cast<uint32_t>(track.size())); bytes.insert(bytes.end(), track.begin(), track.end());
	auto result = std::make_shared<safsyn::SmfFile>(); check(result->load_bytes(std::move(bytes)), "fixture load");
	return result;
}
safsyn::PlaybackOptions options()
{
	safsyn::PlaybackOptions result;
	result.render_threads = 1; result.block_frames = 64; result.buffer_frames = 256;
	result.mastering = {}; return result;
}
std::vector<float> collect(safsyn::BufferedSynth& synth)
{
	std::vector<float> output;
	std::array<float, 74> part{};
	await([&] { return synth.ready(); });
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!synth.drained())
	{
		const auto frames = synth.read_audio(part.data(), 37);
		output.insert(output.end(), part.begin(), part.begin() + frames * 2);
		check(std::chrono::steady_clock::now() < deadline, "audio collection timed out");
		if (!frames) std::this_thread::yield();
	}
	check(synth.stats().error.empty(), "playback error");
	return output;
}
void queue_test()
{
	safsyn::detail::MidiQueue<uint64_t> queue(127);
	constexpr size_t producers = 4, count = 25000;
	std::array<std::thread, producers> writers;
	for (size_t p = 0; p < producers; ++p) writers[p] = std::thread([&, p] {
		for (size_t i = 0; i < count; ++i)
			while (!queue.push(p * count + i)) std::this_thread::yield();
	});
	std::array<size_t, producers> expected{};
	bool correct = true;
	for (size_t i = 0; i < producers * count;)
	{
		uint64_t item = 0;
		if (!queue.pop(item)) { std::this_thread::yield(); continue; }
		if (item >= producers * count) correct = false;
		else if (expected[item / count]++ != item % count) correct = false;
		++i;
	}
	for (auto& writer : writers) writer.join();
	check(correct, "concurrent MIDI producers lost, duplicated, or reordered events");
	uint64_t item = 0; check(!queue.pop(item), "queue should be empty");
}
void ring_test()
{
	safsyn::detail::AudioRing ring(127);
	std::atomic<bool> correct{true};
	constexpr size_t frames = 200000;
	std::thread writer([&] {
		std::array<float, 38> block{};
		for (size_t position = 0; position < frames;)
		{
			const size_t count = (std::min)(size_t{19}, frames - position);
			for (size_t i = 0; i < count; ++i) { block[2*i] = float(position+i); block[2*i+1] = -float(position+i); }
			if (ring.write(block.data(), count)) position += count; else std::this_thread::yield();
		}
	});
	std::array<float, 62> block{};
	for (size_t position = 0; position < frames;)
	{
		const size_t count = ring.read(block.data(), 31);
		for (size_t i = 0; i < count; ++i)
			if (block[2*i] != float(position+i) || block[2*i+1] != -float(position+i)) correct.store(false);
		for (size_t i = count * 2; i < block.size(); ++i) if (block[i] != 0) correct.store(false);
		position += count;
	}
	writer.join(); check(correct.load(), "audio ring wrap/order/silence failure");
}
void file_timing_test()
{
	auto source = bank(); auto midi = file(51911);
	auto config = options(); config.midi_queue_capacity = 2; config.mastering.output_gain_db = -60;
	safsyn::BufferedSynth synth(source, config, midi); synth.start();
	const auto audio = collect(synth);
	const auto stats = synth.stats();
	check(stats.engine.logical_voices_started == 51911, "file burst lost note-ons");
	check(stats.scheduled_events == 51911 * 2 + 1, "file events dropped");
	check(stats.engine.cohort_capacity_steals == 0 && stats.rejected_midi_events == 0, "file burst used live queue");
	check(stats.active_voices == 0 && stats.active_cohorts == 0, "file note-offs left stuck voices");
	safsyn::SynthEngine reference(48000, 1); reference.set_soundfont(source.get());
	reference.set_voice_model(safsyn::VoiceModel::Cohorts, 4096);
	std::vector<float> expected(audio.size());
	reference.note_on_batch(0, 60, 100, 51911); reference.render_audio(expected.data(), 48);
	reference.note_off_batch(0, 60, 51911); reference.render_audio(expected.data() + 96, static_cast<uint32_t>(expected.size()/2 - 48));
	check(audio.size() >= 288, "release tail truncated");
	for (size_t i = 0; i < audio.size(); ++i)
		check(std::abs(audio[i] - expected[i] * 0.001f) < 1e-6f, "file event timing changed at block boundary");
	config.block_frames = 113; config.buffer_frames = 512;
	safsyn::BufferedSynth repartitioned(source, config, midi); repartitioned.start();
	const auto other = collect(repartitioned);
	for (size_t i = 0; i < (std::min)(audio.size(), other.size()); ++i)
		check(audio[i] == other[i], "scalar sample sequence changes with producer block size");
}
void parallel_test()
{
	auto config = options(); config.render_threads = 4; config.mastering.output_gain_db = -12;
	auto source = bank(); auto midi = file(512, true);
	safsyn::BufferedSynth first(source, config, midi); first.start(); const auto a = collect(first);
	safsyn::BufferedSynth second(source, config, midi); second.start(); const auto b = collect(second);
	check(first.stats().render_threads == 4 && first.stats().engine.parallel_render_calls > 0, "cohort worker pool not used");
	check(a == b, "fixed-thread playback is not deterministic");
	config.render_threads = 1;
	safsyn::BufferedSynth scalar(source, config, midi); scalar.start(); const auto c = collect(scalar);
	check(a.size() == c.size(), "parallel playback changes duration");
	for (size_t i = 0; i < a.size(); ++i) check(std::abs(a[i] - c[i]) < 1e-5f, "parallel audio differs from scalar");
}
void live_recovery_test()
{
	auto config = options(); config.midi_queue_capacity = 4;
	safsyn::BufferedSynth synth(bank(), config);
	for (size_t i = 0; i < 4; ++i) check(synth.enqueue_short_message(0x00643c90), "queue rejected early");
	check(!synth.enqueue_short_message(0x00003c80), "full live queue did not report failure");
	synth.start(); await([&] { return synth.ready(); });
	std::vector<float> block(2048);
	synth.read_audio(block.data(), 1024);
	check(synth.stats().underruns > 0 && synth.stats().underrun_frames >= 768, "starvation was not reported");
	check(synth.stats().rejected_midi_events == 1, "MIDI overflow count");
	for (float sample : block) check(sample == 0, "overflow replayed old note-ons without note-offs");
	check(synth.enqueue_short_message(0x00643c90), "recovery rejected new event");
	await([&] { synth.read_audio(block.data(), 128); return synth.stats().active_voices > 0; });
	synth.panic();
	await([&] { synth.read_audio(block.data(), 128); return synth.stats().midi_recoveries > 0 && synth.stats().active_voices == 0; });
	synth.stop(); check(synth.finished(), "stop did not join producer");
	check(!synth.enqueue_short_message(0x00643c90), "stopped synth accepts events");
}
void file_sender_backpressure_test()
{
	using Result = safsyn::MidiEnqueueResult;
	auto config = options(); config.midi_queue_capacity = 4;
	safsyn::BufferedSynth synth(bank(), config);
	check(synth.try_enqueue_short_message(0x00643c90) == Result::Queued, "file sender note rejected");
	synth.start(); await([&] { return synth.ready(); });
	check(synth.stats().active_voices == 1, "held carrier did not start");
	// The undrained audio ring keeps the producer blocked. Saturate the MIDI
	// queue with automation while that carrier is still held.
	for (uint32_t message : {0x000007b0u, 0x007f07b0u, 0x00400bb0u, 0x007f0ab0u})
		check(synth.try_enqueue_short_message(message) == Result::Queued, "automation rejected early");
	constexpr uint32_t bend = 0x007f7fe0;
	for (size_t i = 0; i < 32; ++i)
		check(synth.try_enqueue_short_message(bend) == Result::Full, "full queue did not request retry");
	std::array<float, 128> audio{};
	await([&] {
		synth.read_audio(audio.data(), 64);
		return synth.stats().scheduled_events == 5;
	});
	check(synth.try_enqueue_short_message(bend) == Result::Queued, "file event retry failed");
	await([&] {
		synth.read_audio(audio.data(), 64);
		return synth.stats().scheduled_events == 6;
	});
	bool heard_panned_carrier = false;
	for (size_t i = 0; i < 32; ++i)
	{
		await([&] { return synth.stats().buffered_frames >= 64; });
		synth.read_audio(audio.data(), 64);
		double left = 0, right = 0;
		for (size_t frame = 0; frame < 64; ++frame)
		{
			check(std::isfinite(audio[frame * 2]) && std::isfinite(audio[frame * 2 + 1]),
				"retried automation produced non-finite audio");
			left += std::abs(audio[frame * 2]); right += std::abs(audio[frame * 2 + 1]);
		}
		// CC10=127 leaves the fine-pan LSB at zero, just short of hard right.
		heard_panned_carrier |= right > 0.001 && left < right * 0.02;
	}
	const auto stats = synth.stats();
	check(heard_panned_carrier && stats.active_voices == 1,
		"queue saturation erased the held note or its later automation");
	check(stats.rejected_midi_events == 0 && stats.midi_recoveries == 0 && stats.scheduled_events == 6,
		"retrying a full queue lost, duplicated, or invalidated MIDI events");
	check(synth.try_enqueue_short_message(0x00003c80) == Result::Queued, "final note-off rejected");
	await([&] { synth.read_audio(audio.data(), 64); return synth.stats().active_voices == 0; });
	synth.request_stop();
	check(synth.try_enqueue_short_message(bend) == Result::Unavailable,
		"stopped sender must terminate retries instead of reporting full");
	synth.stop();
	safsyn::BufferedSynth scheduled(bank(), config, file(1));
	check(scheduled.try_enqueue_short_message(bend) == Result::Unavailable,
		"direct SMF playback accepted live file-sender input");
}
void phase_preparation_test()
{
	for (auto mode : {safsyn::PhaseMode::Coherent, safsyn::PhaseMode::RandomPolarity,
		safsyn::PhaseMode::Analytic, safsyn::PhaseMode::SmoothField, safsyn::PhaseMode::IndependentBins})
	{
		auto config = options(); config.phase.mode = mode; config.phase.pool_size = 4;
		auto source = bank();
		safsyn::BufferedSynth synth(source, config, file(32)); synth.start();
		const auto audio = collect(synth); const auto stats = synth.stats();
		check(stats.preparation.completed == stats.preparation.total && !stats.preparing,
			"playback started without completing phase preparation");
		check(stats.phase.cache_bytes == stats.preparation.total_cache_bytes && stats.phase.failures == 0,
			"playback phase cache was not fully prepared");
		safsyn::SynthEngine reference(48000, 1); reference.set_soundfont(source.get());
		reference.set_voice_model(safsyn::VoiceModel::Cohorts, config.maximum_cohorts); reference.set_phase_settings(config.phase);
		std::vector<float> expected(audio.size());
		reference.note_on_batch(0, 60, 100, 32); reference.render_audio(expected.data(), 48);
		reference.note_off_batch(0, 60, 32); reference.render_audio(expected.data() + 96, static_cast<uint32_t>(expected.size() / 2 - 48));
		check(audio == expected, "startup preparation changed the first notes or MIDI timing");
	}
	auto config = options(); config.phase.mode = safsyn::PhaseMode::Analytic; config.maximum_phase_cache_bytes = 1;
	safsyn::BufferedSynth failed(bank(), config, file(32)); failed.start(); await([&] { return failed.finished(); });
	check(!failed.stats().error.empty() && failed.stats().scheduled_events == 0 && failed.stats().phase.cache_bytes == 0,
		"phase budget failure must stop before dispatching MIDI or allocating phase PCM");
	check(failed.ready() && failed.drained(), "preparation failure did not terminate cleanly");
	safsyn::BufferedSynth cancelled(bank(), options(), file(32)); cancelled.request_stop(); cancelled.start();
	await([&] { return cancelled.finished(); });
	check(cancelled.stats().error.empty() && cancelled.stats().scheduled_events == 0 && cancelled.drained(),
		"cancelled startup processed MIDI or reported an error");
	config = options(); config.phase.mode = safsyn::PhaseMode::RandomPolarity;
	config.phase.strength = NAN; config.phase.pool_size = 0; config.phase.correlation_hz = NAN;
	safsyn::BufferedSynth inactive(bank(), config, file(32)); inactive.start(); collect(inactive);
	check(inactive.stats().phase.cache_bytes == 0, "inactive phase settings were validated or used");
}
void lifecycle_and_limiter_test()
{
	for (size_t i = 0; i < 8; ++i)
	{
		auto config = options(); config.render_threads = 4;
		safsyn::BufferedSynth synth(bank(), config); synth.start();
		await([&] { return synth.ready(); }); synth.stop();
	}
	auto config = options(); config.mastering = {-3, true, -1, 5, 100};
	safsyn::BufferedSynth loud(bank(), config, file(51911)); loud.start();
	const auto audio = collect(loud);
	for (float sample : audio) check(std::isfinite(sample) && std::abs(sample) <= 0.891252f, "live limiter exceeded ceiling");
	check(loud.stats().safety_clamped_samples == 0, "limiter relied on hard clamp");
	config.maximum_cohorts = 0; bool rejected = false;
	try { safsyn::BufferedSynth invalid(bank(), config); } catch (const std::invalid_argument&) { rejected = true; }
	check(rejected, "unbounded live cohort capacity was accepted");
	std::vector<uint8_t> malformed{'M','T','h','d',0,0,0,6,0,0,0,1,3,232,
		'M','T','r','k',0,0,0,3,0,0x90,60};
	auto broken = std::make_shared<safsyn::SmfFile>();
	check(broken->load_bytes(std::move(malformed)), "malformed fixture chunk load");
	safsyn::BufferedSynth failed(bank(), options(), broken); failed.start();
	await([&] { return failed.finished(); });
	check(!failed.stats().error.empty() && failed.ready() && failed.drained(), "stream failure did not terminate cleanly");
}
}
int main()
{
	try
	{
		queue_test(); ring_test(); file_timing_test(); parallel_test(); live_recovery_test(); lifecycle_and_limiter_test();
		file_sender_backpressure_test(); phase_preparation_test();
		std::cout << "Playback: concurrent queues, sample timing, dense bursts, workers, overflow, underruns, limiter, lifecycle passed\n";
		return 0;
	}
	catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
