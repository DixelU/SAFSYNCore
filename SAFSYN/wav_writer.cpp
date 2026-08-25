#include "wav_writer.h"

#include <fstream>
#include <limits>
#include <memory>
#include <utility>

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

void write_u64(std::ostream& stream, uint64_t value)
{
	const char bytes[] = {
		static_cast<char>(value & 0xff),
		static_cast<char>((value >> 8) & 0xff),
		static_cast<char>((value >> 16) & 0xff),
		static_cast<char>((value >> 24) & 0xff),
		static_cast<char>((value >> 32) & 0xff),
		static_cast<char>((value >> 40) & 0xff),
		static_cast<char>((value >> 48) & 0xff),
		static_cast<char>((value >> 56) & 0xff)
	};
	stream.write(bytes, sizeof(bytes));
}
}

struct FloatWavWriter::Impl
{
	std::ofstream stream;
	WavContainer container = WavContainer::Riff;
	uint64_t frames = 0;
	bool failed = false;
	bool open = false;
};

FloatWavWriter::FloatWavWriter() : impl_(std::make_unique<Impl>()) {}
FloatWavWriter::~FloatWavWriter()
{
	close();
}
FloatWavWriter::FloatWavWriter(FloatWavWriter&&) noexcept = default;
FloatWavWriter& FloatWavWriter::operator=(FloatWavWriter&& other) noexcept
{
	if (this != &other)
	{
		close();
		impl_ = std::move(other.impl_);
	}
	return *this;
}

bool FloatWavWriter::open(const char* path, uint32_t sample_rate,
	uint64_t expected_frames) noexcept
{
	if (!impl_ || !path || sample_rate == 0 ||
		sample_rate > (std::numeric_limits<uint32_t>::max)() / 8U ||
		expected_frames > (std::numeric_limits<uint64_t>::max)() / 8ULL)
		return false;
	close();
	impl_->frames = 0;
	impl_->failed = false;
	const uint64_t expected_data_size = expected_frames * 8ULL;
	impl_->container = expected_data_size >
		static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()) - 36ULL
		? WavContainer::Rf64 : WavContainer::Riff;
	impl_->stream.open(path, std::ios::binary | std::ios::trunc);
	if (!impl_->stream)
		return false;
	impl_->open = true;
	constexpr uint16_t channels = 2;
	constexpr uint16_t bytes_per_sample = sizeof(float);
	if (impl_->container == WavContainer::Riff)
	{
		impl_->stream.write("RIFF", 4);
		write_u32(impl_->stream, 0);
		impl_->stream.write("WAVE", 4);
	}
	else
	{
		impl_->stream.write("RF64", 4);
		write_u32(impl_->stream, (std::numeric_limits<uint32_t>::max)());
		impl_->stream.write("WAVE", 4);
		impl_->stream.write("ds64", 4);
		write_u32(impl_->stream, 28);
		write_u64(impl_->stream, 0); // RIFF size
		write_u64(impl_->stream, 0); // data size
		write_u64(impl_->stream, 0); // sample frames
		write_u32(impl_->stream, 0); // table entries
	}
	impl_->stream.write("fmt ", 4);
	write_u32(impl_->stream, 16);
	write_u16(impl_->stream, 3); // WAVE_FORMAT_IEEE_FLOAT
	write_u16(impl_->stream, channels);
	write_u32(impl_->stream, sample_rate);
	write_u32(impl_->stream, sample_rate * channels * bytes_per_sample);
	write_u16(impl_->stream, channels * bytes_per_sample);
	write_u16(impl_->stream, bytes_per_sample * 8);
	impl_->stream.write("data", 4);
	write_u32(impl_->stream, impl_->container == WavContainer::Rf64
		? (std::numeric_limits<uint32_t>::max)() : 0);
	impl_->failed = !impl_->stream.good();
	return !impl_->failed;
}

bool FloatWavWriter::write(const float* samples, size_t frames) noexcept
{
	if (!impl_ || !impl_->open || impl_->failed || (!samples && frames != 0) ||
		frames > ((std::numeric_limits<uint64_t>::max)() - impl_->frames))
		return false;
	const uint64_t new_frames = impl_->frames + frames;
	if (new_frames > (std::numeric_limits<uint64_t>::max)() / 8ULL)
	{
		impl_->failed = true;
		return false;
	}
	const uint64_t data_size = new_frames * 8ULL;
	if (impl_->container == WavContainer::Riff && data_size >
			static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()) - 36ULL)
	{
		impl_->failed = true;
		return false;
	}
	const uint64_t byte_count = static_cast<uint64_t>(frames) * 8ULL;
	if (byte_count > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)()))
	{
		impl_->failed = true;
		return false;
	}
	impl_->stream.write(reinterpret_cast<const char*>(samples),
		static_cast<std::streamsize>(byte_count));
	impl_->frames = new_frames;
	impl_->failed = !impl_->stream.good();
	return !impl_->failed;
}

bool FloatWavWriter::close() noexcept
{
	if (!impl_ || !impl_->open)
		return impl_ != nullptr;
	bool success = !impl_->failed;
	const uint64_t data_size = impl_->frames * 8ULL;
	if (success)
	{
		if (impl_->container == WavContainer::Riff)
		{
			impl_->stream.seekp(4);
			write_u32(impl_->stream, static_cast<uint32_t>(36ULL + data_size));
			impl_->stream.seekp(40);
			write_u32(impl_->stream, static_cast<uint32_t>(data_size));
		}
		else
		{
			impl_->stream.seekp(20);
			write_u64(impl_->stream, 72ULL + data_size);
			write_u64(impl_->stream, data_size);
			write_u64(impl_->stream, impl_->frames);
		}
		success = impl_->stream.good();
	}
	impl_->stream.close();
	success = success && !impl_->stream.fail();
	impl_->open = false;
	impl_->failed = !success;
	return success;
}

bool FloatWavWriter::is_open() const noexcept
{
	return impl_ && impl_->open;
}

uint64_t FloatWavWriter::frames_written() const noexcept
{
	return impl_ ? impl_->frames : 0;
}

WavContainer FloatWavWriter::container() const noexcept
{
	return impl_ ? impl_->container : WavContainer::Riff;
}

bool write_float_wav(const char* path, const float* samples, size_t frames,
	uint32_t sample_rate) noexcept
{
	if (!path || (!samples && frames != 0) || sample_rate == 0)
		return false;
	FloatWavWriter writer;
	return writer.open(path, sample_rate, frames) && writer.write(samples, frames) &&
		writer.close();
}

} // namespace safsyn
