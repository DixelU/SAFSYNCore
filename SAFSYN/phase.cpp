#include "phase.h"

#include "core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace safsyn
{
namespace
{
using Complex = std::complex<double>;
constexpr double pi = 3.1415926535897932384626433832795;

struct PreparationCancelled {};
void check_cancel(const std::stop_token& stop, size_t iteration = 0)
{
	if ((iteration & 16383) == 0 && stop.stop_requested()) throw PreparationCancelled{};
}

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

void radix2_fft(std::vector<Complex>& values, bool inverse, std::stop_token stop)
{
	check_cancel(stop);
	const size_t n = values.size();
	for (size_t i = 1, j = 0; i < n; ++i)
	{
		check_cancel(stop, i);
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j)
			std::swap(values[i], values[j]);
	}
	// Every group of a stage uses the same twiddle recurrence, so it is built
	// once per stage. Repeated multiplication (not per-index cos/sin) is the
	// reference arithmetic and keeps cached quadratures bit-identical.
	auto stage_twiddles = [inverse](Complex* table, size_t length) {
		const double angle = (inverse ? 2.0 : -2.0) * pi / static_cast<double>(length);
		const Complex step(std::cos(angle), std::sin(angle));
		Complex factor(1.0, 0.0);
		for (size_t offset = 0; offset < length / 2; ++offset)
		{
			table[offset] = factor;
			factor *= step;
		}
	};
	auto butterflies = [&](size_t start, size_t length, const Complex* twiddles) {
		const size_t half = length / 2;
		Complex* first = values.data() + start;
		Complex* second = first + half;
		for (size_t offset = 0; offset < half; ++offset)
		{
			check_cancel(stop, start + offset);
			const Complex even = first[offset];
			const Complex odd = second[offset] * twiddles[offset];
			first[offset] = even + odd;
			second[offset] = even - odd;
		}
	};
	// Stages that fit in a cache-sized block run depth-first per block; each
	// butterfly still sees the same inputs, so results match stage order.
	constexpr size_t cache_block = size_t{1} << 14;
	const size_t block = (std::min)(n, cache_block);
	std::vector<Complex> block_twiddles(block > 1 ? block - 1 : 0);
	for (size_t length = 2; length <= block; length <<= 1)
		stage_twiddles(block_twiddles.data() + length / 2 - 1, length);
	for (size_t block_start = 0; block_start < n; block_start += block)
		for (size_t length = 2; length <= block; length <<= 1)
			for (size_t start = block_start; start < block_start + block; start += length)
				butterflies(start, length, block_twiddles.data() + length / 2 - 1);
	if (block < n)
	{
		std::vector<Complex> twiddles(n / 2);
		for (size_t length = block * 2; length <= n; length <<= 1)
		{
			stage_twiddles(twiddles.data(), length);
			for (size_t start = 0; start < n; start += length)
				butterflies(start, length, twiddles.data());
			if (length == n)
				break;
		}
	}
	if (inverse)
		for (Complex& value : values)
			value /= static_cast<double>(n);
}

// Bluestein chirp and transformed chirp filter for one non-power-of-two length.
// Loop bodies reuse one plan for the forward and inverse transform of every
// channel; the values equal those of a per-call construction.
struct BluesteinPlan
{
	std::vector<Complex> chirp; // (cos, sin) of pi * (i^2 mod 2n) / n
	std::vector<Complex> filter;

	BluesteinPlan(size_t n, std::stop_token stop)
	{
		if (n == 0 || is_power_of_two(n))
			return;
		const size_t convolution_size = next_power_of_two(n * 2 - 1);
		chirp.resize(n);
		filter.resize(convolution_size);
		for (size_t index = 0; index < n; ++index)
		{
			check_cancel(stop, index);
			const double i = static_cast<double>(index);
			const double angle = pi * std::fmod(i * i, 2.0 * static_cast<double>(n)) /
				static_cast<double>(n);
			chirp[index] = Complex(std::cos(angle), std::sin(angle));
			filter[index] = chirp[index];
			if (index != 0)
				filter[convolution_size - index] = chirp[index];
		}
		radix2_fft(filter, false, stop);
	}
};

std::vector<Complex> forward_fft_any(const std::vector<Complex>& input, const BluesteinPlan& plan,
	std::stop_token stop)
{
	check_cancel(stop);
	const size_t n = input.size();
	if (n == 0)
		return {};
	if (is_power_of_two(n))
	{
		auto result = input;
		radix2_fft(result, false, stop);
		return result;
	}

	std::vector<Complex> a(plan.filter.size());
	for (size_t index = 0; index < n; ++index)
		a[index] = input[index] * std::conj(plan.chirp[index]);
	radix2_fft(a, false, stop);
	for (size_t index = 0; index < a.size(); ++index)
		a[index] *= plan.filter[index];
	radix2_fft(a, true, stop);
	a.resize(n);
	for (size_t index = 0; index < n; ++index)
		a[index] *= std::conj(plan.chirp[index]);
	return a;
}

std::vector<Complex> inverse_fft_any(const std::vector<Complex>& input, const BluesteinPlan& plan,
	std::stop_token stop)
{
	check_cancel(stop);
	std::vector<Complex> conjugated(input.size());
	for (size_t index = 0; index < input.size(); ++index)
		conjugated[index] = std::conj(input[index]);
	auto result = forward_fft_any(conjugated, plan, stop);
	const double scale = input.empty() ? 1.0 : static_cast<double>(input.size());
	for (Complex& value : result)
		value = std::conj(value) / scale;
	return result;
}

std::vector<float> periodic_quadrature(const std::vector<double>& input, const BluesteinPlan& plan,
	std::stop_token stop)
{
	std::vector<Complex> complex_input(input.size());
	for (size_t index = 0; index < input.size(); ++index)
		complex_input[index] = Complex(input[index], 0.0);
	auto spectrum = forward_fft_any(complex_input, plan, stop);
	const size_t n = spectrum.size();
	for (size_t bin = 1; bin < (n + 1) / 2; ++bin)
		spectrum[bin] *= 2.0;
	for (size_t bin = n / 2 + 1; bin < n; ++bin)
		spectrum[bin] = Complex{};
	if (n % 2 != 0)
		for (size_t bin = (n + 1) / 2; bin < n; ++bin)
			spectrum[bin] = Complex{};
	auto analytic = inverse_fft_any(spectrum, plan, stop);
	std::vector<float> result(n);
	for (size_t index = 0; index < n; ++index)
		result[index] = static_cast<float>(analytic[index].imag());
	return result;
}

std::vector<float> padded_quadrature(const std::vector<double>& input, std::stop_token stop)
{
	check_cancel(stop);
	if (input.empty())
		return {};
	const size_t padded_size = next_power_of_two(input.size() * 2);
	std::vector<Complex> analytic(padded_size);
	for (size_t index = 0; index < input.size(); ++index)
		analytic[index] = Complex(input[index], 0.0);
	radix2_fft(analytic, false, stop);
	for (size_t bin = 1; bin < padded_size / 2; ++bin)
		analytic[bin] *= 2.0;
	for (size_t bin = padded_size / 2 + 1; bin < padded_size; ++bin)
		analytic[bin] = Complex{};
	radix2_fft(analytic, true, stop);
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
		left.continuous == right.continuous &&
		left.preserve_attack_ms == right.preserve_attack_ms;
}

struct ChannelQuadrature
{
	std::vector<float> quadrature;
	double original_energy = 0.0;
	double quadrature_energy = 0.0;
	double cross_energy = 0.0;
};

struct SampleQuadrature
{
	ChannelQuadrature left;
	ChannelQuadrature right;
};

// Pure function of the region's PCM and loop, safe to run on any thread.
ChannelQuadrature build_channel_quadrature(const SampleRegion& region, bool right,
	const BluesteinPlan& loop_plan, std::stop_token stop)
{
	ChannelQuadrature result;
	const auto original = read_channel(region, right);
	result.quadrature = padded_quadrature(original, stop);
	if (has_valid_loop(region))
	{
		const std::vector<double> loop(original.begin() + region.loop_start,
			original.begin() + region.loop_end);
		install_periodic_body(result.quadrature, periodic_quadrature(loop, loop_plan, stop),
			region.loop_start, region.sample_rate);
	}
	for (size_t index = 0; index < original.size(); ++index)
	{
		result.original_energy += original[index] * original[index];
		result.quadrature_energy += static_cast<double>(result.quadrature[index]) *
			result.quadrature[index];
		result.cross_energy += original[index] * result.quadrature[index];
	}
	return result;
}

SampleQuadrature build_sample_quadrature(const SampleRegion& region, std::stop_token stop)
{
	check_cancel(stop);
	const BluesteinPlan loop_plan(has_valid_loop(region) ?
		region.loop_end - region.loop_start : 0, stop);
	SampleQuadrature result;
	result.left = build_channel_quadrature(region, false, loop_plan, stop);
	if (region.channels == 2)
		result.right = build_channel_quadrature(region, true, loop_plan, stop);
	return result;
}

size_t automatic_preparation_threads() noexcept
{
	const unsigned hardware = std::thread::hardware_concurrency();
	return std::clamp<size_t>(hardware > 1 ? hardware - 1 : 1, 1, 8);
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
		std::vector<bool> analytic_variants_seen;

		bool prepared() const noexcept { return !quadrature_left.empty(); }
	};

	PhaseSettings settings;
	PhaseCacheStats statistics;
	std::unordered_map<SampleKey, std::unique_ptr<Entry>, SampleKeyHash> entries;

	void reset_cache() noexcept
	{
		entries.clear();
		statistics = {};
	}

	bool analytic() const noexcept
	{
		return settings.mode == PhaseMode::Analytic && settings.strength > 0.0f;
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
		entry->analytic_variants_seen.resize(settings.pool_size, false);
		Entry* result = entry.get();
		entries.emplace(key, std::move(entry));
		++statistics.cached_samples;
		return *result;
	}

	void publish(Entry& entry, SampleQuadrature&& quadrature) noexcept
	{
		entry.quadrature_left = std::move(quadrature.left.quadrature);
		entry.quadrature_right = std::move(quadrature.right.quadrature);
		entry.original_energy_left = quadrature.left.original_energy;
		entry.quadrature_energy_left = quadrature.left.quadrature_energy;
		entry.cross_energy_left = quadrature.left.cross_energy;
		entry.original_energy_right = quadrature.right.original_energy;
		entry.quadrature_energy_right = quadrature.right.quadrature_energy;
		entry.cross_energy_right = quadrature.right.cross_energy;
		statistics.cache_bytes += entry.quadrature_left.size() * sizeof(float) +
			entry.quadrature_right.size() * sizeof(float);
		++statistics.analytic_samples;
	}

	void ensure_analytic(Entry& entry, const SampleRegion& region)
	{
		if (entry.prepared())
			return;
		const auto started = std::chrono::steady_clock::now();
		publish(entry, build_sample_quadrature(region, {}));
		statistics.preprocessing_ms += std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started).count();
	}

	float analytic_scale(double cosine, double sine, double original_energy,
		double quadrature_energy, double cross_energy) const noexcept
	{
		const double changed_energy = cosine * cosine * original_energy +
			sine * sine * quadrature_energy - 2.0 * cosine * sine * cross_energy;
		return static_cast<float>(changed_energy > 1e-30 ?
			std::sqrt(original_energy / changed_energy) : 1.0);
	}

	// Analytic identity is onset/channel/key, shared across sample layers.
	uint64_t event_hash(uint64_t serial, uint8_t channel, uint8_t note) const noexcept
	{
		uint64_t hash = hash_combine(settings.seed, serial);
		hash = hash_combine(hash, channel);
		hash = hash_combine(hash, note);
		return hash_combine(hash, uint64_t{0}); // Retained layer slot keeps seeded angles stable.
	}

	PhaseVoiceState make_state(const SampleRegion& region, uint64_t region_id,
		uint64_t serial, uint8_t channel, uint8_t note, bool record_assignment)
	{
		PhaseVoiceState state;
		if (!analytic())
			return state;
		if (record_assignment)
			++statistics.assignments;
		state.attack_hold_frames = static_cast<uint32_t>(std::clamp<int64_t>(std::llround(
			settings.preserve_attack_ms * 0.001 * region.sample_rate), 0, region.pcm_len));
		if (state.attack_hold_frames < region.pcm_len && settings.preserve_attack_ms > 0.0f)
			state.attack_fade_frames = (std::min)(region.pcm_len - state.attack_hold_frames,
				static_cast<uint32_t>(std::llround(region.sample_rate * 0.010)));
		const uint64_t identity = event_hash(serial, channel, note);

		Entry& entry = entry_for(region, region_id);
		ensure_analytic(entry, region);
		uint64_t angle_hash = identity;
		if (!settings.continuous)
		{
			const uint32_t variant_index = static_cast<uint32_t>(identity % settings.pool_size);
			angle_hash = hash_combine(hash_combine(settings.seed, variant_index), 0x414e474c45ULL);
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

	struct Job
	{
		Entry* entry = nullptr;
		const SampleRegion* region = nullptr;
	};

	// Transforms are independent per sample, so worker scheduling cannot change
	// cached values. Progress callbacks stay on the calling thread.
	void prepare_parallel(const std::vector<Job>& jobs, size_t threads,
		PhasePreparationProgress& progress, const PhasePreparationOptions& options)
	{
		std::mutex mutex;
		std::condition_variable changed;
		std::stop_source internal;
		std::stop_callback forward(options.stop, [&internal] { internal.request_stop(); });
		const std::stop_token stop = internal.get_token();
		size_t next = 0, finished = 0, running = 0;
		std::exception_ptr failure;
		auto work = [&] {
			for (;;)
			{
				size_t job = 0;
				{
					std::lock_guard lock(mutex);
					if (failure || stop.stop_requested() || next == jobs.size())
						break;
					job = next++;
				}
				try
				{
					auto quadrature = build_sample_quadrature(*jobs[job].region, stop);
					std::lock_guard lock(mutex);
					publish(*jobs[job].entry, std::move(quadrature));
					++finished;
				}
				catch (const PreparationCancelled&)
				{
					break;
				}
				catch (...)
				{
					std::lock_guard lock(mutex);
					if (!failure)
						failure = std::current_exception();
					internal.request_stop();
					break;
				}
				changed.notify_one();
			}
			{
				std::lock_guard lock(mutex);
				--running;
			}
			changed.notify_one();
		};

		std::vector<std::jthread> workers;
		// Workers must be stopped and joined before the locals they reference go
		// away, including when a progress callback throws.
		struct JoinGuard
		{
			std::stop_source& stop;
			std::vector<std::jthread>& workers;
			~JoinGuard() { stop.request_stop(); workers.clear(); }
		} join_guard{internal, workers};
		workers.reserve(threads);
		for (size_t index = 0; index < threads; ++index)
		{
			{
				std::lock_guard lock(mutex);
				++running;
			}
			try
			{
				workers.emplace_back(work);
			}
			catch (...)
			{
				std::lock_guard lock(mutex);
				--running;
				if (workers.empty())
					throw;
				break;
			}
		}

		size_t reported = 0;
		std::unique_lock lock(mutex);
		for (;;)
		{
			changed.wait(lock, [&] { return reported != finished || running == 0; });
			while (reported != finished)
			{
				++reported;
				++progress.completed;
				progress.cache_bytes = statistics.cache_bytes;
				// Samples finishing after a cancellation are kept but not reported.
				if (options.progress && !stop.stop_requested())
				{
					lock.unlock();
					options.progress(progress);
					lock.lock();
				}
			}
			if (running == 0)
				break;
		}
		lock.unlock();
		workers.clear();
		if (failure)
			std::rethrow_exception(failure);
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
	if (settings.mode != PhaseMode::Analytic)
		settings.mode = PhaseMode::Coherent;
	settings.strength = std::clamp(settings.strength, 0.0f, 1.0f);
	settings.pool_size = std::clamp(settings.pool_size, uint32_t{1}, uint32_t{64});
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

bool PhaseProcessor::prepare(std::span<const SampleRegion> regions, const PhasePreparationOptions& options)
{
	try
	{
		check_cancel(options.stop);
		if (!impl_) throw std::logic_error("phase processor has no state");
		auto& state = *impl_;
		PhasePreparationProgress progress;
		progress.cache_bytes = progress.total_cache_bytes = state.statistics.cache_bytes;
		std::unordered_set<Impl::SampleKey, Impl::SampleKeyHash> seen;
		std::vector<size_t> unique_regions;
		if (state.analytic())
			for (size_t id = 0; id < regions.size(); ++id)
			{
				check_cancel(options.stop);
				const auto& region = regions[id];
				if (!region.pcm || region.pcm_len == 0 || region.channels < 1 || region.channels > 2) continue;
				const auto key = state.make_key(region, id);
				if (!seen.insert(key).second) continue;
				unique_regions.push_back(id);
				++progress.total;
				if (const auto found = state.entries.find(key);
					found != state.entries.end() && found->second->prepared())
					continue;
				const uint64_t bytes = uint64_t{region.pcm_len} * region.channels * sizeof(float);
				if (bytes > UINT64_MAX - progress.total_cache_bytes) throw std::length_error("phase cache size overflow");
				progress.total_cache_bytes += bytes;
			}
		if (options.progress) options.progress(progress);
		check_cancel(options.stop);
		// Refuse an oversized cache before allocating any transformed samples.
		if (progress.total_cache_bytes > options.maximum_cache_bytes)
			throw std::length_error("Phase cache needs " +
				std::to_string(progress.total_cache_bytes / 1048576 + (progress.total_cache_bytes % 1048576 != 0)) +
				" MiB; the limit is " + std::to_string(options.maximum_cache_bytes / 1048576) +
				" MiB. Choose Direct sampling mode or raise the cache limit. FFT working memory is extra.");

		std::vector<Impl::Job> jobs;
		for (const size_t id : unique_regions)
		{
			auto& entry = state.entry_for(regions[id], id);
			if (!entry.prepared())
			{
				jobs.push_back({&entry, &regions[id]});
				continue;
			}
			++progress.completed;
			progress.cache_bytes = state.statistics.cache_bytes;
			if (options.progress) options.progress(progress);
		}
		if (jobs.empty())
			return !options.stop.stop_requested();

		// Longest samples first balances workers; the cache is order-independent.
		std::stable_sort(jobs.begin(), jobs.end(), [](const Impl::Job& left, const Impl::Job& right) {
			return uint64_t{left.region->pcm_len} * left.region->channels >
				uint64_t{right.region->pcm_len} * right.region->channels;
		});
		const size_t threads = (std::min)(jobs.size(),
			options.threads != 0 ? size_t{options.threads} : automatic_preparation_threads());
		const auto started = std::chrono::steady_clock::now();
		auto record_time = [&] {
			state.statistics.preprocessing_ms += std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - started).count();
		};
		try
		{
			if (threads <= 1)
				for (const auto& job : jobs)
				{
					check_cancel(options.stop);
					state.publish(*job.entry, build_sample_quadrature(*job.region, options.stop));
					++progress.completed;
					progress.cache_bytes = state.statistics.cache_bytes;
					if (options.progress) options.progress(progress);
				}
			else
				state.prepare_parallel(jobs, threads, progress, options);
		}
		catch (...)
		{
			record_time();
			throw;
		}
		record_time();
		check_cancel(options.stop);
		return true;
	}
	catch (const PreparationCancelled&) { return false; }
}

PhaseVoiceState PhaseProcessor::assign(const SampleRegion& region, uint64_t region_id,
	uint64_t event_serial, uint8_t channel, uint8_t note) noexcept
{
	if (!impl_)
		return {};
	try
	{
		return impl_->make_state(region, region_id, event_serial, channel, note, true);
	}
	catch (...)
	{
		++impl_->statistics.failures;
		return {};
	}
}

PhaseVoiceState PhaseProcessor::reconstruct(const SampleRegion& region, uint64_t region_id,
	uint64_t event_serial, uint8_t channel, uint8_t note) noexcept
{
	if (!impl_)
		return {};
	try
	{
		return impl_->make_state(region, region_id, event_serial, channel, note, false);
	}
	catch (...)
	{
		return {};
	}
}

PhaseCacheStats PhaseProcessor::stats() const noexcept
{
	return impl_ ? impl_->statistics : PhaseCacheStats{};
}

} // namespace safsyn
