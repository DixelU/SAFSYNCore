#pragma once

#include "core.h"
#include "mastering.h"
#include "smf.h"
#include "wav_writer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace safsyn
{

enum class SmfRenderProgressStage : uint8_t
{
	Preparing,
	RenderingEvents,
	RenderingTail,
	Finalizing,
	Complete,
};

struct SmfRenderProgress
{
	SmfRenderProgressStage stage = SmfRenderProgressStage::Preparing;
	uint64_t frames_rendered = 0;
	uint64_t total_frames = 0;
	uint64_t scheduled_events = 0;
	size_t active_voices = 0;
	size_t active_cohorts = 0;
	float raw_peak = 0.0f;
};

// Return false to stop rendering. A cancelled render is finalized as a valid
// partial WAV and reported through SmfRenderResult::cancelled.
using SmfRenderProgressCallback = bool (*)(const SmfRenderProgress& progress,
	void* user_data) noexcept;

enum class TimedMidiEventKind : uint8_t
{
	Ignored,
	ShortMessage,
	SystemExclusive,
	MasterVolume,
};

struct TimedMidiEvent
{
	uint64_t frame = 0;
	TimedMidiEventKind kind = TimedMidiEventKind::Ignored;
	uint32_t short_message = 0;
	uint16_t master_volume = 0;
};

// Return false at the end of the source. Events must be returned in
// non-decreasing frame order.
using TimedMidiNextCallback = bool (*)(TimedMidiEvent& event,
	void* user_data) noexcept;

// Return false if the output sink cannot accept the block. first_frame is
// relative to the start of this render, before any container-specific lead-in.
using StereoPcmWriteCallback = bool (*)(const float* interleaved_stereo,
	uint32_t frames, uint64_t first_frame, void* user_data) noexcept;

struct SmfRenderOptions
{
	uint32_t sample_rate = 48000;
	size_t voice_capacity = 256;
	VoiceModel voice_model = VoiceModel::Cohorts;
	// Zero gives the offline cohort engine dynamic growth without stealing.
	size_t maximum_cohorts = 0;
	size_t render_threads = 1;
	uint16_t initial_bank = 0;
	uint8_t initial_program = 0;
	uint64_t tail_frames = 0;
	bool drain_tail = false;
	uint64_t maximum_tail_frames = 0;
	// Zero means no explicit cap.
	uint64_t maximum_frames = 0;
	uint32_t block_frames = 256;
	bool all_regions = false;
	PhaseSettings phase;
	MasteringSettings mastering;
	SmfRenderProgressCallback progress_callback = nullptr;
	void* progress_user_data = nullptr;
};

struct SmfRenderResult
{
	uint64_t scheduled_events = 0;
	uint64_t dispatched_channel_events = 0;
	uint64_t dispatched_sysex_events = 0;
	uint64_t master_volume_events = 0;
	uint64_t frames_written = 0;
	uint64_t metric_samples = 0;
	uint64_t raw_metric_samples = 0;
	size_t active_voices_at_end = 0;
	size_t active_cohorts_at_end = 0;
	size_t render_threads = 1;
	uint64_t tail_frames_written = 0;
	float peak = 0.0f;
	double rms = 0.0;
	float raw_peak = 0.0f;
	double raw_rms = 0.0;
	double render_ms = 0.0;
	bool truncated = false;
	bool tail_ceiling_reached = false;
	bool cancelled = false;
	WavContainer container = WavContainer::Riff;
	RenderStats engine;
	PhaseCacheStats phase;
	MasteringStats mastering;
	std::vector<SmfDiagnostic> diagnostics;
};

// Shared synthesizer/rendering core for already scheduled MIDI sources. This
// keeps voice policy, phase preparation, mastering, tails, cancellation, and
// metrics identical across WAV, MP4, and prepared-archive callers.
bool render_timed_midi_pcm(uint64_t duration_frames,
	const Soundfont& soundfont,
	TimedMidiNextCallback next_event,
	void* next_event_user_data,
	const SmfRenderOptions& options,
	SmfRenderResult& result,
	StereoPcmWriteCallback write_pcm,
	void* write_pcm_user_data) noexcept;

// Standard-MIDI adapter over the shared PCM renderer.
bool render_smf_pcm(const SmfFile& file, const SmfAnalysis& analysis,
	const Soundfont& soundfont, const SmfRenderOptions& options,
	SmfRenderResult& result, StereoPcmWriteCallback write_pcm,
	void* write_pcm_user_data) noexcept;

bool render_smf_stream(const SmfFile& file, const SmfAnalysis& analysis,
	const Soundfont& soundfont, const char* output_path, const SmfRenderOptions& options,
	SmfRenderResult& result) noexcept;

} // namespace safsyn
