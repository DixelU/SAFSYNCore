#pragma once

#include "core.h"
#include <limits>

// Internal state shared by the baseline and optional vector mixer.
namespace safsyn::detail
{
inline constexpr uint32_t invalid_index = (std::numeric_limits<uint32_t>::max)();

struct StableHandle
{
	uint32_t index = invalid_index;
	uint32_t generation = 0;

	bool valid() const noexcept { return index != invalid_index; }
};

struct PhaseAggregate
{
	uint64_t multiplicity = 0;
	uint64_t coherent_count = 0;
	uint64_t transformed_count = 0;
	double cosine_left = 0.0;
	double sine_left = 0.0;
	double cosine_right = 0.0;
	double sine_right = 0.0;
	const float* quadrature_left = nullptr;
	const float* quadrature_right = nullptr;
	uint32_t attack_hold_frames = 0;
	uint32_t attack_fade_frames = 0;
	PhaseVoiceState singleton;
	bool singleton_valid = false;
	// Filter linear basis signals, not the weighted sum. A note-off can then
	// split any phase contribution without guessing its individual history.
	detail::StereoFilterState original_filter;
	detail::StereoFilterState protected_filter;
	detail::StereoFilterState changed_filter;
	detail::StereoFilterState quadrature_filter;

	// Without a protected attack every blend is exactly one: the protected basis
	// only ever filters zeros and the changed basis equals the original one.
	// Attack settings are engine-wide, so a cohort never switches between forms.
	bool attack_window() const noexcept
	{
		return attack_hold_frames != 0 || attack_fade_frames != 0;
	}

	uint32_t protected_frames() const noexcept
	{
		return attack_hold_frames + attack_fade_frames;
	}

	void clear_filter_history() noexcept
	{
		original_filter = {};
		protected_filter = {};
		changed_filter = {};
		quadrature_filter = {};
	}

	void inherit_filter_history(const PhaseAggregate& source) noexcept
	{
		original_filter = source.original_filter;
		protected_filter = source.protected_filter;
		changed_filter = source.changed_filter;
		quadrature_filter = source.quadrature_filter;
	}

	std::array<float, 2> filtered_sample(const std::array<float, 2>& original0,
		const std::array<float, 2>& original1, uint32_t index0, uint32_t index1,
		float fraction, float envelope, const detail::FilterRamp& filter) noexcept
	{
		const bool window = attack_window();
		auto blend_at = [&](uint32_t index) {
			if (index < attack_hold_frames)
				return 0.0f;
			if (attack_fade_frames == 0 || index >= protected_frames())
				return 1.0f;
			const float u = static_cast<float>(index - attack_hold_frames) / attack_fade_frames;
			return u * u * (3.0f - 2.0f * u);
		};
		const float blend0 = window ? blend_at(index0) : 1.0f;
		const float blend1 = window ? blend_at(index1) : 1.0f;
		auto interpolate = [&](float a, float b) { return (a + (b - a) * fraction) * envelope; };
		std::array<float, 2> output{};
		for (size_t channel = 0; channel < 2; ++channel)
		{
			const float a = original0[channel];
			const float b = original1[channel];
			const float original = original_filter.process(interpolate(a, b), channel, filter);
			double result = static_cast<double>(original) * coherent_count;
			if (transformed_count != 0)
			{
				float changed = original;
				if (window)
				{
					const float protected_sample = protected_filter.process(
						interpolate(a * (1.0f - blend0), b * (1.0f - blend1)), channel, filter);
					changed = changed_filter.process(
						interpolate(a * blend0, b * blend1), channel, filter);
					result += static_cast<double>(protected_sample) * transformed_count;
				}
				if (quadrature_left)
				{
					const auto* quadrature = channel == 0 ? quadrature_left : quadrature_right;
					const float shifted = quadrature_filter.process(interpolate(
						quadrature[index0] * blend0, quadrature[index1] * blend1), channel, filter);
					result += static_cast<double>(changed) * (channel == 0 ? cosine_left : cosine_right) -
						static_cast<double>(shifted) * (channel == 0 ? sine_left : sine_right);
				}
			}
			output[channel] = static_cast<float>(result);
		}
		return output;
	}

	void add(const PhaseVoiceState& state, uint64_t count = 1)
	{
		const uint64_t previous_multiplicity = multiplicity;
		multiplicity += count;
		if (previous_multiplicity == 0 && count == 1)
		{
			singleton = state;
			singleton_valid = true;
		}
		else
			singleton_valid = false;
		if (state.kind == PhaseVoiceState::Kind::Coherent)
		{
			coherent_count += count;
			return;
		}
		transformed_count += count;
		attack_hold_frames = state.attack_hold_frames;
		attack_fade_frames = state.attack_fade_frames;
		quadrature_left = state.quadrature_left;
		quadrature_right = state.quadrature_right;
		cosine_left += static_cast<double>(state.cosine) * state.scale_left * count;
		sine_left += static_cast<double>(state.sine) * state.scale_left * count;
		cosine_right += static_cast<double>(state.cosine) * state.scale_right * count;
		sine_right += static_cast<double>(state.sine) * state.scale_right * count;
	}

	void add(const PhaseAggregate& other)
	{
		const uint64_t previous_multiplicity = multiplicity;
		multiplicity += other.multiplicity;
		if (previous_multiplicity == 0 && other.multiplicity == 1 && other.singleton_valid)
		{
			singleton = other.singleton;
			singleton_valid = true;
		}
		else if (other.multiplicity != 0)
			singleton_valid = false;
		coherent_count += other.coherent_count;
		transformed_count += other.transformed_count;
		cosine_left += other.cosine_left;
		sine_left += other.sine_left;
		cosine_right += other.cosine_right;
		sine_right += other.sine_right;
		if (other.quadrature_left)
		{
			quadrature_left = other.quadrature_left;
			quadrature_right = other.quadrature_right;
		}
		if (other.transformed_count != 0)
		{
			attack_hold_frames = other.attack_hold_frames;
			attack_fade_frames = other.attack_fade_frames;
		}
	}

