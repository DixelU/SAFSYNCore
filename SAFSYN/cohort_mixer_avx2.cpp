#include "cohort_mixer.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#include <cstring>

#ifdef _MSC_VER
#pragma fp_contract(off)
#endif

namespace safsyn::detail
{
namespace
{
// The baseline renderer owns wrapping, filtering, and retirement. This kernel
// only consumes complete eight-frame runs before those boundaries. Keep every
// position/envelope update sequential and do not contract arithmetic into FMA.
struct Endpoints { __m256 first, second; };

int32_t pcm_pair(const int16_t* pcm) noexcept
{
	int32_t result;
	std::memcpy(&result, pcm, sizeof(result));
	return result;
}

Endpoints pcm_endpoints(const int16_t* pcm, const uint32_t* index) noexcept
{
	const auto pairs = _mm256_setr_epi32(pcm_pair(pcm + index[0]), pcm_pair(pcm + index[1]),
		pcm_pair(pcm + index[2]), pcm_pair(pcm + index[3]), pcm_pair(pcm + index[4]),
		pcm_pair(pcm + index[5]), pcm_pair(pcm + index[6]), pcm_pair(pcm + index[7]));
	const auto scale = _mm256_set1_ps(1.0f / 32768.0f);
	return {
		_mm256_mul_ps(scale, _mm256_cvtepi32_ps(_mm256_srai_epi32(_mm256_slli_epi32(pairs, 16), 16))),
		_mm256_mul_ps(scale, _mm256_cvtepi32_ps(_mm256_srai_epi32(pairs, 16)))};
}

struct Adjacent { __m256i first, second; };

// Scalar paired loads outperform hardware gathers on the target hybrid CPU.
// Each 64-bit load supplies both interpolation endpoints without overreading.
Adjacent adjacent_endpoints(const void* values, const uint32_t* index) noexcept
{
	const auto* bytes = static_cast<const unsigned char*>(values);
	auto pair = [&](size_t i) {
		return _mm_loadl_epi64(reinterpret_cast<const __m128i*>(bytes + size_t{index[i]} * 4));
	};
	const auto a = _mm_castsi128_ps(_mm_unpacklo_epi64(pair(0), pair(1)));
	const auto b = _mm_castsi128_ps(_mm_unpacklo_epi64(pair(2), pair(3)));
	const auto c = _mm_castsi128_ps(_mm_unpacklo_epi64(pair(4), pair(5)));
	const auto d = _mm_castsi128_ps(_mm_unpacklo_epi64(pair(6), pair(7)));
	return {
		_mm256_set_m128i(_mm_castps_si128(_mm_shuffle_ps(c, d, _MM_SHUFFLE(2, 0, 2, 0))),
			_mm_castps_si128(_mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0)))),
		_mm256_set_m128i(_mm_castps_si128(_mm_shuffle_ps(c, d, _MM_SHUFFLE(3, 1, 3, 1))),
			_mm_castps_si128(_mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1))))};
}

Endpoints quadrature_endpoints(const float* values, const uint32_t* index) noexcept
{
	const auto pair = adjacent_endpoints(values, index);
	return {_mm256_castsi256_ps(pair.first), _mm256_castsi256_ps(pair.second)};
}

__m256 join(__m128 low, __m128 high) noexcept
{
	return _mm256_insertf128_ps(_mm256_castps128_ps256(low), high, 1);
}

template<VectorSource Source>
struct SourceWeights
{
	__m256d count, cosine, sine;

	explicit SourceWeights(const PhaseAggregate& phase, bool right) noexcept
		: count(_mm256_set1_pd(static_cast<double>(phase.coherent_count))),
		cosine(_mm256_set1_pd(right ? phase.cosine_right : phase.cosine_left)),
		sine(_mm256_set1_pd(right ? phase.sine_right : phase.sine_left)) {}

