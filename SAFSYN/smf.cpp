#include "smf.h"

#include "long_uint.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <queue>
#include <tuple>
#include <utility>

namespace safsyn
{
namespace
{
uint16_t read_be16(const std::vector<uint8_t>& bytes, size_t offset) noexcept
{
	return static_cast<uint16_t>((static_cast<uint16_t>(bytes[offset]) << 8) |
		bytes[offset + 1]);
}

uint32_t read_be32(const std::vector<uint8_t>& bytes, size_t offset) noexcept
{
	return (static_cast<uint32_t>(bytes[offset]) << 24) |
		(static_cast<uint32_t>(bytes[offset + 1]) << 16) |
		(static_cast<uint32_t>(bytes[offset + 2]) << 8) |
		bytes[offset + 3];
}

bool chunk_is(const std::vector<uint8_t>& bytes, size_t offset, const char* name) noexcept
{
	return offset + 4 <= bytes.size() && std::memcmp(bytes.data() + offset, name, 4) == 0;
}

void add_diagnostic(std::vector<SmfDiagnostic>& diagnostics, SmfDiagnosticSeverity severity,
	uint32_t track, uint64_t offset, std::string message)
{
	diagnostics.push_back({severity, track, offset, std::move(message)});
}

enum class ParseResult { Event, End, Error };

struct TrackCursor
{
	const std::vector<uint8_t>* bytes = nullptr;
	size_t position = 0;
	size_t end = 0;
	uint32_t track = 0;
	uint8_t running_status = 0;
	uint64_t absolute_tick = 0;
	uint64_t ordinal = 0;
	bool ended = false;
	bool saw_end_of_track = false;
	bool warned_missing_end = false;
	std::vector<SmfDiagnostic>* diagnostics = nullptr;

	bool read_vlq(uint32_t& value, const char* purpose)
	{
		value = 0;
		for (uint32_t count = 0; count < 4; ++count)
		{
			if (position >= end)
			{
				add_diagnostic(*diagnostics, SmfDiagnosticSeverity::Error, track, position,
					std::string("truncated ") + purpose + " VLQ");
				return false;
			}
			const uint8_t byte = (*bytes)[position++];
			value = (value << 7) | (byte & 0x7fU);
			if ((byte & 0x80U) == 0)
				return true;
		}
		add_diagnostic(*diagnostics, SmfDiagnosticSeverity::Error, track, position - 1,
			std::string(purpose) + " VLQ exceeds four bytes");
		return false;
	}

	bool read_data_byte(uint8_t& value, const char* purpose)
	{
		if (position >= end)
		{
			add_diagnostic(*diagnostics, SmfDiagnosticSeverity::Error, track, position,
				std::string("truncated ") + purpose);
			return false;
		}
		value = (*bytes)[position++];
		if (value >= 0x80)
		{
			add_diagnostic(*diagnostics, SmfDiagnosticSeverity::Error, track, position - 1,
				std::string("status byte encountered in ") + purpose);
			return false;
		}
		return true;
	}

