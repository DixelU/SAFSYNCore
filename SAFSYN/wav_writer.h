#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace safsyn
{

enum class WavContainer : uint8_t { Riff, Rf64 };

class FloatWavWriter
{
public:
	FloatWavWriter();
	~FloatWavWriter();
	FloatWavWriter(const FloatWavWriter&) = delete;
	FloatWavWriter& operator=(const FloatWavWriter&) = delete;
	FloatWavWriter(FloatWavWriter&&) noexcept;
	FloatWavWriter& operator=(FloatWavWriter&&) noexcept;

	// expected_frames selects RIFF when it fits and RF64 otherwise. The final
	// sizes are patched from the number of frames actually written.
	bool open(const char* path, uint32_t sample_rate, uint64_t expected_frames) noexcept;
	bool write(const float* interleaved_stereo, size_t frames) noexcept;
	bool close() noexcept;
	bool is_open() const noexcept;
	uint64_t frames_written() const noexcept;
	WavContainer container() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// Writes interleaved stereo IEEE float32 WAV without normalization or limiting.
bool write_float_wav(const char* path, const float* samples, size_t frames,
	uint32_t sample_rate) noexcept;

} // namespace safsyn