	void subtract(const PhaseAggregate& other) noexcept
	{
		multiplicity -= other.multiplicity;
		coherent_count -= other.coherent_count;
		transformed_count -= other.transformed_count;
		cosine_left -= other.cosine_left;
		sine_left -= other.sine_left;
		cosine_right -= other.cosine_right;
		sine_right -= other.sine_right;
		if (multiplicity != 1)
			singleton_valid = false;
	}

	// The scalar reference for SIMD kernels; they must reproduce it exactly.
	float sample(float original, uint32_t index, bool right) const noexcept
	{
		if (multiplicity == 1 && singleton_valid)
			return singleton.apply(original, index, right);
		double changed = 0.0;
		if (quadrature_left)
		{
			const float* quadrature = right ? quadrature_right : quadrature_left;
			const double cosine = right ? cosine_right : cosine_left;
			const double sine = right ? sine_right : sine_left;
			changed = static_cast<double>(original) * cosine -
				static_cast<double>(quadrature[index]) * sine;
		}
		const double coherent = static_cast<double>(original) * coherent_count;
		if (transformed_count == 0)
			return static_cast<float>(coherent);
		const double original_transformed = static_cast<double>(original) * transformed_count;
		double transformed = changed;
		if (index < attack_hold_frames)
			transformed = original_transformed;
		else if (attack_fade_frames != 0 &&
			index < attack_hold_frames + attack_fade_frames)
		{
			const double u = static_cast<double>(index - attack_hold_frames) /
				attack_fade_frames;
			const double blend = u * u * (3.0 - 2.0 * u);
			transformed = original_transformed + (changed - original_transformed) * blend;
		}
		return static_cast<float>(coherent + transformed);
	}
};

enum class CohortStage : uint8_t { Off, Attack, Hold, Decay, Sustain, Release };

struct RenderCohort
{
	const SampleRegion* region = nullptr;
	size_t region_id = 0;
	uint8_t note = 0;
	uint8_t velocity = 0;
	uint8_t channel = 0;
	double pos = 0.0;
	double base_inc = 0.0;
	bool loop_dir_fwd = true;
	CohortStage stage = CohortStage::Off;
	float env = 0.0f;
	float env_inc = 0.0f;
	float release_coefficient = 0.0f;
	bool release_override = false;
	// Released during its attack: the attack still runs to full level first.
	bool finish_attack = false;
	uint32_t hold_samples_left = 0;
	float gain_l = 0.0f;
	float gain_r = 0.0f;
	uint64_t birth_frame = 0;
	uint64_t release_frame = 0;
	uint64_t oldest_serial = 0;
	StableHandle release_target;
	uint64_t release_target_frame = 0;
	PhaseAggregate phase;
	detail::FilterRamp filter;
};

// Internal linkage prevents the linker from substituting a copy compiled for
// AVX2 into baseline code in non-inlined/debug builds.
static inline float advance_envelope(uint32_t sample_rate, RenderCohort& cohort) noexcept
{
	switch (cohort.stage)
	{
	case CohortStage::Off:
		return 0.0f;
	case CohortStage::Attack:
		cohort.env += cohort.env_inc;
		if (cohort.env >= 1.0f)
		{
			cohort.env = 1.0f;
			if (cohort.hold_samples_left > 0)
				cohort.stage = CohortStage::Hold;
			else if (cohort.region->decay > 0.0f && cohort.region->sustain < 1.0f)
			{
				cohort.stage = CohortStage::Decay;
				cohort.env_inc = (cohort.region->sustain - 1.0f) /
					(cohort.region->decay * sample_rate);
			}
			else
				cohort.stage = CohortStage::Sustain;
		}
		break;
	case CohortStage::Hold:
		if (cohort.hold_samples_left > 0)
			--cohort.hold_samples_left;
		if (cohort.hold_samples_left == 0)
		{
			if (cohort.region->decay > 0.0f && cohort.region->sustain < 1.0f)
			{
				cohort.stage = CohortStage::Decay;
				cohort.env_inc = (cohort.region->sustain - 1.0f) /
					(cohort.region->decay * sample_rate);
			}
			else
				cohort.stage = CohortStage::Sustain;
		}
		break;
	case CohortStage::Decay:
		cohort.env += cohort.env_inc;
		if (cohort.env <= cohort.region->sustain)
		{
			cohort.stage = CohortStage::Sustain;
			cohort.env = cohort.region->sustain;
		}
		break;
	case CohortStage::Sustain:
		cohort.env = cohort.region->sustain;
		break;
	case CohortStage::Release:
		if (cohort.finish_attack)
		{
			// The same steps the attack would have taken, then the release.
			cohort.env += cohort.env_inc;
			if (cohort.env >= 1.0f)
			{
				cohort.env = 1.0f;
				cohort.finish_attack = false;
			}
			break;
		}
		cohort.env *= cohort.release_coefficient;
		if (cohort.env <= 1.0e-5f)
		{
			cohort.stage = CohortStage::Off;
			return 0.0f;
		}
		break;
	}
	return cohort.env;
}

// Each form retains the float/double operation order of its scalar reference.
enum class VectorSource : uint8_t { Coherent, CoherentSum, AnalyticSingle, AnalyticSum };
using VectorKernel = uint32_t (*)(SynthEngine&, RenderCohort&, float*, uint32_t,
	double, bool, float) noexcept;
VectorKernel avx2_kernel(VectorSource source, const SampleRegion& region) noexcept;
} // namespace safsyn::detail