	ParseResult next(SmfEvent& event)
	{
		if (ended)
			return ParseResult::End;
		if (position == end)
		{
			ended = true;
			if (!saw_end_of_track && !warned_missing_end)
			{
				warned_missing_end = true;
				add_diagnostic(*diagnostics, SmfDiagnosticSeverity::Warning, track, position,
					"track chunk ended without an end-of-track meta event");
			}
			return ParseResult::End;
		}

		const size_t event_offset = position;
		uint32_t delta = 0;
		if (!read_vlq(delta, "delta-time"))
			return ParseResult::Error;
		if (absolute_tick > (std::numeric_limits<uint64_t>::max)() - delta)
		{
			add_diagnostic(*diagnostics, SmfDiagnosticSeverity::Error, track, event_offset,
				"absolute MIDI tick overflow");
			return ParseResult::Error;
		}
		absolute_tick += delta;
		if (position >= end)
		{
			add_diagnostic(*diagnostics, SmfDiagnosticSeverity::Error, track, position,
				"truncated event after delta-time");
			return ParseResult::Error;
		}

		const uint8_t first = (*bytes)[position++];
		uint8_t status = first;
		bool has_first_data = false;
		uint8_t first_data = 0;
		if (first < 0x80)
		{
			if (running_status < 0x80 || running_status >= 0xf0)
			{
				add_diagnostic(*diagnostics, SmfDiagnosticSeverity::Error, track, position - 1,
					"running-status data has no preceding channel status");
				return ParseResult::Error;
			}
			status = running_status;
			has_first_data = true;
			first_data = first;
		}
		else if (status < 0xf0)
			running_status = status;
		else
			running_status = 0;

		event = {};
		event.tick = absolute_tick;
		event.ordinal = ordinal++;
		event.track = track;
		event.byte_offset = event_offset;
		event.status = status;

		if (status >= 0x80 && status <= 0xef)
		{
			event.kind = SmfEventKind::Channel;
			event.data_size = (status & 0xf0U) == 0xc0 || (status & 0xf0U) == 0xd0 ? 1 : 2;
			if (has_first_data)
				event.data1 = first_data;
			else if (!read_data_byte(event.data1, "first channel-event data byte"))
				return ParseResult::Error;
			if (event.data_size == 2 &&
				!read_data_byte(event.data2, "second channel-event data byte"))
				return ParseResult::Error;
			return ParseResult::Event;
		}

		if (status == 0xff)
		{
			if (position >= end)
			{
				add_diagnostic(*diagnostics, SmfDiagnosticSeverity::Error, track, position,
					"truncated meta-event type");
				return ParseResult::Error;
			}
			event.meta_type = (*bytes)[position++];
			uint32_t length = 0;
			if (!read_vlq(length, "meta-event length"))
				return ParseResult::Error;
			if (length > end - position)
			{
				add_diagnostic(*diagnostics, SmfDiagnosticSeverity::Error, track, position,
					"truncated meta-event payload");
				return ParseResult::Error;
			}
			event.payload_size = length;
			const size_t payload = position;
			event.payload_offset = payload;
			position += length;
			if (event.meta_type == 0x2f)
			{
				event.kind = SmfEventKind::EndOfTrack;
				saw_end_of_track = true;
				if (length != 0)
					add_diagnostic(*diagnostics, SmfDiagnosticSeverity::Warning, track, payload,
						"end-of-track meta event has a nonzero payload");
				if (position != end)
					add_diagnostic(*diagnostics, SmfDiagnosticSeverity::Warning, track, position,
						"bytes after end-of-track were ignored");
				position = end;
				ended = true;
				return ParseResult::Event;
			}
			if (event.meta_type == 0x51)
			{
				if (length == 3)
				{
					event.tempo_us_per_quarter =
						(static_cast<uint32_t>((*bytes)[payload]) << 16) |
						(static_cast<uint32_t>((*bytes)[payload + 1]) << 8) |
						(*bytes)[payload + 2];
					if (event.tempo_us_per_quarter != 0)
					{
						event.kind = SmfEventKind::Tempo;
						return ParseResult::Event;
					}
				}
				add_diagnostic(*diagnostics, SmfDiagnosticSeverity::Warning, track, payload,
					"invalid tempo meta event was ignored");
			}
			event.kind = SmfEventKind::Meta;
			return ParseResult::Event;
		}

		if (status == 0xf0 || status == 0xf7)
		{
			uint32_t length = 0;
			if (!read_vlq(length, "system-exclusive length"))
				return ParseResult::Error;
			if (length > end - position)
			{
				add_diagnostic(*diagnostics, SmfDiagnosticSeverity::Error, track, position,
					"truncated system-exclusive payload");
				return ParseResult::Error;
			}
			event.kind = SmfEventKind::SystemExclusive;
			event.payload_size = length;
			event.payload_offset = position;
			position += length;
			return ParseResult::Event;
		}

		add_diagnostic(*diagnostics, SmfDiagnosticSeverity::Error, track, position - 1,
			"unsupported system status in SMF track");
		return ParseResult::Error;
	}
};

struct HeapNode
{
	SmfEvent event;
};

struct LaterEvent
{
	bool operator()(const HeapNode& left, const HeapNode& right) const noexcept
	{
		return std::tie(left.event.tick, left.event.track, left.event.ordinal) >
			std::tie(right.event.tick, right.event.track, right.event.ordinal);
	}
};

class ExactSampleClock
{
public:
	bool configure(const SmfHeader& header, uint32_t sample_rate) noexcept
	{
		if (sample_rate == 0)
			return false;
		smpte_ = header.smpte;
		if (!smpte_)
		{
			if (header.ppqn == 0)
				return false;
			denominator_ = static_cast<uint64_t>(header.ppqn) * 1'000'000ULL;
			factor_ = static_cast<uint64_t>(sample_rate) * 500'000ULL;
			return true;
		}
		if (header.ticks_per_frame == 0)
			return false;
		switch (header.smpte_code)
		{
		case -24: case -25: case -30:
			factor_ = sample_rate;
			denominator_ = static_cast<uint64_t>(-header.smpte_code) *
				header.ticks_per_frame;
			return true;
		case -29:
			factor_ = static_cast<uint64_t>(sample_rate) * 1001ULL;
			denominator_ = 30'000ULL * header.ticks_per_frame;
			return true;
		default:
			return false;
		}
	}

