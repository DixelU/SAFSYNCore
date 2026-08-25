#pragma once

#include <cstddef>
#include <cstdint>

namespace safsyn
{

// Writes interleaved stereo IEEE float32 WAV without normalization or limiting.
bool write_float_wav(const char* path, const float* samples, size_t frames,
	uint32_t sample_rate) noexcept;

} // namespace safsyn
