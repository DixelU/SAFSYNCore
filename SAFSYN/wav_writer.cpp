#include "wav_writer.h"

#include <fstream>
#include <limits>

namespace safsyn
{
namespace
{
void write_u16(std::ostream& stream, uint16_t value)
{
	const char bytes[] = {
		static_cast<char>(value & 0xff),
		static_cast<char>((value >> 8) & 0xff)
	};
	stream.write(bytes, sizeof(bytes));
}

void write_u32(std::ostream& stream, uint32_t value)
{
	const char bytes[] = {
		static_cast<char>(value & 0xff),
		static_cast<char>((value >> 8) & 0xff),
		static_cast<char>((value >> 16) & 0xff),
		static_cast<char>((value >> 24) & 0xff)
	};
	stream.write(bytes, sizeof(bytes));
}
}

bool write_float_wav(const char* path, const float* samples, size_t frames,
	uint32_t sample_rate) noexcept
{
	if (!path || (!samples && frames != 0) || sample_rate == 0)
		return false;
	constexpr uint32_t channels = 2;
	constexpr uint32_t bytes_per_sample = sizeof(float);
	constexpr uint32_t fmt_size = 16;
	const uint64_t data_size64 = static_cast<uint64_t>(frames) * channels * bytes_per_sample;
	if (data_size64 > std::numeric_limits<uint32_t>::max() - 36u)
		return false;
	const uint32_t data_size = static_cast<uint32_t>(data_size64);

	std::ofstream stream(path, std::ios::binary);
	if (!stream)
		return false;
	stream.write("RIFF", 4);
	write_u32(stream, 36u + data_size);
	stream.write("WAVE", 4);
	stream.write("fmt ", 4);
	write_u32(stream, fmt_size);
	write_u16(stream, 3); // WAVE_FORMAT_IEEE_FLOAT
	write_u16(stream, channels);
	write_u32(stream, sample_rate);
	write_u32(stream, sample_rate * channels * bytes_per_sample);
	write_u16(stream, channels * bytes_per_sample);
	write_u16(stream, bytes_per_sample * 8);
	stream.write("data", 4);
	write_u32(stream, data_size);
	stream.write(reinterpret_cast<const char*>(samples), data_size);
	return stream.good();
}

} // namespace safsyn
