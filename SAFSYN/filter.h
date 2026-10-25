#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace safsyn::detail
{

struct FilterCoefficients
{
	float b0 = 1.0f;
	float b1 = 0.0f;
	float a1 = 0.0f;
	float a2 = 0.0f;
	bool enabled = false;

	bool operator==(const FilterCoefficients&) const noexcept = default;
};

// Persistent, sample-counted interpolation keeps automation independent of the
// caller's render block size. Bypass transitions crossfade the same filter.
struct FilterRamp
{
	FilterCoefficients current;
	FilterCoefficients target;
	std::array<float, 4> step{};
	float mix = 0.0f;
	float mix_step = 0.0f;
	uint32_t remaining = 0;

	bool active() const noexcept { return mix != 0.0f || target.enabled; }

	// True means filtering is starting with a fresh history.
	bool set(FilterCoefficients next, bool immediate = false) noexcept
	{
		if (!immediate && next == target)
			return false;
		const bool reset = !active() && next.enabled;
		target = next;
		if (immediate)
		{
			current = target;
			mix = target.enabled ? 1.0f : 0.0f;
			remaining = 0;
			return true;
		}
		if (reset)
			current = target;
		const auto end = target.enabled ? target : current;
		constexpr float inverse_frames = 1.0f / 32.0f;
		step = {(end.b0 - current.b0) * inverse_frames,
			(end.b1 - current.b1) * inverse_frames,
			(end.a1 - current.a1) * inverse_frames,
			(end.a2 - current.a2) * inverse_frames};
		mix_step = ((target.enabled ? 1.0f : 0.0f) - mix) * inverse_frames;
		remaining = 32;
		return reset;
	}

	void advance() noexcept
	{
		if (remaining == 0)
			return;
		if (--remaining == 0)
		{
			current = target;
			mix = target.enabled ? 1.0f : 0.0f;
			return;
		}
		current.b0 += step[0];
		current.b1 += step[1];
		current.a1 += step[2];
		current.a2 += step[3];
		mix += mix_step;
	}
};

// Pan and volume changes glide a sounding voice's left/right output gains
// linearly, as BASSMIDI does over about 4 ms; a stepped gain tears the
// waveform and fast automation turns into a click train. Idle ramps leave the
// voice on its steady gain * amplitude formula.
struct GainRamp
{
	std::array<float, 2> current{};
	std::array<float, 2> target{};
	std::array<float, 2> step{};
	uint32_t remaining = 0;

	bool active() const noexcept { return remaining != 0; }

	// from is the steady output gain, used only when no ramp is running.
	void set(std::array<float, 2> from, std::array<float, 2> to, uint32_t frames) noexcept
	{
		if (active() ? to == target : from == to)
			return;
		if (frames == 0)
		{
			remaining = 0;
			return;
		}
		if (!active())
			current = from;
		target = to;
		const float inverse_frames = 1.0f / static_cast<float>(frames);
		step = {(target[0] - current[0]) * inverse_frames,
			(target[1] - current[1]) * inverse_frames};
		remaining = frames;
	}

	void advance() noexcept
	{
		if (remaining == 0)
			return;
		if (--remaining == 0)
		{
			current = target;
			return;
		}
		current[0] += step[0];
		current[1] += step[1];
	}
};

struct StereoFilterState
{
	std::array<float, 2> z1{};
	std::array<float, 2> z2{};

	float process(float input, std::size_t channel, const FilterRamp& filter) noexcept
	{
		const auto& c = filter.current;
		const float wet = c.b0 * input + z1[channel];
		z1[channel] = c.b1 * input - c.a1 * wet + z2[channel];
		z2[channel] = c.b0 * input - c.a2 * wet;
		return input + (wet - input) * filter.mix;
	}
};

} // namespace safsyn::detail