	__m128 group(__m128 x, __m128 q) const noexcept
	{
		const auto xd = _mm256_cvtps_pd(x);
		auto value = _mm256_mul_pd(xd, count);
		if constexpr (Source == VectorSource::AnalyticSum)
			value = _mm256_add_pd(value, _mm256_sub_pd(_mm256_mul_pd(xd, cosine),
				_mm256_mul_pd(_mm256_cvtps_pd(q), sine)));
		return _mm256_cvtpd_ps(value);
	}

	__m256 apply(__m256 x, __m256 q) const noexcept
	{
		return join(group(_mm256_castps256_ps128(x), _mm256_castps256_ps128(q)),
			group(_mm256_extractf128_ps(x, 1), _mm256_extractf128_ps(q, 1)));
	}
};

// Failure must leave the envelope untouched: the baseline will consume the
// ending release frame and retire the cohort at the original sample position.
#ifdef _MSC_VER
__forceinline
#else
__attribute__((always_inline)) inline
#endif
bool envelope_frames(uint32_t rate, RenderCohort& cohort, __m256& envelope) noexcept
{
	if (cohort.stage == CohortStage::Sustain)
	{
		cohort.env = cohort.region->sustain;
		envelope = _mm256_set1_ps(cohort.env);
		return true;
	}
	alignas(32) float values[8];
	if (cohort.stage == CohortStage::Release)
	{
		if (cohort.finish_attack) return false;
		float value = cohort.env;
		for (auto& sample : values)
		{
			value *= cohort.release_coefficient;
			if (!(value > 1.0e-5f)) return false;
			sample = value;
		}
		cohort.env = value;
	}
	else
	{
		if (cohort.stage == CohortStage::Off) return false;
		for (auto& sample : values) sample = advance_envelope(rate, cohort);
	}
	envelope = _mm256_load_ps(values);
	return true;
}

