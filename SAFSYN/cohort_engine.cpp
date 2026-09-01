#include "cohort_engine.h"

#include "core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_M_X64) || defined(__SSE2__)
#include <immintrin.h>
#endif

namespace safsyn
{
namespace
{
constexpr uint32_t invalid_index = (std::numeric_limits<uint32_t>::max)();

float pcm_to_float(int16_t sample) noexcept
{
	return static_cast<float>(sample) / 32768.0f;
}

void add_mix_buffer(float* destination, const float* source, size_t samples) noexcept
{
	size_t index = 0;
#if defined(_M_X64) || defined(__SSE2__)
	for (; index + 4 <= samples; index += 4)
	{
		const __m128 accumulated = _mm_loadu_ps(destination + index);
		const __m128 contribution = _mm_loadu_ps(source + index);
		_mm_storeu_ps(destination + index, _mm_add_ps(accumulated, contribution));
	}
#endif
	for (; index < samples; ++index)
		destination[index] += source[index];
}

struct StableHandle
{
	uint32_t index = invalid_index;
	uint32_t generation = 0;

	bool valid() const noexcept { return index != invalid_index; }
};

struct OnsetKey
{
	const SampleRegion* region = nullptr;
	uint8_t channel = 0;
	uint8_t note = 0;
	uint8_t velocity = 0;

	bool operator==(const OnsetKey&) const noexcept = default;
};

struct OnsetKeyHash
{
	size_t operator()(const OnsetKey& key) const noexcept
	{
		size_t hash = reinterpret_cast<size_t>(key.region);
		hash ^= static_cast<size_t>(key.channel) << 3;
		hash ^= static_cast<size_t>(key.note) << 11;
		hash ^= static_cast<size_t>(key.velocity) << 19;
		return hash;
	}
};

struct VariantTerm
{
	const float* left = nullptr;
	const float* right = nullptr;
	uint64_t count = 0;
};

struct PhaseAggregate
{
	uint64_t multiplicity = 0;
	uint64_t coherent_count = 0;
	uint64_t transformed_count = 0;
	double polarity_sum = 0.0;
	double cosine_left = 0.0;
	double sine_left = 0.0;
	double cosine_right = 0.0;
	double sine_right = 0.0;
	const float* quadrature_left = nullptr;
	const float* quadrature_right = nullptr;
	uint32_t attack_hold_frames = 0;
	uint32_t attack_fade_frames = 0;
	std::vector<VariantTerm> variants;
	PhaseVoiceState singleton;
	bool singleton_valid = false;

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
		switch (state.kind)
		{
		case PhaseVoiceState::Kind::Coherent:
			break;
		case PhaseVoiceState::Kind::Polarity:
			polarity_sum += static_cast<double>(state.polarity) * count;
			break;
		case PhaseVoiceState::Kind::Analytic:
			quadrature_left = state.quadrature_left;
			quadrature_right = state.quadrature_right;
			cosine_left += static_cast<double>(state.cosine) * state.scale_left * count;
			sine_left += static_cast<double>(state.sine) * state.scale_left * count;
			cosine_right += static_cast<double>(state.cosine) * state.scale_right * count;
			sine_right += static_cast<double>(state.sine) * state.scale_right * count;
			break;
		case PhaseVoiceState::Kind::Variant:
		{
			auto found = std::find_if(variants.begin(), variants.end(), [&](const VariantTerm& term) {
				return term.left == state.variant_left && term.right == state.variant_right;
			});
			if (found == variants.end())
				variants.push_back({state.variant_left, state.variant_right, count});
			else
				found->count += count;
			break;
		}
		}
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
		polarity_sum += other.polarity_sum;
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
		for (const auto& term : other.variants)
		{
			auto found = std::find_if(variants.begin(), variants.end(), [&](const VariantTerm& own) {
				return own.left == term.left && own.right == term.right;
			});
			if (found == variants.end())
				variants.push_back(term);
			else
				found->count += term.count;
		}
	}

	void subtract(const PhaseAggregate& other) noexcept
	{
		multiplicity -= other.multiplicity;
		coherent_count -= other.coherent_count;
		transformed_count -= other.transformed_count;
		polarity_sum -= other.polarity_sum;
		cosine_left -= other.cosine_left;
		sine_left -= other.sine_left;
		cosine_right -= other.cosine_right;
		sine_right -= other.sine_right;
		for (const auto& term : other.variants)
		{
			auto found = std::find_if(variants.begin(), variants.end(), [&](const VariantTerm& own) {
				return own.left == term.left && own.right == term.right;
			});
			if (found != variants.end())
				found->count -= term.count;
		}
		if (multiplicity != 1)
			singleton_valid = false;
	}

	float sample(float original, uint32_t index, bool right) const noexcept
	{
		if (multiplicity == 1 && singleton_valid)
			return singleton.apply(original, index, right);
		double changed = 0.0;
		changed += static_cast<double>(original) * polarity_sum;
		if (quadrature_left)
		{
			const float* quadrature = right ? quadrature_right : quadrature_left;
			const double cosine = right ? cosine_right : cosine_left;
			const double sine = right ? sine_right : sine_left;
			changed += static_cast<double>(original) * cosine -
				static_cast<double>(quadrature[index]) * sine;
		}
		for (const auto& term : variants)
		{
			if (term.count == 0)
				continue;
			const float* values = right ? term.right : term.left;
			changed += static_cast<double>(values[index]) * term.count;
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
	uint32_t hold_samples_left = 0;
	float gain_l = 0.0f;
	float gain_r = 0.0f;
	uint64_t birth_frame = 0;
	uint64_t release_frame = 0;
	uint64_t oldest_serial = 0;
	StableHandle release_target;
	uint64_t release_target_frame = 0;
	PhaseAggregate phase;
};

struct CohortSlot
{
	RenderCohort cohort;
	uint32_t generation = 1;
	uint32_t channel_previous = invalid_index;
	uint32_t channel_next = invalid_index;
	uint32_t note_previous = invalid_index;
	uint32_t note_next = invalid_index;
	bool occupied = false;
};

struct LogicalRegionLink
{
	StableHandle cohort;
	size_t region_id = 0;
};

struct LogicalBatch
{
	uint8_t channel = 0;
	uint8_t note = 0;
	uint8_t velocity = 0;
	uint64_t first_serial = 0;
	uint64_t held = 0;
	uint64_t sustained = 0;
	uint64_t onset_frame = 0;
	bool one_shot_blocking = false;
	bool one_shot_release_applied = false;
	bool one_shot_release_pending = false;
	std::vector<LogicalRegionLink> links;
};

struct LogicalSlot
{
	LogicalBatch batch;
	uint32_t generation = 1;
	bool occupied = false;
};
}

struct CohortEngineState
{
	struct RenderScratch
	{
		std::vector<float> audio;
		std::vector<uint32_t> retired;
	};