	bool advance(uint64_t ticks) noexcept
	{
		try
		{
			using Wide = dixelu::long_uint<0>;
			const Wide numerator = Wide(ticks) * Wide(factor_) + Wide(remainder_);
			uint64_t quotient = 0;
			uint64_t remainder = 0;
			if (!divide_wide_by_small(numerator, denominator_, quotient, remainder) ||
				frame_ > (std::numeric_limits<uint64_t>::max)() - quotient)
				return false;
			frame_ += quotient;
			remainder_ = remainder;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	void set_tempo(uint32_t microseconds_per_quarter, uint32_t sample_rate) noexcept
	{
		if (!smpte_ && microseconds_per_quarter != 0)
			factor_ = static_cast<uint64_t>(sample_rate) * microseconds_per_quarter;
	}

	uint64_t frame() const noexcept { return frame_; }

private:
	static bool divide_wide_by_small(const dixelu::long_uint<0>& dividend,
		uint64_t divisor, uint64_t& quotient, uint64_t& remainder) noexcept
	{
		if (divisor == 0 || dividend[1] >= divisor)
			return false;
#if defined(__SIZEOF_INT128__) && !defined(__wasm__)
		const __uint128_t combined =
			(static_cast<__uint128_t>(dividend[1]) << 64) | dividend[0];
		quotient = static_cast<uint64_t>(combined / divisor);
		remainder = static_cast<uint64_t>(combined % divisor);
#elif defined(_MSC_VER) && defined(_M_X64)
		quotient = _udiv128(dividend[1], dividend[0], divisor, &remainder);
#else
		quotient = 0;
		remainder = dividend[1];
		for (size_t bit = 64; bit-- > 0;)
		{
			const bool overflow = (remainder >> 63) != 0;
			remainder = (remainder << 1) | ((dividend[0] >> bit) & 1U);
			if (overflow || remainder >= divisor)
			{
				remainder -= divisor;
				quotient |= uint64_t{1} << bit;
			}
		}
#endif
		return true;
	}

	uint64_t factor_ = 0;
	uint64_t denominator_ = 1;
	uint64_t remainder_ = 0;
	uint64_t frame_ = 0;
	bool smpte_ = false;
};
} // namespace

bool SmfFile::load(const char* path) noexcept
{
	header_ = {};
	bytes_.clear();
	tracks_.clear();
	diagnostics_.clear();
	valid_ = false;
	if (!path)
	{
		add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Error, UINT32_MAX, 0,
			"null SMF path");
		return false;
	}
	try
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream)
		{
			add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Error, UINT32_MAX, 0,
				"could not open SMF file");
			return false;
		}
		const std::streamoff length = stream.tellg();
		if (length < 0 || static_cast<uint64_t>(length) >
			(static_cast<uint64_t>((std::numeric_limits<size_t>::max)())))
		{
			add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Error, UINT32_MAX, 0,
				"SMF file is too large for this process");
			return false;
		}
		bytes_.resize(static_cast<size_t>(length));
		stream.seekg(0);
		if (!bytes_.empty())
			stream.read(reinterpret_cast<char*>(bytes_.data()),
				static_cast<std::streamsize>(bytes_.size()));
		if (!stream && !bytes_.empty())
		{
			add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Error, UINT32_MAX, 0,
				"failed while reading SMF file");
			return false;
		}
		return parse_chunks();
	}
	catch (...)
	{
		add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Error, UINT32_MAX, 0,
			"exception while loading SMF file");
		return false;
	}
}

bool SmfFile::load_bytes(std::vector<uint8_t> bytes) noexcept
{
	header_ = {};
	bytes_ = std::move(bytes);
	tracks_.clear();
	diagnostics_.clear();
	valid_ = false;
	try
	{
		return parse_chunks();
	}
	catch (...)
	{
		add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Error, UINT32_MAX, 0,
			"exception while parsing SMF chunks");
		return false;
	}
}

const uint8_t* SmfFile::payload_data(const SmfEvent& event) const noexcept
{
	if (event.payload_size == 0 || event.payload_offset > bytes_.size() ||
		event.payload_size > bytes_.size() - static_cast<size_t>(event.payload_offset))
		return nullptr;
	return bytes_.data() + static_cast<size_t>(event.payload_offset);
}

bool SmfFile::parse_chunks()
{
	if (bytes_.size() < 14 || !chunk_is(bytes_, 0, "MThd"))
	{
		add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Error, UINT32_MAX, 0,
			"missing or truncated MThd chunk");
		return false;
	}
	const uint32_t header_length = read_be32(bytes_, 4);
	if (header_length < 6 || header_length > bytes_.size() - 8)
	{
		add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Error, UINT32_MAX, 4,
			"invalid MThd chunk length");
		return false;
	}
	header_.format = read_be16(bytes_, 8);
	header_.track_count = read_be16(bytes_, 10);
	header_.division = read_be16(bytes_, 12);
	if (header_.format == 2)
	{
		add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Error, UINT32_MAX, 8,
			"SMF type 2 is not supported");
		return false;
	}
	if (header_.format > 2)
	{
		add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Error, UINT32_MAX, 8,
			"unknown SMF format");
		return false;
	}
	if (header_.track_count == 0 || (header_.format == 0 && header_.track_count != 1))
	{
		add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Error, UINT32_MAX, 10,
			"SMF track count is inconsistent with its format");
		return false;
	}
	if ((header_.division & 0x8000U) == 0)
	{
		header_.ppqn = header_.division;
		if (header_.ppqn == 0)
		{
			add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Error, UINT32_MAX, 12,
				"PPQN division cannot be zero");
			return false;
		}
	}
	else
	{
		header_.smpte = true;
		header_.smpte_code = static_cast<int8_t>(header_.division >> 8);
		header_.ticks_per_frame = static_cast<uint8_t>(header_.division & 0xffU);
		if ((header_.smpte_code != -24 && header_.smpte_code != -25 &&
			header_.smpte_code != -29 && header_.smpte_code != -30) ||
			header_.ticks_per_frame == 0)
		{
			add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Error, UINT32_MAX, 12,
				"invalid SMPTE division");
			return false;
		}
	}

	size_t position = 8 + header_length;
	while (tracks_.size() < header_.track_count)
	{
		if (position > bytes_.size() || bytes_.size() - position < 8)
		{
			add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Error, UINT32_MAX, position,
				"truncated chunk header before all declared tracks");
			return false;
		}
		const uint32_t chunk_length = read_be32(bytes_, position + 4);
		const size_t payload = position + 8;
		if (chunk_length > bytes_.size() - payload)
		{
			add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Error, UINT32_MAX, position + 4,
				"chunk length exceeds the SMF file");
			return false;
		}
		if (chunk_is(bytes_, position, "MTrk"))
			tracks_.push_back({payload, payload + chunk_length});
		else
			add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Warning, UINT32_MAX, position,
				"unknown non-track chunk was skipped");
		position = payload + chunk_length;
	}
	if (position < bytes_.size())
		add_diagnostic(diagnostics_, SmfDiagnosticSeverity::Warning, UINT32_MAX, position,
			"trailing bytes after declared track chunks were ignored");
	valid_ = true;
	return true;
}