template<VectorSource Source, bool Stereo, bool Planar>
uint32_t render_frames(SynthEngine& owner, RenderCohort& cohort, float* out,
	uint32_t frames, double increment, bool valid_loop, float amplitude) noexcept
{
	constexpr bool analytic = Source == VectorSource::AnalyticSum;
	const auto& region = *cohort.region;
	const auto& phase = cohort.phase;
	const double end = valid_loop ? region.loop_end : region.pcm_len;
	const auto gain_l = _mm256_set1_ps(cohort.gain_l);
	const auto gain_r = _mm256_set1_ps(cohort.gain_r);
	const auto amp = _mm256_set1_ps(amplitude);
	const SourceWeights<Source> left_weights(phase, false), right_weights(phase, true);
	[[maybe_unused]] const auto* ql = phase.quadrature_left;
	[[maybe_unused]] const auto* qr = phase.quadrature_right;
	[[maybe_unused]] const auto protect = phase.protected_frames();

	uint32_t frame = 0;
	for (; frames - frame >= 8; frame += 8)
	{
		const double p0 = cohort.pos;
		const double p1 = p0 + increment, p2 = p1 + increment;
		const double p3 = p2 + increment, p4 = p3 + increment;
		const double p5 = p4 + increment, p6 = p5 + increment;
		const double p7 = p6 + increment, p8 = p7 + increment;
		if (!(p0 >= 0.0 && p7 < end - 1.0 && p8 < end)) break;
		if constexpr (analytic)
			if (p0 < protect) break;
		const uint32_t indices[] = {static_cast<uint32_t>(p0), static_cast<uint32_t>(p1),
			static_cast<uint32_t>(p2), static_cast<uint32_t>(p3), static_cast<uint32_t>(p4),
			static_cast<uint32_t>(p5), static_cast<uint32_t>(p6), static_cast<uint32_t>(p7)};
		__m256 envelope;
		if (!envelope_frames(owner.sample_rate(), cohort, envelope)) break;
		const auto fraction = _mm256_setr_ps(static_cast<float>(p0 - indices[0]),
			static_cast<float>(p1 - indices[1]), static_cast<float>(p2 - indices[2]),
			static_cast<float>(p3 - indices[3]), static_cast<float>(p4 - indices[4]),
			static_cast<float>(p5 - indices[5]), static_cast<float>(p6 - indices[6]),
			static_cast<float>(p7 - indices[7]));
		Endpoints left, right;
		if constexpr (Stereo && !Planar)
		{
			const auto [first, second] = adjacent_endpoints(region.pcm, indices);
			const auto scale = _mm256_set1_ps(1.0f / 32768.0f);
			left = {_mm256_mul_ps(scale, _mm256_cvtepi32_ps(_mm256_srai_epi32(_mm256_slli_epi32(first, 16), 16))),
				_mm256_mul_ps(scale, _mm256_cvtepi32_ps(_mm256_srai_epi32(_mm256_slli_epi32(second, 16), 16)))};
			right = {_mm256_mul_ps(scale, _mm256_cvtepi32_ps(_mm256_srai_epi32(first, 16))),
				_mm256_mul_ps(scale, _mm256_cvtepi32_ps(_mm256_srai_epi32(second, 16)))};
		}
		else
		{
			left = pcm_endpoints(region.pcm, indices);
			if constexpr (Planar) right = pcm_endpoints(region.pcm_right, indices);
		}
		Endpoints qleft{_mm256_setzero_ps(), _mm256_setzero_ps()}, qright = qleft;
		if constexpr (analytic)
		{
			qleft = quadrature_endpoints(ql, indices);
			if constexpr (Stereo) qright = quadrature_endpoints(qr, indices);
		}
		auto interpolate = [&](Endpoints x, Endpoints q, const SourceWeights<Source>& weights) {
			const auto first = weights.apply(x.first, q.first);
			const auto second = weights.apply(x.second, q.second);
			return _mm256_add_ps(first, _mm256_mul_ps(_mm256_sub_ps(second, first), fraction));
		};
		const auto sample_l = interpolate(left, qleft, left_weights);
		auto sample_r = sample_l;
		if constexpr (Stereo) sample_r = interpolate(right, qright, right_weights);
		const auto l = _mm256_mul_ps(_mm256_mul_ps(_mm256_mul_ps(sample_l, envelope), gain_l), amp);
		const auto r = _mm256_mul_ps(_mm256_mul_ps(_mm256_mul_ps(sample_r, envelope), gain_r), amp);
		const auto low = _mm256_unpacklo_ps(l, r), high = _mm256_unpackhi_ps(l, r);
		float* destination = out + size_t{frame} * 2;
		_mm256_storeu_ps(destination, _mm256_add_ps(_mm256_loadu_ps(destination),
			_mm256_permute2f128_ps(low, high, 0x20)));
		_mm256_storeu_ps(destination + 8, _mm256_add_ps(_mm256_loadu_ps(destination + 8),
			_mm256_permute2f128_ps(low, high, 0x31)));
		cohort.pos = p8;
	}
	return frame;
}

template<VectorSource Source>
VectorKernel layout_kernel(const SampleRegion& region) noexcept
{
	if (region.channels != 2) return &render_frames<Source, false, false>;
	if (region.pcm_right) return &render_frames<Source, true, true>;
	return &render_frames<Source, true, false>;
}
}

VectorKernel avx2_kernel(VectorSource source, const SampleRegion& region) noexcept
{
	switch (source)
	{
	case VectorSource::CoherentSum: return layout_kernel<VectorSource::CoherentSum>(region);
	case VectorSource::AnalyticSum: return layout_kernel<VectorSource::AnalyticSum>(region);
	default: return nullptr;
	}
}
} // namespace safsyn::detail
#else
namespace safsyn::detail
{
VectorKernel avx2_kernel(VectorSource, const SampleRegion&) noexcept { return nullptr; }
}
#endif
