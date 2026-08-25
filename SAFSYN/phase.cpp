#include "phase.h"

#include "core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace safsyn
{
namespace
{
using Complex = std::complex<double>;
constexpr double pi = 3.1415926535897932384626433832795;

uint64_t splitmix64(uint64_t value) noexcept
{
	value += 0x9e3779b97f4a7c15ULL;
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

uint64_t hash_combine(uint64_t seed, uint64_t value) noexcept
{
	return splitmix64(seed ^ splitmix64(value + 0x9e3779b97f4a7c15ULL));
}

double unit_from_hash(uint64_t value) noexcept
{
	return static_cast<double>(value >> 11) * (1.0 / 9007199254740992.0);
}

class StableRandom
{
public:
	explicit StableRandom(uint64_t seed) : state_(seed) {}
	double uniform(double lo, double hi) noexcept
	{
		state_ = splitmix64(state_);
		return lo + (hi - lo) * unit_from_hash(state_);
	}

private:
	uint64_t state_;
};

size_t next_power_of_two(size_t value)
{
	size_t result = 1;
	while (result < value)
	{
		if (result > (std::numeric_limits<size_t>::max)() / 2)
			throw std::bad_alloc();
		result *= 2;
	}
	return result;
}

bool is_power_of_two(size_t value) noexcept
{
	return value != 0 && (value & (value - 1)) == 0;
}

void radix2_fft(std::vector<Complex>& values, bool inverse)
{
	const size_t n = values.size();
	for (size_t i = 1, j = 0; i < n; ++i)
	{
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j)
			std::swap(values[i], values[j]);
	}
	for (size_t length = 2; length <= n; length <<= 1)
	{
		const double angle = (inverse ? 2.0 : -2.0) * pi / static_cast<double>(length);
		const Complex step(std::cos(angle), std::sin(angle));
		for (size_t start = 0; start < n; start += length)
		{
			Complex factor(1.0, 0.0);
			for (size_t offset = 0; offset < length / 2; ++offset)
			{
				const Complex even = values[start + offset];
				const Complex odd = values[start + offset + length / 2] * factor;
				values[start + offset] = even + odd;
				values[start + offset + length / 2] = even - odd;
				factor *= step;
			}
		}
		if (length == n)
			break;
	}
	if (inverse)
		for (Complex& value : values)
			value /= static_cast<double>(n);
}

std::vector<Complex> forward_fft_any(const std::vector<Complex>& input)
{
	const size_t n = input.size();
	if (n == 0)
		return {};
	if (is_power_of_two(n))
	{
		auto result = input;
		radix2_fft(result, false);
		return result;
	}

	const size_t convolution_size = next_power_of_two(n * 2 - 1);
	std::vector<Complex> a(convolution_size), b(convolution_size);
	for (size_t index = 0; index < n; ++index)
	{
		const double i = static_cast<double>(index);
		const double angle = pi * std::fmod(i * i, 2.0 * static_cast<double>(n)) /
			static_cast<double>(n);
		const Complex negative(std::cos(angle), -std::sin(angle));
		const Complex positive(std::cos(angle), std::sin(angle));
		a[index] = input[index] * negative;
		b[index] = positive;
		if (index != 0)
			b[convolution_size - index] = positive;
	}
	radix2_fft(a, false);
	radix2_fft(b, false);
	for (size_t index = 0; index < convolution_size; ++index)
		a[index] *= b[index];
	radix2_fft(a, true);
	a.resize(n);
	for (size_t index = 0; index < n; ++index)
	{
		const double i = static_cast<double>(index);
		const double angle = pi * std::fmod(i * i, 2.0 * static_cast<double>(n)) /
			static_cast<double>(n);
		a[index] *= Complex(std::cos(angle), -std::sin(angle));
	}
	return a;
}

std::vector<Complex> inverse_fft_any(const std::vector<Complex>& input)
{
	std::vector<Complex> conjugated(input.size());
	for (size_t index = 0; index < input.size(); ++index)
		conjugated[index] = std::conj(input[index]);
	auto result = forward_fft_any(conjugated);
	const double scale = input.empty() ? 1.0 : static_cast<double>(input.size());
	for (Complex& value : result)
		value = std::conj(value) / scale;
	return result;
}

std::vector<float> periodic_quadrature(const std::vector<double>& input)
{
	std::vector<Complex> complex_input(input.size());
	for (size_t index = 0; index < input.size(); ++index)
		complex_input[index] = Complex(input[index], 0.0);
	auto spectrum = forward_fft_any(complex_input);
	const size_t n = spectrum.size();
	for (size_t bin = 1; bin < (n + 1) / 2; ++bin)
		spectrum[bin] *= 2.0;
	for (size_t bin = n / 2 + 1; bin < n; ++bin)
		spectrum[bin] = Complex{};
	if (n % 2 != 0)
		for (size_t bin = (n + 1) / 2; bin < n; ++bin)
			spectrum[bin] = Complex{};
	auto analytic = inverse_fft_any(spectrum);
	std::vector<float> result(n);
	for (size_t index = 0; index < n; ++index)
		result[index] = static_cast<float>(analytic[index].imag());
	return result;
}

std::vector<float> padded_quadrature(const std::vector<double>& input)
{
	if (input.empty())
		return {};
	const size_t padded_size = next_power_of_two(input.size() * 2);
	std::vector<Complex> analytic(padded_size);
	for (size_t index = 0; index < input.size(); ++index)
		analytic[index] = Complex(input[index], 0.0);
	radix2_fft(analytic, false);
	for (size_t bin = 1; bin < padded_size / 2; ++bin)
		analytic[bin] *= 2.0;
	for (size_t bin = padded_size / 2 + 1; bin < padded_size; ++bin)
		analytic[bin] = Complex{};
	radix2_fft(analytic, true);
	std::vector<float> result(input.size());
	for (size_t index = 0; index < input.size(); ++index)
		result[index] = static_cast<float>(analytic[index].imag());
	return result;
}

double smoothstep(double value) noexcept
{
	value = std::clamp(value, 0.0, 1.0);
	return value * value * (3.0 - 2.0 * value);
}

void install_periodic_body(std::vector<float>& destination,
	const std::vector<float>& periodic, uint32_t loop_start, uint32_t sample_rate)
{
	if (periodic.empty() || loop_start + periodic.size() > destination.size())
		return;
	const size_t fade = (std::min)({static_cast<size_t>(loop_start), periodic.size() / 4,
		static_cast<size_t>(std::llround(sample_rate * 0.010))});
	for (size_t offset = 0; offset < fade; ++offset)
	{
		const size_t destination_index = loop_start - fade + offset;
		const size_t periodic_index = periodic.size() - fade + offset;
		const double blend = smoothstep(static_cast<double>(offset + 1) /
			static_cast<double>(fade + 1));
		destination[destination_index] = static_cast<float>(
			destination[destination_index] * (1.0 - blend) + periodic[periodic_index] * blend);
	}
	std::copy(periodic.begin(), periodic.end(), destination.begin() + loop_start);
}

std::vector<double> make_phase_delta(size_t n, uint32_t sample_rate, PhaseMode mode,
	float strength, float correlation_hz, uint64_t seed)
{
	const size_t bins = n / 2 + 1;
	std::vector<double> delta(bins, 0.0);
	if (bins <= 1)
		return delta;
	StableRandom random(seed);
	const double extent = pi * static_cast<double>(strength);
	if (mode == PhaseMode::IndependentBins)
	{
		for (size_t bin = 1; bin < bins; ++bin)
			delta[bin] = random.uniform(-extent, extent);
	}
	else
	{
		const double bin_hz = static_cast<double>(sample_rate) / static_cast<double>(n);
		const size_t stride = (std::max)(size_t{1}, static_cast<size_t>(std::llround(
			static_cast<double>(correlation_hz) / (std::max)(bin_hz, 1e-12))));
		std::vector<size_t> anchor_bins;
		std::vector<double> anchor_values;
		for (size_t bin = 0; bin < bins; bin += stride)
		{
			anchor_bins.push_back(bin);
			anchor_values.push_back(random.uniform(-extent, extent));
		}
		if (anchor_bins.empty() || anchor_bins.back() != bins - 1)
		{
			anchor_bins.push_back(bins - 1);
			anchor_values.push_back(random.uniform(-extent, extent));
		}
		for (size_t anchor = 0; anchor + 1 < anchor_bins.size(); ++anchor)
		{
			const size_t first = anchor_bins[anchor];
			const size_t last = anchor_bins[anchor + 1];
			for (size_t bin = first; bin <= last; ++bin)
			{
				const double fraction = last == first ? 0.0 :
					static_cast<double>(bin - first) / static_cast<double>(last - first);
				delta[bin] = anchor_values[anchor] +
					(anchor_values[anchor + 1] - anchor_values[anchor]) * fraction;
			}
		}
	}
	delta[0] = 0.0;
	if (n % 2 == 0)
		delta.back() = 0.0;
	return delta;
}

std::vector<float> apply_phase_field(const std::vector<double>& input,
	const std::vector<double>& delta)
{
	std::vector<Complex> complex_input(input.size());
	for (size_t index = 0; index < input.size(); ++index)
		complex_input[index] = Complex(input[index], 0.0);
	auto spectrum = forward_fft_any(complex_input);
	const size_t n = spectrum.size();
	for (size_t bin = 1; bin < (n + 1) / 2; ++bin)
	{
		const Complex rotation(std::cos(delta[bin]), std::sin(delta[bin]));
		spectrum[bin] *= rotation;
		spectrum[n - bin] = std::conj(spectrum[bin]);
	}
	auto changed = inverse_fft_any(spectrum);
	std::vector<float> result(n);
	for (size_t index = 0; index < n; ++index)
		result[index] = static_cast<float>(changed[index].real());
	return result;
}

double mean_square(const std::vector<double>& values) noexcept
{
	double sum = 0.0;
	for (double value : values)
		sum += value * value;
	return values.empty() ? 0.0 : sum / static_cast<double>(values.size());
}

void rms_match(std::vector<float>& changed, const std::vector<double>& original)
{
	double changed_sum = 0.0;
	for (float value : changed)
		changed_sum += static_cast<double>(value) * value;
	const double changed_ms = changed.empty() ? 0.0 : changed_sum / changed.size();
	const double original_ms = mean_square(original);
	const double scale = changed_ms > 1e-30 ? std::sqrt(original_ms / changed_ms) : 1.0;
	for (float& value : changed)
		value = static_cast<float>(value * scale);
}

float pcm_to_float(int16_t value) noexcept
{
	return static_cast<float>(value) / 32768.0f;
}

std::vector<double> read_channel(const SampleRegion& region, bool right)
{
	std::vector<double> result(region.pcm_len);
	for (uint32_t index = 0; index < region.pcm_len; ++index)
	{
		int16_t sample = 0;
		if (right && region.channels == 2)
			sample = region.pcm_right ? region.pcm_right[index] :
				region.pcm[static_cast<size_t>(index) * 2 + 1];
		else
			sample = region.pcm_right ? region.pcm[index] :
				region.pcm[static_cast<size_t>(index) * region.channels];
		result[index] = pcm_to_float(sample);
	}
	return result;
}

bool has_valid_loop(const SampleRegion& region) noexcept
{
	const bool looping = region.loop_mode == LoopMode::Forward ||
		region.loop_mode == LoopMode::Sustain || region.loop_mode == LoopMode::PingPong;
	return looping && region.loop_start < region.loop_end && region.loop_end <= region.pcm_len &&
		region.loop_end - region.loop_start >= 2;
}

bool same_settings(const PhaseSettings& left, const PhaseSettings& right) noexcept
{
	return left.mode == right.mode && left.strength == right.strength &&
		left.seed == right.seed && left.pool_size == right.pool_size &&
		left.continuous == right.continuous && left.correlation_hz == right.correlation_hz &&
		left.preserve_attack_ms == right.preserve_attack_ms;
}
} // namespace

struct PhaseProcessor::Impl
{
	struct SampleKey
	{
		uint64_t logical_id = 0;
		const int16_t* left = nullptr;
		const int16_t* right = nullptr;
		uint32_t length = 0;
		uint32_t sample_rate = 0;
		uint32_t loop_start = 0;
		uint32_t loop_end = 0;
		uint8_t channels = 0;
		LoopMode loop_mode = LoopMode::None;

		bool operator==(const SampleKey& other) const noexcept
		{
			return logical_id == other.logical_id && left == other.left && right == other.right &&
				length == other.length && sample_rate == other.sample_rate &&
				loop_start == other.loop_start && loop_end == other.loop_end &&
				channels == other.channels && loop_mode == other.loop_mode;
		}
	};

	struct SampleKeyHash
	{
		size_t operator()(const SampleKey& key) const noexcept
		{
			uint64_t hash = hash_combine(key.logical_id, key.length);
			hash = hash_combine(hash, reinterpret_cast<uintptr_t>(key.left));
			hash = hash_combine(hash, reinterpret_cast<uintptr_t>(key.right));
			hash = hash_combine(hash, (static_cast<uint64_t>(key.loop_start) << 32) | key.loop_end);
			hash = hash_combine(hash, static_cast<uint64_t>(key.loop_mode));
			return static_cast<size_t>(hash);
		}
	};

	struct Variant
	{
		std::vector<float> left;
		std::vector<float> right;
	};

	struct Entry
	{
		SampleKey key;
		std::vector<float> quadrature_left;
		std::vector<float> quadrature_right;
		double original_energy_left = 0.0;
		double quadrature_energy_left = 0.0;
		double cross_energy_left = 0.0;
		double original_energy_right = 0.0;
		double quadrature_energy_right = 0.0;
		double cross_energy_right = 0.0;
		std::vector<std::unique_ptr<Variant>> variants;
		std::vector<bool> analytic_variants_seen;
	};

	PhaseSettings settings;
	PhaseCacheStats statistics;
	std::unordered_map<SampleKey, std::unique_ptr<Entry>, SampleKeyHash> entries;

	void reset_cache() noexcept
	{
		entries.clear();
		statistics = {};
	}

	SampleKey make_key(const SampleRegion& region, uint64_t region_id) const noexcept
	{
		SampleKey key;
		key.logical_id = region.logical_sample_id != 0 ? region.logical_sample_id : region_id + 1;
		key.left = region.pcm;
		key.right = region.pcm_right;
		key.length = region.pcm_len;
		key.sample_rate = region.sample_rate;
		key.loop_start = region.loop_start;
		key.loop_end = region.loop_end;
		key.channels = region.channels;
		key.loop_mode = region.loop_mode;
		return key;
	}

	Entry& entry_for(const SampleRegion& region, uint64_t region_id)
	{
		const SampleKey key = make_key(region, region_id);
		auto found = entries.find(key);
		if (found != entries.end())
			return *found->second;
		auto entry = std::make_unique<Entry>();
		entry->key = key;
		entry->variants.resize(settings.pool_size);
		entry->analytic_variants_seen.resize(settings.pool_size, false);
		Entry* result = entry.get();
		entries.emplace(key, std::move(entry));
		++statistics.cached_samples;
		return *result;
	}

	void compute_energy(const std::vector<double>& original, const std::vector<float>& quadrature,
		double& original_energy, double& quadrature_energy, double& cross_energy)
	{
		original_energy = quadrature_energy = cross_energy = 0.0;
		for (size_t index = 0; index < original.size(); ++index)
		{
			original_energy += original[index] * original[index];
			quadrature_energy += static_cast<double>(quadrature[index]) * quadrature[index];
			cross_energy += original[index] * quadrature[index];
		}
	}

	void ensure_analytic(Entry& entry, const SampleRegion& region)
	{
		if (!entry.quadrature_left.empty())
			return;
		const auto started = std::chrono::steady_clock::now();
		const auto left = read_channel(region, false);
		auto quadrature_left = padded_quadrature(left);
		if (has_valid_loop(region))
		{
			const std::vector<double> loop(left.begin() + region.loop_start,
				left.begin() + region.loop_end);
			install_periodic_body(quadrature_left, periodic_quadrature(loop),
				region.loop_start, region.sample_rate);
		}
		double original_energy_left = 0.0;
		double quadrature_energy_left = 0.0;
		double cross_energy_left = 0.0;
		compute_energy(left, quadrature_left, original_energy_left,
			quadrature_energy_left, cross_energy_left);
		std::vector<float> quadrature_right;
		double original_energy_right = 0.0;
		double quadrature_energy_right = 0.0;
		double cross_energy_right = 0.0;
		if (region.channels == 2)
		{
			const auto right = read_channel(region, true);
			quadrature_right = padded_quadrature(right);
			if (has_valid_loop(region))
			{
				const std::vector<double> loop(right.begin() + region.loop_start,
					right.begin() + region.loop_end);
				install_periodic_body(quadrature_right, periodic_quadrature(loop),
					region.loop_start, region.sample_rate);
			}
			compute_energy(right, quadrature_right, original_energy_right,
				quadrature_energy_right, cross_energy_right);
		}
		entry.quadrature_left = std::move(quadrature_left);
		entry.quadrature_right = std::move(quadrature_right);
		entry.original_energy_left = original_energy_left;
		entry.quadrature_energy_left = quadrature_energy_left;
		entry.cross_energy_left = cross_energy_left;
		entry.original_energy_right = original_energy_right;
		entry.quadrature_energy_right = quadrature_energy_right;
		entry.cross_energy_right = cross_energy_right;
		statistics.cache_bytes += entry.quadrature_left.size() * sizeof(float) +
			entry.quadrature_right.size() * sizeof(float);
		++statistics.analytic_samples;
		statistics.preprocessing_ms += std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started).count();
	}

	uint64_t variant_seed(const Entry& entry, uint32_t variant_index, uint64_t salt = 0) const noexcept
	{
		uint64_t seed = hash_combine(settings.seed, entry.key.logical_id);
		seed = hash_combine(seed, variant_index);
		seed = hash_combine(seed, static_cast<uint64_t>(settings.mode));
		return hash_combine(seed, salt);
	}

	Variant& ensure_variant(Entry& entry, const SampleRegion& region, uint32_t variant_index)
	{
		if (entry.variants[variant_index])
			return *entry.variants[variant_index];
		const auto started = std::chrono::steady_clock::now();
		auto variant = std::make_unique<Variant>();
		const auto left = read_channel(region, false);
		const auto delta = make_phase_delta(left.size(), region.sample_rate, settings.mode,
			settings.strength, settings.correlation_hz, variant_seed(entry, variant_index));
		variant->left = apply_phase_field(left, delta);
		if (region.channels == 2)
		{
			const auto right = read_channel(region, true);
			variant->right = apply_phase_field(right, delta);
		}
		if (has_valid_loop(region))
		{
			const size_t loop_size = region.loop_end - region.loop_start;
			const auto loop_delta = make_phase_delta(loop_size, region.sample_rate, settings.mode,
				settings.strength, settings.correlation_hz,
				variant_seed(entry, variant_index, 0x4c4f4f50ULL));
			const std::vector<double> left_loop(left.begin() + region.loop_start,
				left.begin() + region.loop_end);
			install_periodic_body(variant->left, apply_phase_field(left_loop, loop_delta),
				region.loop_start, region.sample_rate);
			if (region.channels == 2)
			{
				const auto right = read_channel(region, true);
				const std::vector<double> right_loop(right.begin() + region.loop_start,
					right.begin() + region.loop_end);
				install_periodic_body(variant->right, apply_phase_field(right_loop, loop_delta),
					region.loop_start, region.sample_rate);
			}
		}
		rms_match(variant->left, left);
		if (region.channels == 2)
			rms_match(variant->right, read_channel(region, true));
		statistics.cache_bytes += variant->left.size() * sizeof(float) +
			variant->right.size() * sizeof(float);
		++statistics.cached_variants;
		statistics.preprocessing_ms += std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started).count();
		entry.variants[variant_index] = std::move(variant);
		return *entry.variants[variant_index];
	}

	float analytic_scale(double cosine, double sine, double original_energy,
		double quadrature_energy, double cross_energy) const noexcept
	{
		const double changed_energy = cosine * cosine * original_energy +
			sine * sine * quadrature_energy - 2.0 * cosine * sine * cross_energy;
		return static_cast<float>(changed_energy > 1e-30 ?
			std::sqrt(original_energy / changed_energy) : 1.0);
	}

	uint64_t event_hash(uint64_t region_id, uint64_t serial, uint8_t channel,
		uint8_t note) const noexcept
	{
		uint64_t hash = hash_combine(settings.seed, serial);
		hash = hash_combine(hash, channel);
		hash = hash_combine(hash, note);
		return hash_combine(hash, region_id);
	}

	PhaseVoiceState make_state(const SampleRegion& region, uint64_t region_id,
		uint64_t serial, uint8_t channel, uint8_t note)
	{
		PhaseVoiceState state;
		if (settings.mode == PhaseMode::Coherent || settings.strength <= 0.0f)
			return state;
		++statistics.assignments;
		state.attack_hold_frames = static_cast<uint32_t>(std::clamp<int64_t>(std::llround(
			settings.preserve_attack_ms * 0.001 * region.sample_rate), 0, region.pcm_len));
		if (state.attack_hold_frames < region.pcm_len && settings.preserve_attack_ms > 0.0f)
			state.attack_fade_frames = (std::min)(region.pcm_len - state.attack_hold_frames,
				static_cast<uint32_t>(std::llround(region.sample_rate * 0.010)));
		const uint64_t identity = event_hash(region_id, serial, channel, note);
		if (settings.mode == PhaseMode::RandomPolarity)
		{
			state.kind = PhaseVoiceState::Kind::Polarity;
			state.polarity = (identity & 1) != 0 ? -1.0f : 1.0f;
			return state;
		}

		Entry& entry = entry_for(region, region_id);
		if (settings.mode == PhaseMode::Analytic)
		{
			ensure_analytic(entry, region);
			uint32_t variant_index = 0;
			uint64_t angle_hash = identity;
			if (!settings.continuous)
			{
				variant_index = static_cast<uint32_t>(identity % settings.pool_size);
				angle_hash = variant_seed(entry, variant_index, 0x414e474c45ULL);
				if (!entry.analytic_variants_seen[variant_index])
				{
					entry.analytic_variants_seen[variant_index] = true;
					++statistics.cached_variants;
				}
			}
			const double angle = (unit_from_hash(splitmix64(angle_hash)) * 2.0 - 1.0) *
				pi * settings.strength;
			state.kind = PhaseVoiceState::Kind::Analytic;
			state.quadrature_left = entry.quadrature_left.data();
			state.quadrature_right = region.channels == 2 ? entry.quadrature_right.data() :
				entry.quadrature_left.data();
			state.cosine = static_cast<float>(std::cos(angle));
			state.sine = static_cast<float>(std::sin(angle));
			state.scale_left = analytic_scale(state.cosine, state.sine,
				entry.original_energy_left, entry.quadrature_energy_left, entry.cross_energy_left);
			state.scale_right = region.channels == 2 ? analytic_scale(state.cosine, state.sine,
				entry.original_energy_right, entry.quadrature_energy_right, entry.cross_energy_right) :
				state.scale_left;
			return state;
		}

		const uint32_t variant_index = static_cast<uint32_t>(identity % settings.pool_size);
		Variant& variant = ensure_variant(entry, region, variant_index);
		state.kind = PhaseVoiceState::Kind::Variant;
		state.variant_left = variant.left.data();
		state.variant_right = region.channels == 2 ? variant.right.data() : variant.left.data();
		return state;
	}
};

