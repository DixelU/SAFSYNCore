#pragma once

#include "core.h"
#include "smf.h"
#include "wav_writer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace safsyn
{

struct SmfRenderOptions
{
	uint32_t sample_rate = 48000;
	size_t voice_capacity = 256;
	uint16_t initial_bank = 0;
	uint8_t initial_program = 0;
	uint64_t tail_frames = 0;
	// Zero means no explicit cap.
	uint64_t maximum_frames = 0;
	uint32_t block_frames = 256;
	bool all_regions = false;
	PhaseSettings phase;
};

struct SmfRenderResult
{
	uint64_t scheduled_events = 0;
	uint64_t dispatched_channel_events = 0;
	uint64_t frames_written = 0;
	uint64_t metric_samples = 0;
	size_t active_voices_at_end = 0;
	float peak = 0.0f;
	double rms = 0.0;
	double render_ms = 0.0;
	bool truncated = false;
	WavContainer container = WavContainer::Riff;
	RenderStats engine;
	PhaseCacheStats phase;
	std::vector<SmfDiagnostic> diagnostics;
};

bool render_smf_stream(const SmfFile& file, const SmfAnalysis& analysis,
	const Soundfont& soundfont, const char* output_path, const SmfRenderOptions& options,
	SmfRenderResult& result) noexcept;

} // namespace safsyn