	struct ParallelRenderState
	{
		std::mutex mutex;
		std::condition_variable job_ready;
		std::condition_variable job_finished;
		std::vector<std::jthread> workers;
		std::vector<RenderScratch> scratch;
		SynthEngine* owner = nullptr;
		uint32_t frames = 0;
		size_t slot_count = 0;
		uint64_t epoch = 0;
		size_t completed = 0;
		bool stopping = false;

		~ParallelRenderState()
		{
			{
				const std::lock_guard lock(mutex);
				stopping = true;
				++epoch;
			}
			job_ready.notify_all();
			workers.clear();
		}
	};

	std::vector<CohortSlot> cohorts;
	std::vector<uint32_t> free_cohorts;
	std::vector<LogicalSlot> logical_batches;
	std::vector<uint32_t> free_logical_batches;
	std::array<std::vector<StableHandle>, 16 * 128> note_stacks;
	std::unordered_map<OnsetKey, StableHandle, OnsetKeyHash> onset_candidates;
	uint64_t active_logical = 0;
	size_t active_cohort_count = 0;
	std::array<size_t, 16> active_cohorts_by_channel{};
	std::array<uint32_t, 16> channel_heads{};
	std::array<uint32_t, 16> steal_cursors{};
	std::array<uint32_t, 16 * 128> note_heads{};
	size_t maximum_cohorts = 0;
	std::unique_ptr<ParallelRenderState> parallel_render;
	std::vector<std::pair<size_t, const SampleRegion*>> matching_regions_scratch;

	CohortEngineState() noexcept
	{
		channel_heads.fill(invalid_index);
		steal_cursors.fill(invalid_index);
		note_heads.fill(invalid_index);
	}

	static size_t note_index(uint8_t channel, uint8_t note) noexcept
	{
		return static_cast<size_t>(channel) * 128 + note;
	}

	CohortSlot* resolve(StableHandle handle) noexcept
	{
		if (!handle.valid() || handle.index >= cohorts.size())
			return nullptr;
		auto& slot = cohorts[handle.index];
		return slot.occupied && slot.generation == handle.generation ? &slot : nullptr;
	}

	LogicalSlot* resolve_logical(StableHandle handle) noexcept
	{
		if (!handle.valid() || handle.index >= logical_batches.size())
			return nullptr;
		auto& slot = logical_batches[handle.index];
		return slot.occupied && slot.generation == handle.generation ? &slot : nullptr;
	}

	void update_peaks(SynthEngine& owner) noexcept
	{
		const size_t logical = static_cast<size_t>((std::min)(active_logical,
			static_cast<uint64_t>((std::numeric_limits<size_t>::max)())));
		owner.stats_.peak_active_logical_voices = (std::max)(
			owner.stats_.peak_active_logical_voices, logical);
		owner.stats_.peak_active_voices = (std::max)(owner.stats_.peak_active_voices, logical);
		owner.stats_.peak_active_cohorts = (std::max)(
			owner.stats_.peak_active_cohorts, active_cohort_count);
	}

	void update_average_multiplicity(SynthEngine& owner) noexcept
	{
		const uint64_t onset_cohorts = owner.stats_.logical_voices_started -
			owner.stats_.logical_voices_merged;
		owner.stats_.average_cohort_multiplicity = onset_cohorts == 0 ? 0.0 :
			static_cast<double>(owner.stats_.logical_voices_started) / onset_cohorts;
	}

	void link_cohort(uint32_t index) noexcept
	{
		auto& slot = cohorts[index];
		const uint8_t channel = slot.cohort.channel;
		slot.channel_previous = invalid_index;
		slot.channel_next = channel_heads[channel];
		if (slot.channel_next != invalid_index)
			cohorts[slot.channel_next].channel_previous = index;
		channel_heads[channel] = index;
		++active_cohorts_by_channel[channel];

		const size_t key = note_index(channel, slot.cohort.note);
		slot.note_previous = invalid_index;
		slot.note_next = note_heads[key];
		if (slot.note_next != invalid_index)
			cohorts[slot.note_next].note_previous = index;
		note_heads[key] = index;
	}

	void unlink_cohort(uint32_t index) noexcept
	{
		auto& slot = cohorts[index];
		const uint8_t channel = slot.cohort.channel;
		if (slot.channel_previous == invalid_index)
			channel_heads[channel] = slot.channel_next;
		else
			cohorts[slot.channel_previous].channel_next = slot.channel_next;
		if (slot.channel_next != invalid_index)
			cohorts[slot.channel_next].channel_previous = slot.channel_previous;
		if (steal_cursors[channel] == index)
			steal_cursors[channel] = slot.channel_next != invalid_index
				? slot.channel_next : channel_heads[channel];
		--active_cohorts_by_channel[channel];

		const size_t key = note_index(channel, slot.cohort.note);
		if (slot.note_previous == invalid_index)
			note_heads[key] = slot.note_next;
		else
			cohorts[slot.note_previous].note_next = slot.note_next;
		if (slot.note_next != invalid_index)
			cohorts[slot.note_next].note_previous = slot.note_previous;
		slot.channel_previous = invalid_index;
		slot.channel_next = invalid_index;
		slot.note_previous = invalid_index;
		slot.note_next = invalid_index;
	}

	void retire_cohort(SynthEngine& owner, uint32_t index, bool logical_ends) noexcept
	{
		auto& slot = cohorts[index];
		if (!slot.occupied)
			return;
		if (logical_ends)
			active_logical -= slot.cohort.phase.multiplicity;
		unlink_cohort(index);
		slot.occupied = false;
		slot.cohort = {};
		free_cohorts.push_back(index);
		--active_cohort_count;
		(void)owner;
	}

