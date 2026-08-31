#include "windows_synth.h"

#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
std::atomic<bool> interrupted{false};
BOOL WINAPI console_control(DWORD event)
{
	if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
	interrupted.store(true); return TRUE;
}
std::string utf8(const std::wstring& value)
{
	const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	std::string result(static_cast<size_t>(count), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
	return result;
}
struct ConsoleHandler
{
	ConsoleHandler() { SetConsoleCtrlHandler(console_control, TRUE); }
	~ConsoleHandler() { SetConsoleCtrlHandler(console_control, FALSE); }
};
}

int wmain(int argc, wchar_t** argv)
{
	try
	{
		safsyn::WindowsSynthOptions options;
		for (int i = 1; i < argc; ++i)
		{
			const std::wstring argument = argv[i];
			auto value = [&]() -> std::wstring {
				if (++i >= argc) throw std::runtime_error("missing option value");
				return argv[i];
			};
			auto number = [&]() -> uint32_t {
				const auto text = value(); size_t end = 0;
				const auto result = std::stoull(text, &end);
				if (end != text.size() || text.front() == L'-' || result > UINT32_MAX)
					throw std::runtime_error("invalid integer option");
				return static_cast<uint32_t>(result);
			};
			auto decimal = [&]() -> double {
				const auto text = value(); size_t end = 0; const auto result = std::stod(text, &end);
				if (end != text.size() || !std::isfinite(result)) throw std::runtime_error("invalid decimal option");
				return result;
			};
			if (argument == L"--help")
			{
				std::cout << "SAFSYN live synthesizer (Windows WASAPI)\n"
					"--list-devices\n--bank piano.sf2|instrument.sfz (omit for sine)\n"
					"--midi song.mid (omit for live input) --midi-in N --output N\n"
					"--threads 0..64 --cohorts N --buffer-frames N --block-frames N\n"
					"--sample-rate N --gain-db N --no-limiter --seconds N --test-note --mute\n"
					"--phase coherent|polarity|analytic|smooth|independent (default coherent)\n"
					"--phase-strength 0..1 --phase-pool 1..64 --phase-continuous --phase-seed N\n"
					"--phase-correlation-hz N --phase-preserve-attack-ms N --phase-cache-mib N\n"
					"All phase samples/variants are prepared before audio starts (Ctrl+C cancels).\n"
					"Cache limit defaults to 2048 MiB; temporary FFT memory is extra.\n"
					"Live mode runs until Ctrl+C; file mode stops after the release tail.\n";
				return 0;
			}
			else if (argument == L"--list-devices")
			{
				const auto outputs = safsyn::audio_output_devices();
				for (size_t j = 0; j < outputs.size(); ++j) std::cout << "Output " << j << ": " << utf8(outputs[j].name) << '\n';
				for (const auto& input : safsyn::midi_input_devices()) std::cout << "MIDI input " << input.id << ": " << utf8(input.name) << '\n';
				return 0;
			}
			else if (argument == L"--bank") options.bank_path = value();
			else if (argument == L"--midi") options.midi_path = value();
			else if (argument == L"--midi-in")
			{
				const auto input = number();
				if (input > INT_MAX) throw std::runtime_error("MIDI input index out of range");
				options.midi_input = static_cast<int>(input);
			}
			else if (argument == L"--output")
			{
				const auto index = number(); const auto outputs = safsyn::audio_output_devices();
				if (index >= outputs.size()) throw std::runtime_error("output index out of range");
				options.output_device = outputs[index].id;
			}
			else if (argument == L"--threads") options.playback.render_threads = number();
			else if (argument == L"--cohorts") options.playback.maximum_cohorts = number();
			else if (argument == L"--buffer-frames") options.playback.buffer_frames = number();
			else if (argument == L"--block-frames") options.playback.block_frames = number();
			else if (argument == L"--sample-rate") options.playback.sample_rate = number();
			else if (argument == L"--phase")
			{
				const auto mode = value();
				if (mode == L"coherent") options.playback.phase.mode = safsyn::PhaseMode::Coherent;
				else if (mode == L"polarity") options.playback.phase.mode = safsyn::PhaseMode::RandomPolarity;
				else if (mode == L"analytic") options.playback.phase.mode = safsyn::PhaseMode::Analytic;
				else if (mode == L"smooth") options.playback.phase.mode = safsyn::PhaseMode::SmoothField;
				else if (mode == L"independent") options.playback.phase.mode = safsyn::PhaseMode::IndependentBins;
				else throw std::runtime_error("unknown phase mode");
			}
			else if (argument == L"--phase-strength") options.playback.phase.strength = static_cast<float>(decimal());
			else if (argument == L"--phase-pool") options.playback.phase.pool_size = number();
			else if (argument == L"--phase-continuous") options.playback.phase.continuous = true;
			else if (argument == L"--phase-correlation-hz") options.playback.phase.correlation_hz = static_cast<float>(decimal());
			else if (argument == L"--phase-preserve-attack-ms") options.playback.phase.preserve_attack_ms = static_cast<float>(decimal());
			else if (argument == L"--phase-cache-mib") options.playback.maximum_phase_cache_bytes = uint64_t{number()} * 1048576;
			else if (argument == L"--phase-seed")
			{
				const auto text = value(); size_t end = 0;
				if (text.empty() || text.find(L'-') != std::wstring::npos) throw std::runtime_error("invalid phase seed");
				options.playback.phase.seed = std::stoull(text, &end);
				if (end != text.size()) throw std::runtime_error("invalid phase seed");
			}
			else if (argument == L"--gain-db" || argument == L"--seconds")
			{
				const auto text = value(); size_t end = 0; const auto result = std::stod(text, &end);
				if (end != text.size() || !std::isfinite(result)) throw std::runtime_error("invalid decimal option");
				if (argument == L"--gain-db") options.playback.mastering.output_gain_db = result;
				else { if (result <= 0) throw std::runtime_error("seconds must be positive"); options.maximum_seconds = result; }
			}
			else if (argument == L"--no-limiter") options.playback.mastering.limiter_enabled = false;
			else if (argument == L"--mute") options.mute = true;
			else if (argument == L"--test-note") options.test_note = true;
			else throw std::runtime_error("unknown option; see --help");
		}
		safsyn::WindowsSynth synth;
		ConsoleHandler handler;
		synth.start(options);
		auto next_report = std::chrono::steady_clock::now();
		for (;;)
		{
			const auto state = synth.stats();
			if (!state.running) break;
			if (interrupted.load()) synth.request_stop();
			if (std::chrono::steady_clock::now() >= next_report)
			{
				std::cerr << utf8(state.status) << " played_seconds="
					<< double(state.playback.consumed_frames) / options.playback.sample_rate
					<< " events=" << state.playback.scheduled_events
					<< " prepared=" << state.playback.preparation.completed << '/' << state.playback.preparation.total
					<< " cache_mib=" << double(state.playback.phase.cache_bytes) / 1048576
					<< '/' << double(state.playback.preparation.total_cache_bytes) / 1048576
					<< " underruns=" << state.playback.underruns << '\n';
				next_report = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		const auto state = synth.stats();
		const auto& stats = state.playback;
		std::cout << "threads=" << stats.render_threads << " frames=" << stats.consumed_frames
			<< " events=" << stats.scheduled_events << " peak_cohorts=" << stats.engine.peak_active_cohorts
			<< " steals=" << stats.engine.cohort_capacity_steals << " parallel_calls=" << stats.engine.parallel_render_calls
			<< " underruns=" << stats.underruns << " missing_frames=" << stats.underrun_frames
			<< " midi_rejected=" << stats.rejected_midi_events << " device_empty=" << state.empty_device_buffers << '\n';
		std::cout << "prepared=" << stats.preparation.completed << '/' << stats.preparation.total
			<< " preparation_ms=" << stats.preparation_ms << " phase_cache_bytes=" << stats.phase.cache_bytes
			<< " phase_preprocessing_ms=" << stats.phase.preprocessing_ms << " phase_failures=" << stats.phase.failures << '\n';
		if (!state.error.empty()) { std::cerr << state.error << '\n'; return 1; }
		return 0;
	}
	catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
