// Replays a real MIDI file against a real sound bank without an audio device.
//
// The default mode drives the engine the way BufferedSynth's live producer does
// (block-quantized dispatch, then one render call per block) and reports where
// each block's time goes. Its numbers are throughput: a realtime factor below
// one means the live producer cannot keep up.
//
// --live runs the real BufferedSynth instead: a sender submits the events at
// their wall-clock times, as a file player does, and a consumer reads audio at
// the device rate. It reports what a listener would get: underruns and how far
// the sender is pushed behind. --shed turns on overload note shedding.

#include "core.h"
#include "playback.h"
#include "smf.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point started)
{
	return std::chrono::duration<double>(Clock::now() - started).count();
}

// Kernel plus user time of every thread, including worker spin-waits.
double process_cpu_seconds()
{
#ifdef _WIN32
	FILETIME creation, exit, kernel, user;
	if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
		return 0.0;
	auto ticks = [](const FILETIME& time) {
		return (uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime;
	};
	return static_cast<double>(ticks(kernel) + ticks(user)) * 1.0e-7;
#else
	return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
#endif
}

struct Options
{
	std::string bank, midi;
	std::vector<size_t> threads{1, 4, 16};
	size_t maximum_cohorts = 4096;
	bool analytic = false;
	uint32_t sample_rate = 48000;
	uint32_t block_frames = 256;
	uint32_t buffer_frames = 4096;
	double start_seconds = 0.0;
	double duration_seconds = 30.0;
	bool timeline = false;
	bool live = false;
	bool shed = false;
};

uint32_t short_message(const safsyn::SmfEvent& event)
{
	return event.status | (uint32_t{event.data1} << 8) | (uint32_t{event.data2} << 16);
}

bool is_note(const safsyn::SmfEvent& event)
{
	return (event.status & 0xf0) == 0x80 || (event.status & 0xf0) == 0x90;
}

int usage()
{
	std::cerr << "usage: safsyn-live-profile <bank.sf2|.sfz> <file.mid> [--threads 1,4,16]\n"
		"       [--cohorts 4096] [--analytic] [--rate 48000] [--block 256]\n"
		"       [--start seconds] [--duration seconds] [--timeline]\n"
		"       [--live [--buffer 4096] [--shed]]   (--threads 0 selects the automatic count)\n";
	return 2;
}

int profile_blocks(const Options& options, const safsyn::Soundfont& bank,
	const safsyn::SmfFile& midi)
{
	struct Totals
	{
		double dispatch = 0.0, render = 0.0, cpu = 0.0, worst_block = 0.0;
		uint64_t blocks = 0, late_blocks = 0, events = 0, parallel_calls = 0;
		size_t peak_cohorts = 0, peak_events = 0;
	};

	safsyn::SynthEngine engine(options.sample_rate, 1);
	engine.set_voice_model(safsyn::VoiceModel::Cohorts, options.maximum_cohorts);
	engine.set_render_threads(*std::max_element(options.threads.begin(), options.threads.end()));
	engine.set_soundfont(&bank);
	if (options.analytic)
	{
		safsyn::PhaseSettings phase;
		phase.mode = safsyn::PhaseMode::Analytic;
		engine.set_phase_settings(phase);
	}
	const auto started = Clock::now();
	engine.prepare_playback(options.block_frames);
	std::cerr << "prepare: " << seconds_since(started) << " s\n";

	const uint64_t first_frame = static_cast<uint64_t>(options.start_seconds * options.sample_rate);
	const uint64_t last_frame = first_frame +
		static_cast<uint64_t>(options.duration_seconds * options.sample_rate);
	const double block_seconds = static_cast<double>(options.block_frames) / options.sample_rate;
	std::vector<float> block(size_t{options.block_frames} * 2);
	std::array<uint32_t, 4096> batch{};
	struct QueuedEvent { uint32_t message; uint64_t tick; };
	std::vector<QueuedEvent> queued;
	queued.reserve(65536);

	std::cout << "threads,audio_s,dispatch_s,render_s,realtime_factor,dispatch_share,"
		"busy_cores,late_blocks,blocks,worst_block_ms,events,peak_block_events,"
		"peak_cohorts,parallel_calls\n";
	for (const size_t threads : options.threads)
	{
		engine.reset();
		engine.set_render_threads(threads);
		engine.prepare_playback(options.block_frames);
		for (uint8_t channel = 0; channel < 16; ++channel)
			engine.program_change(channel, 0);

		safsyn::ScheduledSmfStream stream(midi, options.sample_rate);
		safsyn::ScheduledSmfEvent next;
		bool has_event = stream.next(next);
		// Controllers and programs before the window still shape its sound.
		while (has_event && next.sample < first_frame)
		{
			if (next.event.kind == safsyn::SmfEventKind::Channel && !is_note(next.event))
				engine.consume_short_message(short_message(next.event));
			has_event = stream.next(next);
		}

		Totals totals;
		double second_dispatch = 0.0, second_render = 0.0;
		uint64_t second_events = 0, second_mark = first_frame;
		const uint64_t parallel_before = engine.stats().parallel_render_calls;
		const double cpu_before = process_cpu_seconds();
		for (uint64_t cursor = first_frame; cursor < last_frame && has_event;
			cursor += options.block_frames)
		{
			// Parsing stands in for the sender thread and its queue, so it is not
			// timed. The live producer drains at most this many events per block.
			queued.clear();
			while (has_event && next.sample <= cursor && queued.size() < 65536)
			{
				if (next.event.kind == safsyn::SmfEventKind::Channel)
					queued.push_back({short_message(next.event), next.event.tick});
				has_event = stream.next(next);
			}
			const size_t block_events = queued.size();

			const auto dispatch_started = Clock::now();
			size_t count = 0;
			std::optional<uint64_t> batch_tick;
			auto dispatch = [&] {
				engine.consume_short_messages(batch.data(), count, batch_tick);
				count = 0;
			};
			for (const auto& event : queued)
			{
				if (event.tick != batch_tick)
				{
					dispatch();
					batch_tick = event.tick;
				}
				batch[count++] = event.message;
				if (count == batch.size()) dispatch();
			}
			dispatch();
			const double dispatch_time = seconds_since(dispatch_started);
			totals.peak_cohorts = (std::max)(totals.peak_cohorts, engine.active_cohort_count());

			const auto render_started = Clock::now();
			engine.render_audio(block.data(), options.block_frames);
			const double render_time = seconds_since(render_started);

			totals.dispatch += dispatch_time;
			totals.render += render_time;
			totals.events += block_events;
			totals.peak_events = (std::max)(totals.peak_events, block_events);
			totals.worst_block = (std::max)(totals.worst_block, dispatch_time + render_time);
			totals.late_blocks += dispatch_time + render_time > block_seconds;
			++totals.blocks;
			second_dispatch += dispatch_time;
			second_render += render_time;
			second_events += block_events;
			if (options.timeline && cursor + options.block_frames - second_mark >= options.sample_rate)
			{
				std::cerr << "  t=" << std::fixed << std::setprecision(0)
					<< static_cast<double>(second_mark) / options.sample_rate
					<< std::setprecision(1) << " dispatch_ms=" << second_dispatch * 1000.0
					<< " render_ms=" << second_render * 1000.0 << " events=" << second_events
					<< " cohorts=" << engine.active_cohort_count() << '\n';
				second_dispatch = second_render = 0.0;
				second_events = 0;
				second_mark = cursor + options.block_frames;
			}
		}
		totals.cpu = process_cpu_seconds() - cpu_before;
		totals.parallel_calls = engine.stats().parallel_render_calls - parallel_before;
		const double audio = static_cast<double>(totals.blocks) * block_seconds;
		const double work = totals.dispatch + totals.render;
		std::cout << engine.render_threads() << std::fixed << std::setprecision(3)
			<< ',' << audio << ',' << totals.dispatch << ',' << totals.render
			<< ',' << (work > 0.0 ? audio / work : 0.0)
			<< ',' << (work > 0.0 ? totals.dispatch / work : 0.0)
			<< ',' << (work > 0.0 ? totals.cpu / work : 0.0)
			<< ',' << totals.late_blocks << ',' << totals.blocks
			<< ',' << totals.worst_block * 1000.0 << ',' << totals.events
			<< ',' << totals.peak_events << ',' << totals.peak_cohorts
			<< ',' << totals.parallel_calls << std::endl;
	}
	return 0;
}

// Both threads wait by yielding: timer sleeps are too coarse to stand in for a
// 10 ms device period or for event timestamps.
void wait_until(Clock::time_point deadline)
{
	while (Clock::now() < deadline)
		std::this_thread::yield();
}

int play_live(const Options& options, std::shared_ptr<const safsyn::Soundfont> bank,
	const safsyn::SmfFile& midi)
{
	const uint64_t first_frame = static_cast<uint64_t>(options.start_seconds * options.sample_rate);
	const uint64_t last_frame = first_frame +
		static_cast<uint64_t>(options.duration_seconds * options.sample_rate);
	std::cout << "threads,audio_s,underruns,underrun_ms,sender_late_ms,sender_blocked_ms,"
		"overloaded_share,peak_render_load,events,shed_notes,peak_cohorts\n";
	for (const size_t threads : options.threads)
	{
		safsyn::PlaybackOptions playback;
		playback.sample_rate = options.sample_rate;
		playback.block_frames = options.block_frames;
		playback.buffer_frames = options.buffer_frames;
		playback.maximum_cohorts = options.maximum_cohorts;
		playback.render_threads = threads;
		playback.shed_notes = options.shed;
		if (options.analytic)
			playback.phase.mode = safsyn::PhaseMode::Analytic;
		safsyn::BufferedSynth synth(bank, playback);
		const auto started = Clock::now();
		synth.start();
		while (!synth.ready())
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		if (const auto error = synth.stats().error; !error.empty())
		{
			std::cerr << "playback failed: " << error << '\n';
			return 1;
		}
		std::cerr << "prepare: " << seconds_since(started) << " s\n";

		// A file sender keeps an event until the queue accepts it.
		double blocked = 0.0;
		auto submit = [&](const safsyn::SmfEvent& event) {
			const auto began = Clock::now();
			bool waited = false;
			while (synth.try_enqueue_short_message(short_message(event), event.tick) ==
				safsyn::MidiEnqueueResult::Full)
			{
				waited = true;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (waited)
				blocked += seconds_since(began);
		};
		// The producer only drains its queue while audio is being consumed, so
		// the device runs before the first event is submitted.
		std::atomic<bool> finished{false}, measuring{false};
		uint64_t load_samples = 0, overloaded = 0;
		double peak_load = 0.0;
		size_t peak_cohorts = 0;
		std::thread consumer([&] {
			const uint32_t period = options.sample_rate / 100;
			std::vector<float> audio(size_t{period} * 2);
			const auto device_started = Clock::now();
			for (uint64_t tick = 1; !finished.load(std::memory_order_acquire); ++tick)
			{
				wait_until(device_started + std::chrono::milliseconds(10) * tick);
				synth.read_audio(audio.data(), period);
				if (!measuring.load(std::memory_order_acquire))
					continue;
				const auto stats = synth.stats();
				++load_samples;
				overloaded += stats.render_load > 1.0;
				peak_load = (std::max)(peak_load, stats.render_load);
				peak_cohorts = (std::max)(peak_cohorts, stats.active_cohorts);
			}
		});
		safsyn::ScheduledSmfStream stream(midi, options.sample_rate);
		safsyn::ScheduledSmfEvent next;
		bool has_event = stream.next(next);
		// Controllers and programs before the window still shape its sound.
		while (has_event && next.sample < first_frame)
		{
			if (next.event.kind == safsyn::SmfEventKind::Channel && !is_note(next.event))
				submit(next.event);
			has_event = stream.next(next);
		}
		blocked = 0.0;
		const auto origin = Clock::now() + std::chrono::milliseconds(50);
		const auto before = synth.stats();
		measuring.store(true, std::memory_order_release);
		uint64_t events = 0;
		double latest = 0.0;
		while (has_event && next.sample < last_frame)
		{
			if (next.event.kind == safsyn::SmfEventKind::Channel)
			{
				const auto due = origin + std::chrono::duration_cast<Clock::duration>(
					std::chrono::duration<double>(static_cast<double>(next.sample - first_frame) /
						options.sample_rate));
				wait_until(due);
				submit(next.event);
				latest = (std::max)(latest, std::chrono::duration<double>(Clock::now() - due).count());
				++events;
			}
			has_event = stream.next(next);
		}
		const double audio_seconds = seconds_since(origin);
		finished.store(true, std::memory_order_release);
		consumer.join();
		const auto after = synth.stats();
		synth.stop();
		std::cout << after.render_threads << std::fixed << std::setprecision(3)
			<< ',' << audio_seconds << ',' << after.underruns - before.underruns
			<< ',' << static_cast<double>(after.underrun_frames - before.underrun_frames) *
				1000.0 / options.sample_rate
			<< ',' << latest * 1000.0 << ',' << blocked * 1000.0
			<< ',' << (load_samples ? static_cast<double>(overloaded) / load_samples : 0.0)
			<< ',' << peak_load << ',' << events
			<< ',' << after.shed_notes - before.shed_notes << ',' << peak_cohorts << std::endl;
	}
	return 0;
}
}

int main(int argc, char** argv)
{
	Options options;
	if (argc < 3) return usage();
	options.bank = argv[1];
	options.midi = argv[2];
	for (int index = 3; index < argc; ++index)
	{
		const std::string name = argv[index];
		auto value = [&]() -> std::string { return index + 1 < argc ? argv[++index] : ""; };
		if (name == "--threads")
		{
			options.threads.clear();
			std::stringstream list(value());
			for (std::string item; std::getline(list, item, ',');)
				options.threads.push_back(std::stoull(item));
		}
		else if (name == "--cohorts") options.maximum_cohorts = std::stoull(value());
		else if (name == "--analytic") options.analytic = true;
		else if (name == "--rate") options.sample_rate = static_cast<uint32_t>(std::stoul(value()));
		else if (name == "--block") options.block_frames = static_cast<uint32_t>(std::stoul(value()));
		else if (name == "--buffer") options.buffer_frames = static_cast<uint32_t>(std::stoul(value()));
		else if (name == "--start") options.start_seconds = std::stod(value());
		else if (name == "--duration") options.duration_seconds = std::stod(value());
		else if (name == "--timeline") options.timeline = true;
		else if (name == "--live") options.live = true;
		else if (name == "--shed") options.shed = true;
		else return usage();
	}
	if (options.threads.empty() || !options.block_frames) return usage();

	auto bank = std::make_shared<safsyn::Soundfont>();
	const bool sfz = options.bank.size() > 4 &&
		(options.bank.substr(options.bank.size() - 4) == ".sfz");
	auto started = Clock::now();
	if (!(sfz ? safsyn::load_sfz(options.bank.c_str(), *bank) : safsyn::load_sf2(options.bank.c_str(), *bank)))
	{
		std::cerr << "could not load sound bank\n";
		return 1;
	}
	std::cerr << "bank: " << bank->regions.size() << " regions, " << seconds_since(started) << " s\n";
	started = Clock::now();
	safsyn::SmfFile midi;
	if (!midi.load(options.midi.c_str()))
	{
		std::cerr << "could not load MIDI file\n";
		return 1;
	}
	std::cerr << "midi: " << midi.input_bytes() << " bytes, " << seconds_since(started) << " s\n";
	try
	{
		return options.live ? play_live(options, bank, midi) : profile_blocks(options, *bank, midi);
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