	struct StealCandidate
	{
		uint32_t index = invalid_index;
		enum class Scope : uint8_t { Channel, Reserve, Global } scope = Scope::Global;
	};

	StealCandidate steal_candidate(SynthEngine& owner, uint8_t request_channel,
		uint8_t request_note) noexcept
	{
		auto estimated_level = [&](const RenderCohort& cohort) {
			const auto& channel = owner.channels_[cohort.channel];
			const double gain = (std::max)(std::abs(cohort.gain_l), std::abs(cohort.gain_r));
			return static_cast<double>(cohort.env) * channel.volume * channel.expression *
				owner.master_volume_ * gain * cohort.phase.multiplicity;
		};
		auto prefer = [&](uint32_t selected, uint32_t challenger) {
			if (selected == invalid_index)
				return true;
			const auto& current = cohorts[challenger].cohort;
			const auto& candidate = cohorts[selected].cohort;
			const bool current_releasing = current.stage == CohortStage::Release;
			const bool candidate_releasing = candidate.stage == CohortStage::Release;
			const double current_level = estimated_level(current);
			const double candidate_level = estimated_level(candidate);
			return (current_releasing && !candidate_releasing) ||
				(current_releasing == candidate_releasing &&
					(current_level < candidate_level ||
						(current_level == candidate_level &&
							current.oldest_serial > candidate.oldest_serial)));
		};
		const size_t channel_reserve = maximum_cohorts / active_cohorts_by_channel.size();
		uint8_t victim_channel = request_channel;
		auto scope = StealCandidate::Scope::Global;
		if (active_cohorts_by_channel[request_channel] != 0 &&
			active_cohorts_by_channel[request_channel] >= channel_reserve)
			scope = StealCandidate::Scope::Channel;
		else if (active_cohorts_by_channel[request_channel] < channel_reserve)
		{
			size_t largest_excess = 0;
			for (uint8_t channel = 0; channel < active_cohorts_by_channel.size(); ++channel)
			{
				const size_t count = active_cohorts_by_channel[channel];
				const size_t excess = count > channel_reserve ? count - channel_reserve : 0;
				if (excess > largest_excess)
				{
					largest_excess = excess;
					victim_channel = channel;
				}
			}
			if (largest_excess != 0)
				scope = StealCandidate::Scope::Reserve;
		}

		if (scope == StealCandidate::Scope::Global)
		{
			size_t largest_count = 0;
			for (uint8_t channel = 0; channel < active_cohorts_by_channel.size(); ++channel)
				if (active_cohorts_by_channel[channel] > largest_count)
				{
					largest_count = active_cohorts_by_channel[channel];
					victim_channel = channel;
				}
		}

		const uint32_t head = channel_heads[victim_channel];
		if (head == invalid_index)
			return {};
		constexpr size_t probe_limit = 32;
		const size_t probe_count = (std::min)(probe_limit,
			active_cohorts_by_channel[victim_channel]);
		uint32_t index = steal_cursors[victim_channel];
		if (index == invalid_index || !cohorts[index].occupied ||
			cohorts[index].cohort.channel != victim_channel)
			index = head;
		uint32_t best = invalid_index;
		uint32_t best_different_key = invalid_index;
		for (size_t offset = 0; offset < probe_count; ++offset)
		{
			if (prefer(best, index))
				best = index;
			const auto& cohort = cohorts[index].cohort;
			const bool same_key = cohort.channel == request_channel &&
				cohort.note == request_note;
			if (!same_key && prefer(best_different_key, index))
				best_different_key = index;
			index = cohorts[index].channel_next != invalid_index
				? cohorts[index].channel_next : head;
		}
		steal_cursors[victim_channel] = index;
		owner.stats_.steal_candidate_visits += probe_count;
		owner.stats_.maximum_steal_probe = (std::max)(owner.stats_.maximum_steal_probe,
			static_cast<uint32_t>(probe_count));

		if (best_different_key == invalid_index && victim_channel == request_channel)
		{
			for (uint16_t distance = 1; distance < 128; ++distance)
			{
				const uint8_t note = static_cast<uint8_t>((request_note + distance) & 0x7f);
				const uint32_t representative = note_heads[note_index(victim_channel, note)];
				if (representative != invalid_index)
				{
					best_different_key = representative;
					++owner.stats_.steal_candidate_visits;
					owner.stats_.maximum_steal_probe = (std::max)(
						owner.stats_.maximum_steal_probe,
						static_cast<uint32_t>(probe_count + 1));
					break;
				}
			}
		}
		++owner.stats_.steal_searches;
		return {best_different_key != invalid_index ? best_different_key : best, scope};
	}

	StableHandle allocate_cohort(SynthEngine& owner, RenderCohort cohort,
		bool starts_logical_voices)
	{
		if (maximum_cohorts != 0 && active_cohort_count >= maximum_cohorts)
		{
			const StealCandidate victim = steal_candidate(owner, cohort.channel, cohort.note);
			if (victim.index != invalid_index)
			{
				owner.stats_.stolen_voices += cohorts[victim.index].cohort.phase.multiplicity;
				++owner.stats_.cohort_capacity_steals;
				switch (victim.scope)
				{
				case StealCandidate::Scope::Channel:
					++owner.stats_.channel_scoped_steals;
					break;
				case StealCandidate::Scope::Reserve:
					++owner.stats_.channel_reserve_steals;
					break;
				case StealCandidate::Scope::Global:
					++owner.stats_.global_fallback_steals;
					break;
				}
				retire_cohort(owner, victim.index, true);
			}
		}
		uint32_t index = invalid_index;
		if (!free_cohorts.empty())
		{
			index = free_cohorts.back();
			free_cohorts.pop_back();
			auto& slot = cohorts[index];
			if (++slot.generation == 0)
				++slot.generation;
		}
		else
		{
			if (cohorts.size() >= invalid_index)
				return {};
			index = static_cast<uint32_t>(cohorts.size());
			cohorts.push_back({});
		}
		auto& slot = cohorts[index];
		slot.cohort = std::move(cohort);
		slot.occupied = true;
		link_cohort(index);
		++active_cohort_count;
		++owner.stats_.cohorts_created;
		if (starts_logical_voices)
			active_logical += slot.cohort.phase.multiplicity;
		owner.stats_.maximum_cohort_multiplicity = (std::max)(
			owner.stats_.maximum_cohort_multiplicity, slot.cohort.phase.multiplicity);
		update_peaks(owner);
		return {index, slot.generation};
	}

