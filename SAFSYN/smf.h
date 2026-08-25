#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace safsyn
{

enum class SmfDiagnosticSeverity : uint8_t { Warning, Error };

struct SmfDiagnostic
{
	SmfDiagnosticSeverity severity = SmfDiagnosticSeverity::Error;
	uint32_t track = UINT32_MAX;
	uint64_t byte_offset = 0;
	std::string message;
};

struct SmfHeader
{
	uint16_t format = 0;
	uint16_t track_count = 0;
	uint16_t division = 0;
	bool smpte = false;
	uint16_t ppqn = 0;
	int8_t smpte_code = 0;
	uint8_t ticks_per_frame = 0;
};

enum class SmfEventKind : uint8_t
{
	Channel,
	Tempo,
	Meta,
	SystemExclusive,
	EndOfTrack,
};

struct SmfEvent
{
	uint64_t tick = 0;
	uint64_t ordinal = 0;
	uint32_t track = 0;
	uint64_t byte_offset = 0;
	SmfEventKind kind = SmfEventKind::Meta;
	uint8_t status = 0;
	uint8_t data1 = 0;
	uint8_t data2 = 0;
	uint8_t data_size = 0;
	uint8_t meta_type = 0;
	uint32_t tempo_us_per_quarter = 0;
	uint64_t payload_offset = 0;
	uint32_t payload_size = 0;
};

class SmfFile
{
public:
	bool load(const char* path) noexcept;
	bool load_bytes(std::vector<uint8_t> bytes) noexcept;

	const SmfHeader& header() const noexcept { return header_; }
	const std::vector<SmfDiagnostic>& diagnostics() const noexcept { return diagnostics_; }
	bool valid() const noexcept { return valid_; }
	uint64_t input_bytes() const noexcept { return bytes_.size(); }
	// Meta and SysEx payloads remain owned by the file for the lifetime of the
	// SmfFile. A null result means the event does not describe a valid payload.
	const uint8_t* payload_data(const SmfEvent& event) const noexcept;

private:
	struct TrackSpan { size_t begin = 0; size_t end = 0; };
	bool parse_chunks();

	SmfHeader header_;
	std::vector<uint8_t> bytes_;
	std::vector<TrackSpan> tracks_;
	std::vector<SmfDiagnostic> diagnostics_;
	bool valid_ = false;

	friend class MergedSmfStream;
};

class MergedSmfStream
{
public:
	explicit MergedSmfStream(const SmfFile& file);
	~MergedSmfStream();
	MergedSmfStream(const MergedSmfStream&) = delete;
	MergedSmfStream& operator=(const MergedSmfStream&) = delete;
	MergedSmfStream(MergedSmfStream&&) noexcept;
	MergedSmfStream& operator=(MergedSmfStream&&) noexcept;

	bool next(SmfEvent& event) noexcept;
	bool good() const noexcept;
	const std::vector<SmfDiagnostic>& diagnostics() const noexcept;
	size_t state_bytes() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

struct ScheduledSmfEvent
{
	SmfEvent event;
	uint64_t sample = 0;
};

class ScheduledSmfStream
{
public:
	ScheduledSmfStream(const SmfFile& file, uint32_t sample_rate);
	~ScheduledSmfStream();
	ScheduledSmfStream(const ScheduledSmfStream&) = delete;
	ScheduledSmfStream& operator=(const ScheduledSmfStream&) = delete;
	ScheduledSmfStream(ScheduledSmfStream&&) noexcept;
	ScheduledSmfStream& operator=(ScheduledSmfStream&&) noexcept;

	bool next(ScheduledSmfEvent& event) noexcept;
	bool good() const noexcept;
	const std::vector<SmfDiagnostic>& diagnostics() const noexcept;
	size_t state_bytes() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

struct SmfBankProgramUsage
{
	uint8_t channel = 0;
	uint16_t bank = 0;
	uint8_t program = 0;
	uint64_t note_ons = 0;
};

struct SmfGroupHistogramEntry
{
	uint64_t group_size = 0;
	uint64_t groups = 0;
	uint64_t events = 0;
};

struct SmfControllerUsage
{
	uint8_t channel = 0;
	uint8_t controller = 0;
	uint8_t minimum_value = 127;
	uint8_t maximum_value = 0;
	uint64_t events = 0;
	uint64_t first_tick = 0;
	uint64_t last_tick = 0;
	uint64_t first_sample = 0;
	uint64_t last_sample = 0;
};

struct SmfControllerTraceEntry
{
	uint64_t tick = 0;
	uint64_t sample = 0;
	uint32_t track = 0;
	uint64_t ordinal = 0;
	uint8_t channel = 0;
	uint8_t controller = 0;
	uint8_t value = 0;
	uint16_t volume = 0;
	uint16_t pan = 0;
	uint16_t expression = 0;
	uint64_t active_channel_notes_before = 0;
	uint64_t active_channel_notes = 0;
	bool sustain = false;
};

struct SmfAnalysisOptions
{
	uint32_t sample_rate = 48000;
	uint16_t initial_bank = 0;
	uint8_t initial_program = 0;
	uint64_t tail_frames = 0;
	// Zero disables the bounded controller-state trace. The controller
	// histogram is always collected.
	uint64_t controller_trace_frames = 0;
	uint64_t controller_trace_start_frame = 0;
	size_t controller_trace_limit = 0;
	// -1 traces every controller, otherwise only the selected CC.
	int16_t controller_trace_controller = -1;
};

struct SmfAnalysis
{
	SmfHeader header;
	uint32_t sample_rate = 0;
	uint64_t input_bytes = 0;
	uint64_t total_events = 0;
	uint64_t channel_events = 0;
	uint64_t note_ons = 0;
	uint64_t note_offs = 0;
	uint64_t tempo_changes = 0;
	uint64_t meta_events = 0;
	uint64_t sysex_events = 0;
	uint64_t universal_master_volume_events = 0;
	uint64_t last_tick = 0;
	uint64_t duration_frames = 0;
	uint64_t estimated_output_frames = 0;
	uint64_t estimated_output_bytes = 0;
	uint64_t maximum_events_same_tick = 0;
	uint64_t maximum_events_same_sample = 0;
	uint64_t maximum_events_tick_location = 0;
	uint64_t maximum_events_sample_location = 0;
	uint64_t note_on_groups = 0;
	uint64_t note_off_groups = 0;
	uint64_t largest_identical_note_on_group = 0;
	uint64_t largest_identical_note_off_group = 0;
	uint64_t largest_note_on_group_sample = 0;
	uint64_t largest_note_off_group_sample = 0;
	uint64_t estimated_peak_active_logical_notes = 0;
	uint64_t estimated_peak_same_onset_cohorts = 0;
	double estimated_same_onset_compression_ratio = 0.0;
	size_t parser_state_bytes = 0;
	bool requires_rf64 = false;
	std::vector<SmfBankProgramUsage> bank_program_usage;
	std::vector<SmfControllerUsage> controller_usage;
	std::vector<SmfControllerTraceEntry> controller_trace;
	std::vector<SmfGroupHistogramEntry> note_on_group_histogram;
	std::vector<SmfGroupHistogramEntry> note_off_group_histogram;
	std::vector<SmfDiagnostic> diagnostics;
};

bool analyze_smf(const SmfFile& file, const SmfAnalysisOptions& options,
	SmfAnalysis& analysis) noexcept;

// Recognizes the Universal Real-Time Device Control / Master Volume message.
// The returned value is the MIDI little-endian 14-bit volume (0..16383).
bool decode_universal_master_volume(const SmfFile& file, const SmfEvent& event,
	uint16_t& value14) noexcept;

} // namespace safsyn
