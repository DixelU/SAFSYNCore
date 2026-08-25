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

struct SmfAnalysisOptions
{
	uint32_t sample_rate = 48000;
	uint16_t initial_bank = 0;
	uint8_t initial_program = 0;
	uint64_t tail_frames = 0;
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
	uint64_t last_tick = 0;
	uint64_t duration_frames = 0;
	uint64_t estimated_output_frames = 0;
	uint64_t estimated_output_bytes = 0;
	uint64_t maximum_events_same_tick = 0;
	uint64_t maximum_events_same_sample = 0;
	size_t parser_state_bytes = 0;
	bool requires_rf64 = false;
	std::vector<SmfBankProgramUsage> bank_program_usage;
	std::vector<SmfDiagnostic> diagnostics;
};

bool analyze_smf(const SmfFile& file, const SmfAnalysisOptions& options,
	SmfAnalysis& analysis) noexcept;

} // namespace safsyn
