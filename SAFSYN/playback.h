#pragma once

#include "core.h"
#include "mastering.h"
#include "smf.h"

#include <atomic>
#include <memory>
#include <string>

namespace safsyn
{

enum class MidiEnqueueResult { Queued, Full, Unavailable };

struct PlaybackOptions
{
	uint32_t sample_rate = 48000;
	uint32_t block_frames = 256;
	uint32_t buffer_frames = 4096;
	size_t maximum_cohorts = 4096; // Required finite live safety ceiling.
	size_t render_threads = 0; // Auto: up to 16, leaving two logical CPUs free.
	size_t midi_queue_capacity = 262144;
	uint16_t initial_bank = 0;
	uint8_t initial_program = 0;
	uint32_t maximum_tail_seconds = 10;
	PhaseSettings phase;
	uint64_t maximum_phase_cache_bytes = uint64_t{2048} * 1024 * 1024;
	MasteringSettings mastering{-12.0, true, -1.0, 5.0, 100.0};
};

struct PlaybackStats
{
	RenderStats engine;
	PhaseCacheStats phase;
	PhasePreparationProgress preparation;
	bool preparing = false;
	double preparation_ms = 0.0;
	uint64_t scheduled_events = 0;
	uint64_t consumed_frames = 0;
	uint64_t underrun_frames = 0;
	uint64_t underruns = 0;
	uint64_t rejected_midi_events = 0;
	uint64_t midi_recoveries = 0;
	uint64_t safety_clamped_samples = 0;
	size_t active_voices = 0;
	size_t active_cohorts = 0;
	size_t buffered_frames = 0;
	size_t render_threads = 1;
	double render_load = 0.0; // Work time / produced audio duration, latest block.
	float raw_peak = 0.0f;
	bool ready = false;
	bool finished = false;
	std::string error;
};

// One render producer owns the engine and its persistent cohort workers. One
// audio consumer calls read_audio(); it never renders, allocates, or locks.
// MIDI submission is bounded and safe from multiple producer threads. Settings,
// sound bank, and optional SMF are immutable for the duration of a session.
class BufferedSynth
{
public:
	BufferedSynth(std::shared_ptr<const Soundfont> bank, const PlaybackOptions& options,
		std::shared_ptr<const SmfFile> midi = {});
	~BufferedSynth();
	BufferedSynth(const BufferedSynth&) = delete;
	BufferedSynth& operator=(const BufferedSynth&) = delete;

	void start(); // Once only. Control thread; construction does not start work.
	void request_stop() noexcept;
	void stop() noexcept; // Joins the producer; never call from an audio callback.
	bool enqueue_short_message(uint32_t message) noexcept;
	// For file senders that can retry: Full leaves voices, controllers, and
	// queued events intact. The caller retains the message until Queued.
	MidiEnqueueResult try_enqueue_short_message(uint32_t message) noexcept;
	bool enqueue_master_volume(uint16_t value) noexcept;
	// Invalidates queued live events and resets controllers/voices on producer.
	void panic() noexcept;
	void report_input_loss() noexcept;
	// Always fills all frames (silence on starvation). Returns copied music frames.
	uint32_t read_audio(float* stereo, uint32_t frames) noexcept;
	bool ready() const noexcept;
	bool finished() const noexcept;
	bool drained() const noexcept;
	PlaybackStats stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace safsyn
