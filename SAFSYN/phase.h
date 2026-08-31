#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>

namespace safsyn
{

struct SampleRegion;

enum class PhaseMode : uint8_t
{
	Coherent,
	RandomPolarity,
	Analytic,
	SmoothField,
	IndependentBins,
};

struct PhaseSettings
{
	PhaseMode mode = PhaseMode::Coherent;
	float strength = 1.0f;
	uint64_t seed = 0;
	uint32_t pool_size = 64;
	bool continuous = false;
	float correlation_hz = 250.0f;
	float preserve_attack_ms = 0.0f;
};

struct PhaseCacheStats
{
	uint64_t cached_samples = 0;
	uint64_t analytic_samples = 0;
	uint64_t cached_variants = 0;
	uint64_t cache_bytes = 0;
	uint64_t assignments = 0;
	uint64_t failures = 0;
	double preprocessing_ms = 0.0;
};

struct PhasePreparationProgress
{
	uint64_t completed = 0; // Unique sample transforms; one per FFT pool entry.
	uint64_t total = 0;
	uint64_t cache_bytes = 0;
	uint64_t total_cache_bytes = 0; // PCM cache only; temporary FFT memory is extra.
};

struct PhasePreparationOptions
{
	uint64_t maximum_cache_bytes = uint64_t{2048} * 1024 * 1024;
	std::stop_token stop;
	std::function<void(const PhasePreparationProgress&)> progress;
};

struct PhaseVoiceState
{
	enum class Kind : uint8_t { Coherent, Polarity, Analytic, Variant };

	Kind kind = Kind::Coherent;
	const float* quadrature_left = nullptr;
	const float* quadrature_right = nullptr;
	const float* variant_left = nullptr;
	const float* variant_right = nullptr;
	float cosine = 1.0f;
	float sine = 0.0f;
	float scale_left = 1.0f;
	float scale_right = 1.0f;
	float polarity = 1.0f;
	uint32_t attack_hold_frames = 0;
	uint32_t attack_fade_frames = 0;

	float apply(float original, uint32_t index, bool right) const noexcept
	{
		if (kind == Kind::Coherent)
			return original;
		float changed = original;
		switch (kind)
		{
		case Kind::Coherent:
			break;
		case Kind::Polarity:
			changed = original * polarity;
			break;
		case Kind::Analytic:
		{
			const float* quadrature = right ? quadrature_right : quadrature_left;
			const float scale = right ? scale_right : scale_left;
			changed = (cosine * original - sine * quadrature[index]) * scale;
			break;
		}
		case Kind::Variant:
		{
			const float* variant = right ? variant_right : variant_left;
			changed = variant[index];
			break;
		}
		}
		if (index < attack_hold_frames)
			return original;
		if (attack_fade_frames == 0 || index >= attack_hold_frames + attack_fade_frames)
			return changed;
		const float u = static_cast<float>(index - attack_hold_frames) /
			static_cast<float>(attack_fade_frames);
		const float blend = u * u * (3.0f - 2.0f * u);
		return original + (changed - original) * blend;
	}
};

class PhaseProcessor
{
public:
	PhaseProcessor();
	~PhaseProcessor();
	PhaseProcessor(const PhaseProcessor&) = delete;
	PhaseProcessor& operator=(const PhaseProcessor&) = delete;
	PhaseProcessor(PhaseProcessor&&) noexcept;
	PhaseProcessor& operator=(PhaseProcessor&&) noexcept;

	void configure(const PhaseSettings& settings) noexcept;
	const PhaseSettings& settings() const noexcept;
	void clear() noexcept;
	// Prepare every unique sample (and every finite FFT variant) without assigning
	// notes or advancing serials. Region indices must match subsequent assign().
	// Returns false on cancellation; throws on a budget/allocation failure. Call
	// on the engine owner thread before rendering, not from the audio callback.
	bool prepare(std::span<const SampleRegion> regions, const PhasePreparationOptions& options = {});
	PhaseVoiceState assign(const SampleRegion& region, uint64_t region_id,
		uint64_t event_serial, uint8_t channel, uint8_t note) noexcept;
	// Rebuild an already-assigned logical event contribution without counting a
	// second assignment. Cohort note-off splitting uses this to subtract the
	// exact deterministic phase contribution without storing every event angle.
	PhaseVoiceState reconstruct(const SampleRegion& region, uint64_t region_id,
		uint64_t event_serial, uint8_t channel, uint8_t note) noexcept;
	PhaseCacheStats stats() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace safsyn