	StableHandle allocate_logical(LogicalBatch batch)
	{
		uint32_t index = invalid_index;
		if (!free_logical_batches.empty())
		{
			index = free_logical_batches.back();
			free_logical_batches.pop_back();
			auto& slot = logical_batches[index];
			if (++slot.generation == 0)
				++slot.generation;
		}
		else
		{
			if (logical_batches.size() >= invalid_index)
				return {};
			index = static_cast<uint32_t>(logical_batches.size());
			logical_batches.push_back({});
		}
		auto& slot = logical_batches[index];
		slot.batch = std::move(batch);
		slot.occupied = true;
		return {index, slot.generation};
	}

	void retire_logical(StableHandle handle) noexcept
	{
		auto* slot = resolve_logical(handle);
		if (!slot)
			return;
		slot->occupied = false;
		slot->batch = {};
		free_logical_batches.push_back(handle.index);
	}

	void begin_envelope(SynthEngine& owner, RenderCohort& cohort) noexcept
	{
		cohort.env = 0.0f;
		cohort.hold_samples_left = static_cast<uint32_t>(std::max(0.0,
			std::round(static_cast<double>(cohort.region->hold) * owner.sample_rate_)));
		if (cohort.region->attack > 0.0f)
		{
			cohort.stage = CohortStage::Attack;
			cohort.env_inc = 1.0f / (cohort.region->attack * owner.sample_rate_);
		}
		else if (cohort.hold_samples_left > 0)
		{
			cohort.stage = CohortStage::Hold;
			cohort.env = 1.0f;
		}
		else if (cohort.region->decay > 0.0f && cohort.region->sustain < 1.0f)
		{
			cohort.stage = CohortStage::Decay;
			cohort.env = 1.0f;
			cohort.env_inc = (cohort.region->sustain - 1.0f) /
				(cohort.region->decay * owner.sample_rate_);
		}
		else
		{
			cohort.stage = CohortStage::Sustain;
			cohort.env = cohort.region->sustain;
		}
	}

	bool begin_release(SynthEngine& owner, RenderCohort& cohort,
		float seconds_override = -1.0f) noexcept
	{
		const float seconds = seconds_override >= 0.0f
			? seconds_override : owner.release_seconds(*cohort.region, cohort.channel);
		if (seconds <= 0.0f || cohort.env <= 0.0f)
			return false;
		cohort.stage = CohortStage::Release;
		cohort.release_frame = owner.stats_.rendered_frames;
		cohort.env_inc = -cohort.env / (seconds * owner.sample_rate_);
		return true;
	}

	float advance_envelope(SynthEngine& owner, RenderCohort& cohort) noexcept
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
						(cohort.region->decay * owner.sample_rate_);
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
						(cohort.region->decay * owner.sample_rate_);
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
			cohort.env += cohort.env_inc;
			if (cohort.env <= 0.0f)
			{
				cohort.stage = CohortStage::Off;
				return 0.0f;
			}
			break;
		}
		return cohort.env;
	}

	PhaseAggregate make_phase_aggregate(SynthEngine& owner, const SampleRegion& region,
		size_t region_id, uint64_t first_serial, uint64_t count, uint8_t channel,
		uint8_t note, bool assignment)
	{
		PhaseAggregate aggregate;
		for (uint64_t offset = 0; offset < count; ++offset)
		{
			const auto state = assignment
				? owner.phase_processor_.assign(region, region_id, first_serial + offset, channel, note)
				: owner.phase_processor_.reconstruct(region, region_id, first_serial + offset,
					channel, note);
			aggregate.add(state);
		}
		return aggregate;
	}

	void release_range(SynthEngine& owner, LogicalBatch& batch, uint64_t first_serial,
		uint64_t count)
	{
		for (const auto& link : batch.links)
		{
			auto* source_slot = resolve(link.cohort);
			if (!source_slot)
				continue;
			auto& source = source_slot->cohort;
			if (!source.region || source.region->loop_mode == LoopMode::OneShot ||
				source.stage == CohortStage::Release)
				continue;
			PhaseAggregate contribution = make_phase_aggregate(owner, *source.region,
				link.region_id, first_serial, count, batch.channel, batch.note, false);
			RenderCohort released = source;
			released.phase = contribution;
			released.release_target = {};
			released.release_target_frame = 0;
			source.phase.subtract(contribution);
			const bool source_remains = source.phase.multiplicity != 0;
			const StableHandle cached_release_target = source.release_target;
			const bool cached_release_same_frame =
				source.release_target_frame == owner.stats_.rendered_frames;
			if (!begin_release(owner, released))
			{
				active_logical -= contribution.multiplicity;
				if (!source_remains)
					retire_cohort(owner, link.cohort.index, false);
				continue;
			}
			if (!source_remains)
				retire_cohort(owner, link.cohort.index, false);

			CohortSlot* target_slot = nullptr;
			if (cached_release_same_frame)
				target_slot = resolve(cached_release_target);
			if (target_slot && target_slot->cohort.stage == CohortStage::Release)
			{
				target_slot->cohort.phase.add(contribution);
				owner.stats_.maximum_cohort_multiplicity = (std::max)(
					owner.stats_.maximum_cohort_multiplicity,
					target_slot->cohort.phase.multiplicity);
				++owner.stats_.cohort_merges;
			}
			else
			{
				const StableHandle target = allocate_cohort(owner, std::move(released), false);
				if (target.valid() && source_remains)
				{
					if (auto* fresh_source = resolve(link.cohort))
					{
						fresh_source->cohort.release_target = target;
						fresh_source->cohort.release_target_frame = owner.stats_.rendered_frames;
					}
				}
			}
			if (source_remains)
				++owner.stats_.cohort_splits;
		}
	}

	void cleanup_stack(std::vector<StableHandle>& stack) noexcept
	{
		while (!stack.empty() && !resolve_logical(stack.back()))
			stack.pop_back();
	}