struct MergedSmfStream::Impl
{
	std::vector<TrackCursor> cursors;
	std::priority_queue<HeapNode, std::vector<HeapNode>, LaterEvent> heap;
	std::vector<SmfDiagnostic> diagnostics;
	bool valid = true;

	Impl(const std::vector<uint8_t>& bytes, const std::vector<SmfFile::TrackSpan>& tracks,
		const std::vector<SmfDiagnostic>& file_diagnostics, bool file_valid)
		: diagnostics(file_diagnostics), valid(file_valid)
	{
		if (!valid)
			return;
		cursors.reserve(tracks.size());
		for (uint32_t index = 0; index < tracks.size(); ++index)
		{
			const auto& span = tracks[index];
			cursors.push_back({&bytes, span.begin, span.end, index, 0, 0, 0,
				false, false, false, &diagnostics});
		}
		for (auto& cursor : cursors)
		{
			SmfEvent event;
			const ParseResult result = cursor.next(event);
			if (result == ParseResult::Event)
				heap.push({event});
			else if (result == ParseResult::Error)
				valid = false;
		}
	}
};

MergedSmfStream::MergedSmfStream(const SmfFile& file)
	: impl_(std::make_unique<Impl>(file.bytes_, file.tracks_, file.diagnostics_, file.valid_)) {}
MergedSmfStream::~MergedSmfStream() = default;
MergedSmfStream::MergedSmfStream(MergedSmfStream&&) noexcept = default;
MergedSmfStream& MergedSmfStream::operator=(MergedSmfStream&&) noexcept = default;

bool MergedSmfStream::next(SmfEvent& event) noexcept
{
	if (!impl_ || !impl_->valid || impl_->heap.empty())
		return false;
	const HeapNode node = impl_->heap.top();
	impl_->heap.pop();
	event = node.event;
	TrackCursor& cursor = impl_->cursors[event.track];
	SmfEvent following;
	const ParseResult result = cursor.next(following);
	if (result == ParseResult::Event)
		impl_->heap.push({following});
	else if (result == ParseResult::Error)
		impl_->valid = false;
	return true;
}

bool MergedSmfStream::good() const noexcept
{
	return impl_ && impl_->valid;
}

const std::vector<SmfDiagnostic>& MergedSmfStream::diagnostics() const noexcept
{
	static const std::vector<SmfDiagnostic> empty;
	return impl_ ? impl_->diagnostics : empty;
}

size_t MergedSmfStream::state_bytes() const noexcept
{
	return impl_ ? impl_->cursors.capacity() * (sizeof(TrackCursor) + sizeof(HeapNode)) : 0;
}

struct ScheduledSmfStream::Impl
{
	MergedSmfStream merged;
	ExactSampleClock clock;
	uint32_t sample_rate = 0;
	uint64_t previous_tick = 0;
	std::vector<SmfDiagnostic> diagnostics;
	size_t merged_diagnostics_copied = 0;
	bool valid = true;

	Impl(const SmfFile& file, uint32_t rate) : merged(file), sample_rate(rate)
	{
		valid = merged.good() && clock.configure(file.header(), sample_rate);
		sync_diagnostics();
		if (!valid && merged.good())
			add_diagnostic(diagnostics, SmfDiagnosticSeverity::Error, UINT32_MAX, 12,
				"could not configure the exact SMF sample clock");
	}

	void sync_diagnostics()
	{
		const auto& source = merged.diagnostics();
		while (merged_diagnostics_copied < source.size())
			diagnostics.push_back(source[merged_diagnostics_copied++]);
	}
};

ScheduledSmfStream::ScheduledSmfStream(const SmfFile& file, uint32_t sample_rate)
	: impl_(std::make_unique<Impl>(file, sample_rate)) {}
ScheduledSmfStream::~ScheduledSmfStream() = default;
ScheduledSmfStream::ScheduledSmfStream(ScheduledSmfStream&&) noexcept = default;
ScheduledSmfStream& ScheduledSmfStream::operator=(ScheduledSmfStream&&) noexcept = default;

