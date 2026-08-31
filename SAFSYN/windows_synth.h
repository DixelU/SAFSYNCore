#pragma once

#include "playback.h"

#include <memory>
#include <string>
#include <vector>

namespace safsyn
{

struct AudioOutputDevice { std::wstring id, name; };
struct MidiInputDevice { uint32_t id; std::wstring name; };
std::vector<AudioOutputDevice> audio_output_devices();
std::vector<MidiInputDevice> midi_input_devices();

struct WindowsSynthOptions
{
	std::wstring bank_path; // Empty selects the built-in sine instrument.
	std::wstring midi_path; // Empty selects live MIDI mode.
	std::wstring output_device; // Empty selects the default multimedia endpoint.
	int midi_input = -1;
	PlaybackOptions playback; // Standalone host overrides phase to Coherent.
	double maximum_seconds = 0.0; // Optional bounded device smoke test.
	bool mute = false; // Device validation without audible output.
	bool test_note = false;
};

struct WindowsSynthStats
{
	PlaybackStats playback;
	bool running = false;
	std::wstring status = L"Stopped";
	std::string error;
	uint32_t device_buffer_frames = 0;
	uint64_t empty_device_buffers = 0;
};

// All WASAPI COM interfaces are created, used, and released on one dedicated
// delivery thread. WinMM has its own service thread; neither callback calls DSP.
class WindowsSynth
{
public:
	WindowsSynth();
	~WindowsSynth();
	WindowsSynth(const WindowsSynth&) = delete;
	WindowsSynth& operator=(const WindowsSynth&) = delete;
	void start(const WindowsSynthOptions& options);
	void request_stop() noexcept;
	void stop() noexcept;
	bool send_short_message(uint32_t message) noexcept;
	void panic() noexcept;
	WindowsSynthStats stats() const;
private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace safsyn