	void refresh_one_shot_batch(StableHandle handle) noexcept
	{
		auto* slot = resolve_logical(handle);
		if (!slot || !slot->batch.one_shot_blocking)
			return;
		auto& batch = slot->batch;
		for (const auto& link : batch.links)
			if (auto* cohort = resolve(link.cohort);
				cohort && cohort->cohort.region &&
				cohort->cohort.region->loop_mode == LoopMode::OneShot)
				return;
		batch.one_shot_blocking = false;
		if (batch.one_shot_release_applied)
		{
			retire_logical(handle);
			return;
		}
		for (const auto& link : batch.links)
			if (auto* cohort = resolve(link.cohort);
				cohort && cohort->cohort.stage != CohortStage::Release)
				return;
		retire_logical(handle);
	}

	void start_batch(SynthEngine& owner, uint8_t channel, uint8_t note,
		uint8_t velocity, uint64_t count,
		const std::vector<std::pair<size_t, const SampleRegion*>>& regions)
	{
		const uint64_t first_serial = owner.next_serial_;
		owner.next_serial_ += count;
		LogicalBatch batch;
		batch.channel = channel;
		batch.note = note;
		batch.velocity = velocity;
		batch.first_serial = first_serial;
		batch.held = count;
		batch.onset_frame = owner.stats_.rendered_frames;
		batch.one_shot_blocking = std::any_of(regions.begin(), regions.end(),
			[](const auto& item) { return item.second->loop_mode == LoopMode::OneShot; });
		const bool allow_onset_merge = !batch.one_shot_blocking;
		batch.links.reserve(regions.size());
		for (const auto& [region_id, region] : regions)
		{
			PhaseAggregate aggregate = make_phase_aggregate(owner, *region, region_id,
				first_serial, count, channel, note, true);
			const OnsetKey key{region, channel, note, velocity};
			StableHandle handle;
			auto candidate = onset_candidates.find(key);
			if (allow_onset_merge && candidate != onset_candidates.end())
			{
				if (auto* candidate_slot = resolve(candidate->second);
					candidate_slot && candidate_slot->cohort.stage != CohortStage::Release &&
					candidate_slot->cohort.birth_frame == owner.stats_.rendered_frames)
				{
					candidate_slot->cohort.phase.add(aggregate);
					active_logical += count;
					handle = candidate->second;
					owner.stats_.logical_voices_merged += count;
					owner.stats_.cohort_merges += count;
					owner.stats_.maximum_cohort_multiplicity = (std::max)(
						owner.stats_.maximum_cohort_multiplicity,
						candidate_slot->cohort.phase.multiplicity);
					update_peaks(owner);
				}
			}
			if (handle.valid())
			{
				batch.links.push_back({handle, region_id});
				owner.stats_.started_voices += count;
				owner.stats_.logical_voices_started += count;
				update_average_multiplicity(owner);
				continue;
			}
			RenderCohort cohort;
			cohort.region = region;
			cohort.region_id = region_id;
			cohort.note = note;
			cohort.velocity = velocity;
			cohort.channel = channel;
			cohort.base_inc = owner.compute_base_increment(*region, note);
			cohort.birth_frame = owner.stats_.rendered_frames;
			cohort.oldest_serial = first_serial;
			owner.compute_gains(*region, channel, velocity, cohort.gain_l, cohort.gain_r);
			begin_envelope(owner, cohort);
			cohort.phase = std::move(aggregate);
			handle = allocate_cohort(owner, std::move(cohort), true);
			if (handle.valid())
			{
				batch.links.push_back({handle, region_id});
				if (allow_onset_merge)
					onset_candidates[key] = handle;
			}
			owner.stats_.started_voices += count;
			owner.stats_.logical_voices_started += count;
			if (count > 1)
			{
				owner.stats_.logical_voices_merged += count - 1;
				owner.stats_.cohort_merges += count - 1;
			}
			update_average_multiplicity(owner);
		}
		if (!batch.links.empty())
		{
			const StableHandle logical = allocate_logical(std::move(batch));
			if (logical.valid())
				note_stacks[note_index(channel, note)].push_back(logical);
		}
	}

	void note_on_batch(SynthEngine& owner, uint8_t channel, uint8_t note,
		uint8_t velocity, uint64_t count)
	{
		if (count == 0 || velocity == 0 || !owner.soundfont_ || channel >= 16 || note >= 128)
			return;
		const auto& channel_state = owner.channels_[channel];
		const uint16_t selected_bank = static_cast<uint16_t>(
			(static_cast<uint16_t>(channel_state.controllers[0]) << 7) |
			channel_state.controllers[32]);
		const auto& render_regions = owner.all_regions_mode_ &&
			!owner.soundfont_->stress_regions.empty()
			? owner.soundfont_->stress_regions : owner.soundfont_->regions;
		auto& matches = matching_regions_scratch;
		matches.clear();
		bool has_exclusive = false;
		bool has_one_shot = false;
		auto consider = [&](size_t region_id)
		{
			const auto& region = render_regions[region_id];
			if (!region.pcm || region.pcm_len == 0 || region.channels < 1 || region.channels > 2 ||
				(!owner.all_regions_mode_ && (region.preset_bank != selected_bank ||
					region.preset_program != channel_state.program)) ||
				note < region.lo_key || note > region.hi_key ||
				velocity < region.lo_vel || velocity > region.hi_vel)
				return;
			matches.push_back({region_id, &region});
			has_exclusive = has_exclusive || region.exclusive_class != 0;
			has_one_shot = has_one_shot || region.loop_mode == LoopMode::OneShot;
		};
		if (const auto* candidates = owner.region_candidates(selected_bank, channel_state.program, note))
			for (const auto id : *candidates) consider(id);
		else
			for (size_t id = 0; id < render_regions.size(); ++id) consider(id);
		if (matches.empty())
		{
			owner.next_serial_ += count;
			return;
		}
		if ((has_exclusive || has_one_shot) && count > 1)
		{
			for (uint64_t index = 0; index < count; ++index)
				note_on_batch(owner, channel, note, velocity, 1);
			return;
		}
		for (const auto& [region_id, region] : matches)
		{
			(void)region_id;
			if (region->exclusive_class == 0)
				continue;
			for (uint32_t index = 0; index < cohorts.size(); ++index)
			{
				auto& slot = cohorts[index];
				if (!slot.occupied || slot.cohort.channel != channel || !slot.cohort.region ||
					slot.cohort.region->exclusive_class != region->exclusive_class ||
					slot.cohort.stage == CohortStage::Release)
					continue;
				if (!begin_release(owner, slot.cohort, 0.005f))
					retire_cohort(owner, index, true);
			}
		}
		start_batch(owner, channel, note, velocity, count, matches);
	}