bool ScheduledSmfStream::next(ScheduledSmfEvent& scheduled) noexcept
{
	if (!impl_ || !impl_->valid)
		return false;
	SmfEvent event;
	if (!impl_->merged.next(event))
	{
		impl_->sync_diagnostics();
		impl_->valid = impl_->merged.good();
		return false;
	}
	impl_->sync_diagnostics();
	if (event.tick < impl_->previous_tick ||
		!impl_->clock.advance(event.tick - impl_->previous_tick))
	{
		add_diagnostic(impl_->diagnostics, SmfDiagnosticSeverity::Error, event.track,
			event.byte_offset, "SMF sample timestamp overflow");
		impl_->valid = false;
		return false;
	}
	impl_->previous_tick = event.tick;
	scheduled.event = event;
	scheduled.sample = impl_->clock.frame();
	if (event.kind == SmfEventKind::Tempo)
		impl_->clock.set_tempo(event.tempo_us_per_quarter, impl_->sample_rate);
	return true;
}

bool ScheduledSmfStream::good() const noexcept
{
	return impl_ && impl_->valid && impl_->merged.good();
}

const std::vector<SmfDiagnostic>& ScheduledSmfStream::diagnostics() const noexcept
{
	static const std::vector<SmfDiagnostic> empty;
	return impl_ ? impl_->diagnostics : empty;
}

size_t ScheduledSmfStream::state_bytes() const noexcept
{
	return impl_ ? sizeof(Impl) + impl_->merged.state_bytes() : 0;
}

bool decode_universal_master_volume(const SmfFile& file, const SmfEvent& event,
	uint16_t& value14) noexcept
{
	value14 = 0;
	if (event.kind != SmfEventKind::SystemExclusive)
		return false;
	const uint8_t* payload = file.payload_data(event);
	if (!payload)
		return false;
	size_t offset = 0;
	if (event.payload_size != 0 && payload[0] == 0xf0)
		offset = 1;
	if (event.payload_size - offset < 6 || payload[offset] != 0x7f ||
		payload[offset + 1] > 0x7f || payload[offset + 2] != 0x04 ||
		payload[offset + 3] != 0x01 || payload[offset + 4] > 0x7f ||
		payload[offset + 5] > 0x7f)
		return false;
	value14 = static_cast<uint16_t>(payload[offset + 4] |
		(static_cast<uint16_t>(payload[offset + 5]) << 7));
	return true;
}