PhaseProcessor::PhaseProcessor() : impl_(std::make_unique<Impl>()) {}
PhaseProcessor::~PhaseProcessor() = default;
PhaseProcessor::PhaseProcessor(PhaseProcessor&&) noexcept = default;
PhaseProcessor& PhaseProcessor::operator=(PhaseProcessor&&) noexcept = default;

void PhaseProcessor::configure(const PhaseSettings& requested) noexcept
{
	if (!impl_)
		return;
	PhaseSettings settings = requested;
	settings.strength = std::clamp(settings.strength, 0.0f, 1.0f);
	settings.pool_size = std::clamp(settings.pool_size, uint32_t{1}, uint32_t{64});
	settings.correlation_hz = (std::max)(settings.correlation_hz, 0.001f);
	settings.preserve_attack_ms = (std::max)(settings.preserve_attack_ms, 0.0f);
	if (settings.mode != PhaseMode::Analytic)
		settings.continuous = false;
	if (!same_settings(impl_->settings, settings))
	{
		impl_->settings = settings;
		impl_->reset_cache();
	}
}

const PhaseSettings& PhaseProcessor::settings() const noexcept
{
	static const PhaseSettings defaults;
	return impl_ ? impl_->settings : defaults;
}

void PhaseProcessor::clear() noexcept
{
	if (impl_)
		impl_->reset_cache();
}

PhaseVoiceState PhaseProcessor::assign(const SampleRegion& region, uint64_t region_id,
	uint64_t event_serial, uint8_t channel, uint8_t note) noexcept
{
	if (!impl_)
		return {};
	try
	{
		return impl_->make_state(region, region_id, event_serial, channel, note);
	}
	catch (...)
	{
		++impl_->statistics.failures;
		return {};
	}
}

PhaseCacheStats PhaseProcessor::stats() const noexcept
{
	return impl_ ? impl_->statistics : PhaseCacheStats{};
}

} // namespace safsyn