	void note_off_batch(SynthEngine& owner, uint8_t channel, uint8_t note, uint64_t count)
	{
		if (channel >= 16 || note >= 128 || count == 0)
			return;
		auto& stack = note_stacks[note_index(channel, note)];
		while (count != 0)
		{
			cleanup_stack(stack);
			StableHandle selected;
			LogicalSlot* logical = nullptr;
			for (auto it = stack.rbegin(); it != stack.rend(); ++it)
			{
				refresh_one_shot_batch(*it);
				auto* candidate = resolve_logical(*it);
				if (candidate && candidate->batch.held != 0)
				{
					selected = *it;
					logical = candidate;
					break;
				}
			}
			if (!logical)
				break;
			auto& batch = logical->batch;
			if (batch.one_shot_blocking)
			{
				if (!batch.one_shot_release_applied)
				{
					batch.one_shot_release_applied = true;
					if (owner.channels_[channel].sustain_pedal)
						batch.one_shot_release_pending = true;
					else
						release_range(owner, batch, batch.first_serial, 1);
				}
				break;
			}
			const uint64_t released = (std::min)(count, batch.held);
			const uint64_t first_serial = batch.first_serial + batch.held - released;
			batch.held -= released;
			if (owner.channels_[channel].sustain_pedal)
				batch.sustained += released;
			else
				release_range(owner, batch, first_serial, released);
			count -= released;
			if (batch.held == 0 && batch.sustained == 0)
				retire_logical(selected);
			cleanup_stack(stack);
		}
	}

	void release_sustained(SynthEngine& owner, uint8_t channel)
	{
		if (channel >= 16)
			return;
		for (uint16_t note = 0; note < 128; ++note)
		{
			auto& stack = note_stacks[note_index(channel, static_cast<uint8_t>(note))];
			for (const StableHandle handle : stack)
			{
				auto* slot = resolve_logical(handle);
				if (!slot)
					continue;
				auto& batch = slot->batch;
				if (batch.one_shot_release_pending)
				{
					release_range(owner, batch, batch.first_serial, 1);
					batch.one_shot_release_pending = false;
				}
				if (batch.sustained == 0)
					continue;
				const uint64_t first_serial = batch.first_serial + batch.held;
				release_range(owner, batch, first_serial, batch.sustained);
				batch.sustained = 0;
				if (batch.held == 0)
					retire_logical(handle);
			}
			cleanup_stack(stack);
		}
	}

	void all_notes_off(SynthEngine& owner, uint8_t channel)
	{
		if (channel >= 16)
			return;
		for (uint16_t note = 0; note < 128; ++note)
		{
			auto& stack = note_stacks[note_index(channel, static_cast<uint8_t>(note))];
			for (const StableHandle handle : stack)
			{
				auto* slot = resolve_logical(handle);
				if (!slot)
					continue;
				auto& batch = slot->batch;
				if (owner.channels_[channel].sustain_pedal)
				{
					batch.sustained += batch.held;
					batch.held = 0;
				}
				else
				{
					const uint64_t count = batch.held + batch.sustained;
					if (count != 0)
						release_range(owner, batch, batch.first_serial, count);
					batch.held = 0;
					batch.sustained = 0;
					retire_logical(handle);
				}
			}
			if (!owner.channels_[channel].sustain_pedal)
				stack.clear();
		}
	}

	void all_sound_off(SynthEngine& owner, uint8_t channel)
	{
		if (channel >= 16)
			return;
		while (channel_heads[channel] != invalid_index)
			retire_cohort(owner, channel_heads[channel], true);
		for (uint32_t index = 0; index < logical_batches.size(); ++index)
			if (logical_batches[index].occupied &&
				logical_batches[index].batch.channel == channel)
				retire_logical({index, logical_batches[index].generation});
		for (uint16_t note = 0; note < 128; ++note)
			note_stacks[note_index(channel, static_cast<uint8_t>(note))].clear();
		for (auto it = onset_candidates.begin(); it != onset_candidates.end();)
			if (it->first.channel == channel)
				it = onset_candidates.erase(it);
			else
				++it;
	}

	void update_channel_gains(SynthEngine& owner, uint8_t channel) noexcept
	{
		for (uint32_t index = channel_heads[channel]; index != invalid_index;
			index = cohorts[index].channel_next)
		{
			auto& cohort = cohorts[index].cohort;
			owner.compute_gains(*cohort.region, channel, cohort.velocity,
				cohort.gain_l, cohort.gain_r);
		}
	}

	void set_render_threads(size_t requested) noexcept
	{
		parallel_render.reset();
		const size_t thread_count = std::clamp(requested, size_t{1}, size_t{64});
		if (thread_count == 1)
			return;
		try
		{
			auto state = std::make_unique<ParallelRenderState>();
			state->scratch.resize(thread_count);
			for (size_t lane = 1; lane < thread_count; ++lane)
				state->workers.emplace_back([this, shared = state.get(), lane] {
					uint64_t observed_epoch = 0;
					for (;;)
					{
						std::unique_lock lock(shared->mutex);
						shared->job_ready.wait(lock, [&] {
							return shared->stopping || shared->epoch != observed_epoch;
						});
						if (shared->stopping)
							return;
						observed_epoch = shared->epoch;
						SynthEngine* owner = shared->owner;
						const uint32_t frames = shared->frames;
						const size_t slot_count = shared->slot_count;
						const size_t lanes = shared->scratch.size();
						const size_t begin = slot_count * lane / lanes;
						const size_t end = slot_count * (lane + 1) / lanes;
						lock.unlock();

						auto& scratch = shared->scratch[lane];
						render_range(*owner, scratch.audio.data(), frames,
							static_cast<uint32_t>(begin), static_cast<uint32_t>(end),
							&scratch.retired);

						lock.lock();
						++shared->completed;
						if (shared->completed == shared->workers.size())
							shared->job_finished.notify_one();
					}
				});
			parallel_render = std::move(state);
		}
		catch (...)
		{
			parallel_render.reset();
		}
	}