bool analyze_smf(const SmfFile& file, const SmfAnalysisOptions& options,
	SmfAnalysis& analysis) noexcept
{
	analysis = {};
	analysis.header = file.header();
	analysis.sample_rate = options.sample_rate;
	analysis.input_bytes = file.input_bytes();
	if (!file.valid() || options.sample_rate == 0)
	{
		analysis.diagnostics = file.diagnostics();
		if (options.sample_rate == 0)
			add_diagnostic(analysis.diagnostics, SmfDiagnosticSeverity::Error, UINT32_MAX, 0,
				"analysis sample rate cannot be zero");
		return false;
	}
	try
	{
		struct ActiveOnsetGroup
		{
			uint64_t held = 0;
			uint64_t sustained = 0;
			bool counted = true;
		};
		struct NoteRun
		{
			bool valid = false;
			bool note_on = false;
			uint8_t channel = 0;
			uint8_t note = 0;
			uint8_t velocity = 0;
			uint64_t sample = 0;
			uint64_t count = 0;
		};
		ScheduledSmfStream stream(file, options.sample_rate);
		std::array<uint8_t, 16> bank_msb{};
		std::array<uint8_t, 16> bank_lsb{};
		std::array<uint8_t, 16> programs{};
		std::array<uint8_t, 16> volume_msb{};
		std::array<uint8_t, 16> volume_lsb{};
		std::array<uint8_t, 16> pan_msb{};
		std::array<uint8_t, 16> pan_lsb{};
		std::array<uint8_t, 16> expression_msb{};
		std::array<uint8_t, 16> expression_lsb{};
		std::array<uint64_t, 16> active_channel_notes{};
		std::array<std::array<SmfControllerUsage, 128>, 16> controller_usage{};
		std::array<std::array<bool, 128>, 16> controller_seen{};
		for (size_t channel = 0; channel < 16; ++channel)
		{
			bank_msb[channel] = static_cast<uint8_t>(options.initial_bank >> 7);
			bank_lsb[channel] = static_cast<uint8_t>(options.initial_bank & 0x7f);
			programs[channel] = options.initial_program;
			volume_msb[channel] = 100;
			pan_msb[channel] = 64;
			expression_msb[channel] = 127;
			for (size_t controller = 0; controller < 128; ++controller)
			{
				controller_usage[channel][controller].channel = static_cast<uint8_t>(channel);
				controller_usage[channel][controller].controller =
					static_cast<uint8_t>(controller);
			}
		}
		std::map<uint32_t, uint64_t> usage;
		std::map<uint64_t, uint64_t> note_on_histogram;
		std::map<uint64_t, uint64_t> note_off_histogram;
		using NoteOnGroupKey = std::tuple<uint8_t, uint8_t, uint8_t, uint64_t>;
		using NoteOffGroupKey = std::tuple<uint8_t, uint8_t, uint64_t>;
		std::map<NoteOnGroupKey, uint64_t> sample_note_on_groups;
		std::map<NoteOffGroupKey, uint64_t> sample_note_off_groups;
		std::array<uint64_t, 16> channel_semantic_epoch{};
		uint64_t compatible_group_sample = 0;
		bool have_compatible_group_sample = false;
		std::array<std::vector<ActiveOnsetGroup>, 16 * 128> active_groups;
		std::array<bool, 16> sustain{};
		uint64_t active_logical_notes = 0;
		uint64_t active_onset_cohorts = 0;
		NoteRun note_run;
		auto finalize_compatible_groups = [&]() {
			for (const auto& [key, count] : sample_note_on_groups)
			{
				(void)key;
				++note_on_histogram[count];
				++analysis.note_on_groups;
				analysis.largest_identical_note_on_group = (std::max)(
					analysis.largest_identical_note_on_group, count);
				if (count == analysis.largest_identical_note_on_group)
					analysis.largest_note_on_group_sample = compatible_group_sample;
			}
			for (const auto& [key, count] : sample_note_off_groups)
			{
				(void)key;
				++note_off_histogram[count];
				++analysis.note_off_groups;
				analysis.largest_identical_note_off_group = (std::max)(
					analysis.largest_identical_note_off_group, count);
				if (count == analysis.largest_identical_note_off_group)
					analysis.largest_note_off_group_sample = compatible_group_sample;
			}
			sample_note_on_groups.clear();
			sample_note_off_groups.clear();
		};
		auto update_peaks = [&]() {
			analysis.estimated_peak_active_logical_notes = (std::max)(
				analysis.estimated_peak_active_logical_notes, active_logical_notes);
			analysis.estimated_peak_same_onset_cohorts = (std::max)(
				analysis.estimated_peak_same_onset_cohorts, active_onset_cohorts);
		};
		auto retire_empty = [&](ActiveOnsetGroup& group) {
			if (group.counted && group.held == 0 && group.sustained == 0)
			{
				group.counted = false;
				--active_onset_cohorts;
			}
		};
		auto trim_retired_back = [](std::vector<ActiveOnsetGroup>& groups) {
			while (!groups.empty() && !groups.back().counted)
				groups.pop_back();
		};
		auto release_notes = [&](uint8_t channel, uint8_t note, uint64_t count) {
			auto& groups = active_groups[static_cast<size_t>(channel) * 128 + note];
			while (count != 0)
			{
				auto found = groups.rend();
				for (auto it = groups.rbegin(); it != groups.rend(); ++it)
					if (it->held != 0)
					{
						found = it;
						break;
					}
				if (found == groups.rend())
					break;
				const uint64_t released = (std::min)(count, found->held);
				found->held -= released;
				if (sustain[channel])
					found->sustained += released;
				else
				{
					active_logical_notes -= released;
					active_channel_notes[channel] -= released;
				}
				count -= released;
				retire_empty(*found);
				trim_retired_back(groups);
			}
		};
		auto finalize_note_run = [&]() {
			if (!note_run.valid || note_run.count == 0)
				return;
			if (note_run.note_on)
			{
				auto& groups = active_groups[static_cast<size_t>(note_run.channel) * 128 +
					note_run.note];
				groups.push_back({note_run.count, 0, true});
				active_logical_notes += note_run.count;
				active_channel_notes[note_run.channel] += note_run.count;
				++active_onset_cohorts;
				update_peaks();
			}
			else
			{
				release_notes(note_run.channel, note_run.note, note_run.count);
			}
			note_run = {};
		};
		auto release_sustain = [&](uint8_t channel) {
			for (uint16_t note = 0; note < 128; ++note)
			{
				auto& groups = active_groups[static_cast<size_t>(channel) * 128 + note];
				for (auto& group : groups)
				{
					active_logical_notes -= group.sustained;
					active_channel_notes[channel] -= group.sustained;
					group.sustained = 0;
					retire_empty(group);
				}
				trim_retired_back(groups);
			}
		};
		auto release_channel = [&](uint8_t channel) {
			for (uint16_t note = 0; note < 128; ++note)
			{
				auto& groups = active_groups[static_cast<size_t>(channel) * 128 + note];
				for (const auto& group : groups)
				{
					active_logical_notes -= group.held + group.sustained;
					active_channel_notes[channel] -= group.held + group.sustained;
					if (group.counted)
						--active_onset_cohorts;
				}
				groups.clear();
			}
		};
		auto all_notes_off_channel = [&](uint8_t channel) {
			for (uint16_t note = 0; note < 128; ++note)
			{
				auto& groups = active_groups[static_cast<size_t>(channel) * 128 + note];
				for (auto& group : groups)
				{
					if (sustain[channel])
					{
						group.sustained += group.held;
						group.held = 0;
					}
					else
					{
						active_logical_notes -= group.held + group.sustained;
						active_channel_notes[channel] -= group.held + group.sustained;
						group.held = 0;
						group.sustained = 0;
						retire_empty(group);
					}
				}
				trim_retired_back(groups);
			}
		};
		uint64_t tick_group = 0;
		uint64_t sample_group = 0;
		uint64_t group_tick = 0;
		uint64_t group_sample = 0;
		bool first = true;
		ScheduledSmfEvent scheduled;
		while (stream.next(scheduled))
		{
			if (note_run.valid && scheduled.sample != note_run.sample)
				finalize_note_run();
			if (!have_compatible_group_sample || scheduled.sample != compatible_group_sample)
			{
				if (have_compatible_group_sample)
					finalize_compatible_groups();
				compatible_group_sample = scheduled.sample;
				have_compatible_group_sample = true;
			}
			++analysis.total_events;
			if (first || scheduled.event.tick != group_tick)
			{
				tick_group = 0;
				group_tick = scheduled.event.tick;
			}
			if (first || scheduled.sample != group_sample)
			{
				sample_group = 0;
				group_sample = scheduled.sample;
			}
			first = false;
			++tick_group;
			if (tick_group > analysis.maximum_events_same_tick)
			{
				analysis.maximum_events_same_tick = tick_group;
				analysis.maximum_events_tick_location = scheduled.event.tick;
			}
			++sample_group;
			if (sample_group > analysis.maximum_events_same_sample)
			{
				analysis.maximum_events_same_sample = sample_group;
				analysis.maximum_events_sample_location = scheduled.sample;
			}
			analysis.last_tick = scheduled.event.tick;
			analysis.duration_frames = scheduled.sample;

			switch (scheduled.event.kind)
			{
			case SmfEventKind::Channel:
			{
				++analysis.channel_events;
				const uint8_t command = scheduled.event.status & 0xf0U;
				const uint8_t channel = scheduled.event.status & 0x0fU;
				const bool is_note_on = command == 0x90 && scheduled.event.data2 != 0;
				const bool is_note_off = command == 0x80 ||
					(command == 0x90 && scheduled.event.data2 == 0);
				if (is_note_on || is_note_off)
				{
					const uint8_t velocity = is_note_on ? scheduled.event.data2 : 0;
					if (is_note_on)
						++sample_note_on_groups[NoteOnGroupKey{channel,
							scheduled.event.data1, velocity, channel_semantic_epoch[channel]}];
					else
						++sample_note_off_groups[NoteOffGroupKey{channel,
							scheduled.event.data1, channel_semantic_epoch[channel]}];
					const bool same_run = note_run.valid && note_run.sample == scheduled.sample &&
						note_run.note_on == is_note_on && note_run.channel == channel &&
						note_run.note == scheduled.event.data1 &&
						(!is_note_on || note_run.velocity == velocity);
					if (!same_run)
					{
						finalize_note_run();
						note_run = {true, is_note_on, channel, scheduled.event.data1,
							velocity, scheduled.sample, 0};
					}
					++note_run.count;
				}
				else
				{
					finalize_note_run();
					++channel_semantic_epoch[channel];
				}
				if (command == 0xb0)
				{
					const uint64_t active_channel_notes_before = active_channel_notes[channel];
					auto& controller = controller_usage[channel][scheduled.event.data1];
					if (!controller_seen[channel][scheduled.event.data1])
					{
						controller_seen[channel][scheduled.event.data1] = true;
						controller.minimum_value = scheduled.event.data2;
						controller.maximum_value = scheduled.event.data2;
						controller.first_tick = scheduled.event.tick;
						controller.first_sample = scheduled.sample;
					}
					else
					{
						controller.minimum_value = (std::min)(controller.minimum_value,
							scheduled.event.data2);
						controller.maximum_value = (std::max)(controller.maximum_value,
							scheduled.event.data2);
					}
					++controller.events;
					controller.last_tick = scheduled.event.tick;
					controller.last_sample = scheduled.sample;
					if (scheduled.event.data1 == 0) bank_msb[channel] = scheduled.event.data2;
					if (scheduled.event.data1 == 32) bank_lsb[channel] = scheduled.event.data2;
					if (scheduled.event.data1 == 7) volume_msb[channel] = scheduled.event.data2;
					if (scheduled.event.data1 == 39) volume_lsb[channel] = scheduled.event.data2;
					if (scheduled.event.data1 == 10) pan_msb[channel] = scheduled.event.data2;
					if (scheduled.event.data1 == 42) pan_lsb[channel] = scheduled.event.data2;
					if (scheduled.event.data1 == 11) expression_msb[channel] = scheduled.event.data2;
					if (scheduled.event.data1 == 43) expression_lsb[channel] = scheduled.event.data2;
					if (scheduled.event.data1 == 64)
					{
						const bool was_down = sustain[channel];
						sustain[channel] = scheduled.event.data2 >= 64;
						if (was_down && !sustain[channel])
							release_sustain(channel);
					}
					if (scheduled.event.data1 == 121)
					{
						if (sustain[channel])
							release_sustain(channel);
						sustain[channel] = false;
						volume_msb[channel] = 100;
						volume_lsb[channel] = 0;
						pan_msb[channel] = 64;
						pan_lsb[channel] = 0;
						expression_msb[channel] = 127;
						expression_lsb[channel] = 0;
					}
					if (scheduled.event.data1 == 120)
						release_channel(channel);
					if (scheduled.event.data1 == 123 || scheduled.event.data1 >= 124)
						all_notes_off_channel(channel);
					const bool traced_controller = scheduled.event.data1 == 7 ||
						scheduled.event.data1 == 39 || scheduled.event.data1 == 10 ||
						scheduled.event.data1 == 42 || scheduled.event.data1 == 11 ||
						scheduled.event.data1 == 43 || scheduled.event.data1 == 64 ||
						scheduled.event.data1 >= 120;
					const uint64_t trace_end = options.controller_trace_frames >
						(std::numeric_limits<uint64_t>::max)() - options.controller_trace_start_frame
						? (std::numeric_limits<uint64_t>::max)()
						: options.controller_trace_start_frame + options.controller_trace_frames;
					if (traced_controller && options.controller_trace_frames != 0 &&
						scheduled.sample >= options.controller_trace_start_frame &&
						scheduled.sample <= trace_end &&
						(options.controller_trace_controller < 0 ||
							scheduled.event.data1 == options.controller_trace_controller) &&
						analysis.controller_trace.size() < options.controller_trace_limit)
					{
						analysis.controller_trace.push_back({scheduled.event.tick, scheduled.sample,
							scheduled.event.track, scheduled.event.ordinal, channel,
							scheduled.event.data1, scheduled.event.data2,
							static_cast<uint16_t>((volume_msb[channel] << 7) | volume_lsb[channel]),
							static_cast<uint16_t>((pan_msb[channel] << 7) | pan_lsb[channel]),
							static_cast<uint16_t>((expression_msb[channel] << 7) |
								expression_lsb[channel]), active_channel_notes_before,
							active_channel_notes[channel], sustain[channel]});
					}
				}
				else if (command == 0xc0)
					programs[channel] = scheduled.event.data1;
				if (command == 0x90 && scheduled.event.data2 != 0)
				{
					++analysis.note_ons;
					const uint16_t bank = static_cast<uint16_t>(
						(static_cast<uint16_t>(bank_msb[channel]) << 7) | bank_lsb[channel]);
					const uint32_t key = (static_cast<uint32_t>(channel) << 21) |
						(static_cast<uint32_t>(bank) << 7) | programs[channel];
					++usage[key];
				}
				else if (command == 0x80 || (command == 0x90 && scheduled.event.data2 == 0))
					++analysis.note_offs;
				break;
			}
			case SmfEventKind::Tempo:
				++analysis.tempo_changes;
				++analysis.meta_events;
				break;
			case SmfEventKind::Meta:
			case SmfEventKind::EndOfTrack:
				++analysis.meta_events;
				break;
			case SmfEventKind::SystemExclusive:
				++analysis.sysex_events;
			{
				uint16_t master_volume = 0;
				if (decode_universal_master_volume(file, scheduled.event, master_volume))
					++analysis.universal_master_volume_events;
			}
				break;
			}
		}
		finalize_note_run();
		if (have_compatible_group_sample)
			finalize_compatible_groups();
		analysis.parser_state_bytes = stream.state_bytes();
		analysis.diagnostics = stream.diagnostics();
		if (!stream.good())
			return false;
		if (analysis.duration_frames > (std::numeric_limits<uint64_t>::max)() -
			options.tail_frames)
		{
			add_diagnostic(analysis.diagnostics, SmfDiagnosticSeverity::Error, UINT32_MAX, 0,
				"estimated output frame count overflow");
			return false;
		}
		analysis.estimated_output_frames = analysis.duration_frames + options.tail_frames;
		using Wide = dixelu::long_uint<0>;
		const Wide data_size = Wide(analysis.estimated_output_frames) * Wide(8);
		if (data_size[1] != 0)
		{
			add_diagnostic(analysis.diagnostics, SmfDiagnosticSeverity::Error, UINT32_MAX, 0,
				"estimated WAV data exceeds RF64's 64-bit size fields");
			return false;
		}
		analysis.requires_rf64 = data_size[0] >
			static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()) - 36ULL;
		const uint64_t header_bytes = analysis.requires_rf64 ? 80ULL : 44ULL;
		if (data_size[0] > (std::numeric_limits<uint64_t>::max)() - header_bytes)
		{
			add_diagnostic(analysis.diagnostics, SmfDiagnosticSeverity::Error, UINT32_MAX, 0,
				"estimated WAV file size overflow");
			return false;
		}
		analysis.estimated_output_bytes = data_size[0] + header_bytes;
		analysis.bank_program_usage.reserve(usage.size());
		for (const auto& [key, count] : usage)
			analysis.bank_program_usage.push_back({static_cast<uint8_t>(key >> 21),
				static_cast<uint16_t>((key >> 7) & 0x3fffU), static_cast<uint8_t>(key & 0x7fU),
				count});
		for (size_t channel = 0; channel < 16; ++channel)
			for (size_t controller = 0; controller < 128; ++controller)
				if (controller_seen[channel][controller])
					analysis.controller_usage.push_back(controller_usage[channel][controller]);
		analysis.note_on_group_histogram.reserve(note_on_histogram.size());
		for (const auto& [size, groups] : note_on_histogram)
			analysis.note_on_group_histogram.push_back({size, groups, size * groups});
		analysis.note_off_group_histogram.reserve(note_off_histogram.size());
		for (const auto& [size, groups] : note_off_histogram)
			analysis.note_off_group_histogram.push_back({size, groups, size * groups});
		analysis.estimated_same_onset_compression_ratio = analysis.note_on_groups == 0 ? 0.0 :
			static_cast<double>(analysis.note_ons) / static_cast<double>(analysis.note_on_groups);
		return true;
	}
	catch (...)
	{
		analysis.diagnostics = file.diagnostics();
		add_diagnostic(analysis.diagnostics, SmfDiagnosticSeverity::Error, UINT32_MAX, 0,
			"exception during incremental SMF analysis");
		return false;
	}
}

} // namespace safsyn