	size_t render_threads() const noexcept
	{
		return parallel_render ? parallel_render->scratch.size() : 1;
	}

	void render_range(SynthEngine& owner, float* out, uint32_t frames,
		uint32_t begin, uint32_t end, std::vector<uint32_t>* retired) noexcept
	{
		for (uint32_t cohort_index = begin; cohort_index < end; ++cohort_index)
		{
			auto& slot = cohorts[cohort_index];
			if (!slot.occupied || !slot.cohort.region)
				continue;
			auto retire = [&] {
				if (retired)
					retired->push_back(cohort_index);
				else
					retire_cohort(owner, cohort_index, true);
			};
			auto& cohort = slot.cohort;
			const SampleRegion& region = *cohort.region;
			const auto& channel = owner.channels_[cohort.channel];
			const double increment = cohort.base_inc * channel.pitch_bend_ratio;
			const bool looping = region.loop_mode == LoopMode::Forward ||
				region.loop_mode == LoopMode::PingPong ||
				(region.loop_mode == LoopMode::Sustain && cohort.stage != CohortStage::Release);
			const bool valid_loop = looping && region.loop_start < region.loop_end &&
				region.loop_end <= region.pcm_len && region.loop_end - region.loop_start >= 2;
			const bool coherent_phase = cohort.phase.transformed_count == 0;
			const double coherent_count = static_cast<double>(cohort.phase.coherent_count);
			for (uint32_t frame = 0; frame < frames && slot.occupied; ++frame)
			{
				if (cohort.pos < 0.0 || cohort.pos >= region.pcm_len)
				{
					retire();
					break;
				}
				const float envelope = advance_envelope(owner, cohort);
				if (cohort.stage == CohortStage::Off)
				{
					retire();
					break;
				}

				const uint32_t index0 = static_cast<uint32_t>(cohort.pos);
				uint32_t index1 = (std::min)(index0 + 1, region.pcm_len - 1);
				if (valid_loop && cohort.loop_dir_fwd && index1 >= region.loop_end)
					index1 = region.loop_start;
				const float fraction = static_cast<float>(cohort.pos - index0);
				const size_t offset0 = region.pcm_right ? index0 :
					static_cast<size_t>(index0) * region.channels;
				const size_t offset1 = region.pcm_right ? index1 :
					static_cast<size_t>(index1) * region.channels;
				const float original_l0 = pcm_to_float(region.pcm[offset0]);
				const float original_l1 = pcm_to_float(region.pcm[offset1]);
				const float source_l0 = coherent_phase
					? static_cast<float>(static_cast<double>(original_l0) * coherent_count)
					: cohort.phase.sample(original_l0, index0, false);
				const float source_l1 = coherent_phase
					? static_cast<float>(static_cast<double>(original_l1) * coherent_count)
					: cohort.phase.sample(original_l1, index1, false);
				const float sample_l = source_l0 + (source_l1 - source_l0) * fraction;
				float sample_r = sample_l;
				if (region.channels == 2)
				{
					const float original_r0 = pcm_to_float(region.pcm_right ?
						region.pcm_right[index0] : region.pcm[offset0 + 1]);
					const float original_r1 = pcm_to_float(region.pcm_right ?
						region.pcm_right[index1] : region.pcm[offset1 + 1]);
					const float source_r0 = coherent_phase
						? static_cast<float>(static_cast<double>(original_r0) * coherent_count)
						: cohort.phase.sample(original_r0, index0, true);
					const float source_r1 = coherent_phase
						? static_cast<float>(static_cast<double>(original_r1) * coherent_count)
						: cohort.phase.sample(original_r1, index1, true);
					sample_r = source_r0 + (source_r1 - source_r0) * fraction;
				}
				const float amplitude = envelope * channel.volume * channel.expression *
					owner.master_volume_;
				out[static_cast<size_t>(frame) * 2] += sample_l * cohort.gain_l * amplitude;
				out[static_cast<size_t>(frame) * 2 + 1] += sample_r * cohort.gain_r * amplitude;

				cohort.pos += cohort.loop_dir_fwd ? increment : -increment;
				if (valid_loop && (region.loop_mode == LoopMode::Forward ||
					region.loop_mode == LoopMode::Sustain) && cohort.pos >= region.loop_end)
				{
					const double length = region.loop_end - region.loop_start;
					cohort.pos = region.loop_start + std::fmod(cohort.pos - region.loop_start, length);
				}
				else if (valid_loop && region.loop_mode == LoopMode::PingPong)
				{
					const double start = region.loop_start;
					const double span = static_cast<double>(region.loop_end - 1) - start;
					const double period = span * 2.0;
					double phase = std::fmod(cohort.pos - start, period);
					if (phase < 0.0) phase += period;
					cohort.loop_dir_fwd = phase <= span;
					cohort.pos = start + (cohort.loop_dir_fwd ? phase : period - phase);
				}
				else if (!valid_loop && cohort.pos >= region.pcm_len)
				{
					retire();
					break;
				}
			}
		}
	}

	bool render_parallel(SynthEngine& owner, float* out, uint32_t frames) noexcept
	{
		if (!parallel_render ||
			active_cohort_count < parallel_render->scratch.size() * 16)
			return false;
		const uint64_t work = static_cast<uint64_t>(active_cohort_count) * frames;
		if (work < 8192)
			return false;
		auto& state = *parallel_render;
		const size_t samples = static_cast<size_t>(frames) * 2;
		const size_t slot_count = cohorts.size();
		try
		{
			for (size_t lane = 0; lane < state.scratch.size(); ++lane)
			{
				auto& scratch = state.scratch[lane];
				scratch.audio.resize(samples);
				std::fill(scratch.audio.begin(), scratch.audio.end(), 0.0f);
				scratch.retired.clear();
				const size_t begin = slot_count * lane / state.scratch.size();
				const size_t end = slot_count * (lane + 1) / state.scratch.size();
				scratch.retired.reserve(end - begin);
			}
		}
		catch (...)
		{
			return false;
		}

		{
			const std::lock_guard lock(state.mutex);
			state.owner = &owner;
			state.frames = frames;
			state.slot_count = slot_count;
			state.completed = 0;
			++state.epoch;
		}
		state.job_ready.notify_all();
		const size_t main_end = slot_count / state.scratch.size();
		render_range(owner, state.scratch[0].audio.data(), frames, 0,
			static_cast<uint32_t>(main_end), &state.scratch[0].retired);
		{
			std::unique_lock lock(state.mutex);
			state.job_finished.wait(lock, [&] {
				return state.completed == state.workers.size();
			});
		}

		std::fill(out, out + samples, 0.0f);
		for (const auto& scratch : state.scratch)
			add_mix_buffer(out, scratch.audio.data(), samples);
		for (auto& scratch : state.scratch)
			for (const uint32_t index : scratch.retired)
				if (cohorts[index].occupied)
					retire_cohort(owner, index, true);
		++owner.stats_.parallel_render_calls;
		owner.stats_.parallel_rendered_frames += frames;
		return true;
	}

	void render_audio(SynthEngine& owner, float* out, uint32_t frames) noexcept
	{
		onset_candidates.clear();
		if (!render_parallel(owner, out, frames))
		{
			std::fill(out, out + static_cast<size_t>(frames) * 2, 0.0f);
			render_range(owner, out, frames, 0, static_cast<uint32_t>(cohorts.size()), nullptr);
		}
		owner.stats_.rendered_frames += frames;
	}

	void clear() noexcept
	{
		cohorts.clear();
		free_cohorts.clear();
		channel_heads.fill(invalid_index);
		steal_cursors.fill(invalid_index);
		note_heads.fill(invalid_index);
		logical_batches.clear();
		free_logical_batches.clear();
		for (auto& stack : note_stacks)
			stack.clear();
		onset_candidates.clear();
		active_logical = 0;
		active_cohort_count = 0;
		active_cohorts_by_channel.fill(0);
	}
};

CohortEngine::CohortEngine() : state_(std::make_unique<CohortEngineState>()) {}
CohortEngine::~CohortEngine() = default;
CohortEngine::CohortEngine(CohortEngine&&) noexcept = default;
CohortEngine& CohortEngine::operator=(CohortEngine&&) noexcept = default;

void CohortEngine::configure(SynthEngine& owner, size_t maximum_cohorts) noexcept
{
	if (!state_)
		return;
	state_->clear();
	state_->maximum_cohorts = maximum_cohorts;
	(void)owner;
}

void CohortEngine::set_render_threads(size_t threads) noexcept
{
	if (state_)
		state_->set_render_threads(threads);
}

void CohortEngine::reserve_playback(uint32_t maximum_block_frames, size_t region_count)
{
	if (!state_) return;
	auto& state = *state_;
	// Reserve, do not resize: unused slots must not affect iteration/stealing order.
	const size_t capacity = state.maximum_cohorts;
	state.cohorts.reserve(capacity);
	state.free_cohorts.reserve(capacity);
	state.logical_batches.reserve(capacity);
	state.free_logical_batches.reserve(capacity);
	state.onset_candidates.reserve(capacity);
	state.matching_regions_scratch.reserve(region_count);
	if (state.parallel_render)
		for (auto& scratch : state.parallel_render->scratch)
		{
			scratch.audio.resize(static_cast<size_t>(maximum_block_frames) * 2);
			scratch.retired.reserve((capacity + state.render_threads() - 1) / state.render_threads());
		}
}

size_t CohortEngine::render_threads() const noexcept
{
	return state_ ? state_->render_threads() : 1;
}

void CohortEngine::clear(SynthEngine& owner) noexcept
{
	if (state_)
		state_->clear();
	(void)owner;
}

void CohortEngine::note_on_batch(SynthEngine& owner, uint8_t channel, uint8_t note,
	uint8_t velocity, uint64_t count) noexcept
{
	if (!state_)
		return;
	try { state_->note_on_batch(owner, channel, note, velocity, count); }
	catch (...) {}
}

void CohortEngine::note_off_batch(SynthEngine& owner, uint8_t channel, uint8_t note,
	uint64_t count) noexcept
{
	if (!state_)
		return;
	try { state_->note_off_batch(owner, channel, note, count); }
	catch (...) {}
}

void CohortEngine::release_sustained(SynthEngine& owner, uint8_t channel) noexcept
{
	if (!state_)
		return;
	try { state_->release_sustained(owner, channel); }
	catch (...) {}
}

void CohortEngine::all_notes_off(SynthEngine& owner, uint8_t channel) noexcept
{
	if (!state_)
		return;
	try { state_->all_notes_off(owner, channel); }
	catch (...) {}
}

void CohortEngine::all_sound_off(SynthEngine& owner, uint8_t channel) noexcept
{
	if (!state_)
		return;
	try { state_->all_sound_off(owner, channel); }
	catch (...) {}
}

void CohortEngine::update_channel_gains(SynthEngine& owner, uint8_t channel) noexcept
{
	if (state_)
		state_->update_channel_gains(owner, channel);
}

void CohortEngine::invalidate_onset_merges(uint8_t channel) noexcept
{
	if (state_)
		for (auto it = state_->onset_candidates.begin(); it != state_->onset_candidates.end();)
			if (it->first.channel == channel)
				it = state_->onset_candidates.erase(it);
			else
				++it;
}

void CohortEngine::invalidate_all_onset_merges() noexcept
{
	if (state_)
		state_->onset_candidates.clear();
}

void CohortEngine::render_audio(SynthEngine& owner, float* out, uint32_t frames) noexcept
{
	if (state_)
		state_->render_audio(owner, out, frames);
}

size_t CohortEngine::active_logical_voices() const noexcept
{
	return state_ ? static_cast<size_t>((std::min)(state_->active_logical,
		static_cast<uint64_t>((std::numeric_limits<size_t>::max)()))) : 0;
}

size_t CohortEngine::active_cohorts() const noexcept
{
	return state_ ? state_->active_cohort_count : 0;
}

} // namespace safsyn
