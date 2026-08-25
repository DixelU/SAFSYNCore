#include "core.h"
#include "cohort_engine.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace safsyn
{
namespace
{
constexpr double pi = 3.1415926535897932384626433832795;

float pcm_to_float(int16_t sample) noexcept
{
	return static_cast<float>(sample) / 32768.0f;
}

const std::array<float, 128>& velocity_amplitudes() noexcept
{
	static const auto amplitudes = [] {
		std::array<float, 128> result{};
		for (size_t velocity = 1; velocity < result.size(); ++velocity)
			result[velocity] = std::pow(static_cast<float>(velocity) / 127.0f, 1.7f);
		return result;
	}();
	return amplitudes;
}
}

SynthEngine::SynthEngine(uint32_t sample_rate, size_t voice_capacity)
	: voices_((std::max)(voice_capacity, size_t{1})),
	  cohort_engine_(std::make_unique<CohortEngine>()),
	  sample_rate_((std::max)(sample_rate, uint32_t{1}))
{
	reset();
}

SynthEngine::~SynthEngine() = default;
SynthEngine::SynthEngine(SynthEngine&&) noexcept = default;
SynthEngine& SynthEngine::operator=(SynthEngine&&) noexcept = default;

size_t SynthEngine::active_voice_count() const noexcept
{
	if (voice_model_ == VoiceModel::Cohorts)
		return cohort_engine_ ? cohort_engine_->active_logical_voices() : 0;
	return static_cast<size_t>(std::count_if(voices_.begin(), voices_.end(),
		[](const Voice& voice) { return voice.active(); }));
}

size_t SynthEngine::active_cohort_count() const noexcept
{
	return voice_model_ == VoiceModel::Cohorts && cohort_engine_
		? cohort_engine_->active_cohorts() : active_voice_count();
}

void SynthEngine::set_soundfont(const Soundfont* soundfont) noexcept
{
	silence_all();
	phase_processor_.clear();
	soundfont_ = soundfont;
}

void SynthEngine::set_phase_settings(const PhaseSettings& settings) noexcept
{
	silence_all();
	phase_processor_.configure(settings);
}

void SynthEngine::set_voice_model(VoiceModel model, size_t maximum_cohorts) noexcept
{
	silence_all();
	voice_model_ = model;
	maximum_cohorts_ = maximum_cohorts;
	if (cohort_engine_)
		cohort_engine_->configure(*this, maximum_cohorts_);
}

void SynthEngine::reset() noexcept
{
	silence_all();
	for (auto& channel : channels_)
		channel = ChannelState{};
	master_volume_ = 1.0f;
	next_serial_ = 1;
	stats_ = {};
}

void SynthEngine::silence_all() noexcept
{
	for (auto& voice : voices_)
		voice = Voice{};
	if (cohort_engine_)
		cohort_engine_->clear(*this);
}

SynthEngine::Voice* SynthEngine::allocate_voice(uint8_t request_channel,
	uint8_t request_note) noexcept
{
	for (auto& voice : voices_)
		if (!voice.active())
			return &voice;

	std::array<size_t, 16> channel_counts{};
	for (const auto& voice : voices_)
		if (voice.active())
			++channel_counts[voice.channel];
	const size_t channel_reserve = voices_.size() / channel_counts.size();

	auto estimated_level = [&](const Voice& voice) {
		const auto& channel = channels_[voice.channel];
		return voice.env * channel.volume * channel.expression * master_volume_ *
			(std::max)(std::abs(voice.gain_l), std::abs(voice.gain_r));
	};
	auto prefer = [&](const Voice* current, const Voice& challenger) {
		if (!current)
			return true;
		const bool challenger_releasing = challenger.stage == Voice::Stage::Release;
		const bool current_releasing = current->stage == Voice::Stage::Release;
		const float challenger_level = estimated_level(challenger);
		const float current_level = estimated_level(*current);
		return (challenger_releasing && !current_releasing) ||
			(challenger_releasing == current_releasing &&
				(challenger_level < current_level ||
					(challenger_level == current_level &&
						challenger.serial > current->serial)));
	};
	Voice* same_channel = nullptr;
	Voice* same_channel_different_key = nullptr;
	Voice* over_reserve = nullptr;
	Voice* global = nullptr;
	Voice* global_different_key = nullptr;
	for (auto& voice : voices_)
	{
		const bool same_key = voice.channel == request_channel && voice.note == request_note;
		if (prefer(global, voice))
			global = &voice;
		if (!same_key && prefer(global_different_key, voice))
			global_different_key = &voice;
		if (voice.channel == request_channel)
		{
			if (prefer(same_channel, voice))
				same_channel = &voice;
			if (!same_key && prefer(same_channel_different_key, voice))
				same_channel_different_key = &voice;
		}
		if (channel_counts[voice.channel] > channel_reserve &&
			prefer(over_reserve, voice))
			over_reserve = &voice;
	}

	Voice* candidate = nullptr;
	if (channel_counts[request_channel] != 0 &&
		channel_counts[request_channel] >= channel_reserve)
	{
		candidate = same_channel_different_key ? same_channel_different_key : same_channel;
		if (candidate)
			++stats_.channel_scoped_steals;
	}
	else if (channel_counts[request_channel] < channel_reserve && over_reserve)
	{
		candidate = over_reserve;
		++stats_.channel_reserve_steals;
	}
	if (!candidate)
	{
		candidate = global_different_key ? global_different_key : global;
		++stats_.global_fallback_steals;
	}
	++stats_.stolen_voices;
	return candidate;
}

double SynthEngine::compute_increment(const SampleRegion& region, uint8_t note,
	float bend_semitones) const noexcept
{
	const double tracked_cents =
		(static_cast<int>(note) - static_cast<int>(region.root_key)) *
		static_cast<double>(region.scale_tuning);
	const double cents = tracked_cents + region.coarse_tune * 100.0 +
		region.fine_tune + bend_semitones * 100.0;
	return std::pow(2.0, cents / 1200.0) *
		(static_cast<double>(region.sample_rate) / sample_rate_);
}

void SynthEngine::compute_gains(const SampleRegion& region, uint8_t channel,
	uint8_t velocity, float& left, float& right) const noexcept
{
	const float combined_pan = std::clamp(region.pan + channels_[channel].pan,
		-1.0f, 1.0f);
	const double angle = (static_cast<double>(combined_pan) + 1.0) * pi * 0.25;
	const float base_gain = region.attenuation * velocity_amplitudes()[velocity];
	left = base_gain * static_cast<float>(std::cos(angle));
	right = base_gain * static_cast<float>(std::sin(angle));
}

void SynthEngine::begin_envelope(Voice& voice) noexcept
{
	voice.env = 0.0f;
	voice.hold_samples_left = static_cast<uint32_t>(
		std::max(0.0, std::round(static_cast<double>(voice.region->hold) * sample_rate_)));
	if (voice.region->attack > 0.0f)
	{
		voice.stage = Voice::Stage::Attack;
		voice.env_inc = 1.0f / (voice.region->attack * sample_rate_);
	}
	else if (voice.hold_samples_left > 0)
	{
		voice.stage = Voice::Stage::Hold;
		voice.env = 1.0f;
	}
	else if (voice.region->decay > 0.0f && voice.region->sustain < 1.0f)
	{
		voice.stage = Voice::Stage::Decay;
		voice.env = 1.0f;
		voice.env_inc = (voice.region->sustain - 1.0f) /
			(voice.region->decay * sample_rate_);
	}
	else
	{
		voice.stage = Voice::Stage::Sustain;
		voice.env = voice.region->sustain;
	}
}

void SynthEngine::begin_release(Voice& voice, float seconds_override) noexcept
{
	if (!voice.active() || voice.stage == Voice::Stage::Release)
		return;
	const float seconds = seconds_override >= 0.0f ? seconds_override : voice.region->release;
	voice.note_off_pending = false;
	if (seconds <= 0.0f || voice.env <= 0.0f)
	{
		voice = Voice{};
		return;
	}
	voice.stage = Voice::Stage::Release;
	voice.env_inc = -voice.env / (seconds * sample_rate_);
}

float SynthEngine::advance_envelope(Voice& voice) noexcept
{
	switch (voice.stage)
	{
	case Voice::Stage::Off:
		return 0.0f;
	case Voice::Stage::Attack:
		voice.env += voice.env_inc;
		if (voice.env >= 1.0f)
		{
			voice.env = 1.0f;
			if (voice.hold_samples_left > 0)
				voice.stage = Voice::Stage::Hold;
			else if (voice.region->decay > 0.0f && voice.region->sustain < 1.0f)
			{
				voice.stage = Voice::Stage::Decay;
				voice.env_inc = (voice.region->sustain - 1.0f) /
					(voice.region->decay * sample_rate_);
			}
			else
				voice.stage = Voice::Stage::Sustain;
		}
		break;
	case Voice::Stage::Hold:
		if (voice.hold_samples_left > 0)
			--voice.hold_samples_left;
		if (voice.hold_samples_left == 0)
		{
			if (voice.region->decay > 0.0f && voice.region->sustain < 1.0f)
			{
				voice.stage = Voice::Stage::Decay;
				voice.env_inc = (voice.region->sustain - 1.0f) /
					(voice.region->decay * sample_rate_);
			}
			else
				voice.stage = Voice::Stage::Sustain;
		}
		break;
	case Voice::Stage::Decay:
		voice.env += voice.env_inc;
		if (voice.env <= voice.region->sustain)
		{
			voice.stage = Voice::Stage::Sustain;
			voice.env = voice.region->sustain;
		}
		break;
	case Voice::Stage::Sustain:
		voice.env = voice.region->sustain;
		break;
	case Voice::Stage::Release:
		voice.env += voice.env_inc;
		if (voice.env <= 0.0f)
		{
			voice = Voice{};
			return 0.0f;
		}
		break;
	}
	return voice.env;
}

void SynthEngine::note_on(uint8_t channel, uint8_t note, uint8_t velocity) noexcept
{
	if (voice_model_ == VoiceModel::Cohorts)
	{
		if (velocity == 0)
			note_off_batch(channel, note, 1);
		else if (cohort_engine_)
			cohort_engine_->note_on_batch(*this, channel, note, velocity, 1);
		return;
	}
	if (velocity == 0)
	{
		note_off(channel, note);
		return;
	}
	if (!soundfont_ || channel >= 16 || note >= 128)
		return;

	const uint64_t serial = next_serial_++;
	const auto& channel_state = channels_[channel];
	const uint16_t selected_bank = static_cast<uint16_t>(
		(static_cast<uint16_t>(channel_state.controllers[0]) << 7) |
		channel_state.controllers[32]);
	const auto& render_regions = all_regions_mode_ && !soundfont_->stress_regions.empty()
		? soundfont_->stress_regions : soundfont_->regions;
	for (size_t region_id = 0; region_id < render_regions.size(); ++region_id)
	{
		const auto& region = render_regions[region_id];
		if (!region.pcm || region.pcm_len == 0 || region.channels < 1 || region.channels > 2 ||
			(!all_regions_mode_ && (region.preset_bank != selected_bank ||
				region.preset_program != channel_state.program)) ||
			note < region.lo_key || note > region.hi_key ||
			velocity < region.lo_vel || velocity > region.hi_vel)
			continue;

		if (region.exclusive_class != 0)
			for (auto& existing : voices_)
				if (existing.active() && existing.channel == channel && existing.region &&
					existing.region->exclusive_class == region.exclusive_class)
					begin_release(existing, 0.005f);

		Voice* voice = allocate_voice(channel, note);
		*voice = Voice{};
		voice->region = &region;
		voice->note = note;
		voice->velocity = velocity;
		voice->channel = channel;
		voice->inc = compute_increment(region, note, channels_[channel].pitch_bend_semitones);
		voice->serial = serial;
		voice->phase = phase_processor_.assign(region, region_id, serial, channel, note);
		compute_gains(region, channel, velocity, voice->gain_l, voice->gain_r);
		begin_envelope(*voice);
		++stats_.started_voices;
		++stats_.logical_voices_started;
		stats_.average_cohort_multiplicity = 1.0;
		stats_.maximum_cohort_multiplicity = 1;
	}
	stats_.peak_active_voices = (std::max)(stats_.peak_active_voices, active_voice_count());
	stats_.peak_active_logical_voices = stats_.peak_active_voices;
}

void SynthEngine::note_on_batch(uint8_t channel, uint8_t note, uint8_t velocity,
	uint64_t count) noexcept
{
	if (count == 0)
		return;
	if (voice_model_ == VoiceModel::Cohorts)
	{
		if (velocity == 0)
			note_off_batch(channel, note, count);
		else if (cohort_engine_)
			cohort_engine_->note_on_batch(*this, channel, note, velocity, count);
		return;
	}
	for (uint64_t index = 0; index < count; ++index)
		note_on(channel, note, velocity);
}

void SynthEngine::note_off(uint8_t channel, uint8_t note) noexcept
{
	if (voice_model_ == VoiceModel::Cohorts)
	{
		if (cohort_engine_)
			cohort_engine_->note_off_batch(*this, channel, note, 1);
		return;
	}
	if (channel >= 16)
		return;
	uint64_t newest_serial = 0;
	for (const auto& voice : voices_)
		if (voice.active() && voice.stage != Voice::Stage::Release && !voice.note_off_pending &&
			voice.channel == channel && voice.note == note)
			newest_serial = (std::max)(newest_serial, voice.serial);
	if (newest_serial == 0)
		return;

	for (auto& voice : voices_)
	{
		if (!voice.active() || voice.channel != channel || voice.note != note ||
			voice.serial != newest_serial)
			continue;
		if (voice.region->loop_mode == LoopMode::OneShot)
			continue;
		if (channels_[channel].sustain_pedal)
			voice.note_off_pending = true;
		else
			begin_release(voice);
	}
}

void SynthEngine::note_off_batch(uint8_t channel, uint8_t note, uint64_t count) noexcept
{
	if (count == 0)
		return;
	if (voice_model_ == VoiceModel::Cohorts)
	{
		if (cohort_engine_)
			cohort_engine_->note_off_batch(*this, channel, note, count);
		return;
	}
	for (uint64_t index = 0; index < count; ++index)
		note_off(channel, note);
}

void SynthEngine::update_channel_voice_gains(uint8_t channel) noexcept
{
	if (voice_model_ == VoiceModel::Cohorts)
	{
		if (cohort_engine_)
			cohort_engine_->update_channel_gains(*this, channel);
		return;
	}
	for (auto& voice : voices_)
		if (voice.active() && voice.channel == channel)
			compute_gains(*voice.region, channel, voice.velocity, voice.gain_l, voice.gain_r);
}

void SynthEngine::update_channel_pitch_bend(uint8_t channel) noexcept
{
	auto& state = channels_[channel];
	const int displacement = static_cast<int>(state.pitch_bend_value) - 8192;
	const float normalized = displacement < 0
		? displacement / 8192.0f
		: displacement / 8191.0f;
	state.pitch_bend_semitones = normalized * state.pitch_bend_range_semitones;
	update_channel_pitch(channel);
}

void SynthEngine::update_channel_pitch(uint8_t channel) noexcept
{
	if (voice_model_ == VoiceModel::Cohorts)
	{
		if (cohort_engine_)
			cohort_engine_->update_channel_pitch(*this, channel);
		return;
	}
	for (auto& voice : voices_)
		if (voice.active() && voice.channel == channel)
			voice.inc = compute_increment(*voice.region, voice.note,
				channels_[channel].pitch_bend_semitones);
}

void SynthEngine::all_notes_off(uint8_t channel) noexcept
{
	if (voice_model_ == VoiceModel::Cohorts)
	{
		if (cohort_engine_)
			cohort_engine_->all_notes_off(*this, channel);
		return;
	}
	for (auto& voice : voices_)
		if (voice.active() && voice.channel == channel &&
			voice.region->loop_mode != LoopMode::OneShot &&
			voice.stage != Voice::Stage::Release && !voice.note_off_pending)
		{
			if (channels_[channel].sustain_pedal)
				voice.note_off_pending = true;
			else
				begin_release(voice);
		}
}

void SynthEngine::all_sound_off(uint8_t channel) noexcept
{
	if (voice_model_ == VoiceModel::Cohorts)
	{
		if (cohort_engine_)
			cohort_engine_->all_sound_off(*this, channel);
		return;
	}
	for (auto& voice : voices_)
		if (voice.active() && voice.channel == channel)
			voice = Voice{};
}

void SynthEngine::control_change(uint8_t channel, uint8_t controller, uint8_t value) noexcept
{
	if (channel >= 16 || controller >= 128)
		return;
	value = (std::min)(value, uint8_t{127});
	if (voice_model_ == VoiceModel::Cohorts && cohort_engine_)
		cohort_engine_->invalidate_onset_merges(channel);
	auto& state = channels_[channel];
	state.controllers[controller] = value;
	switch (controller)
	{
	case 0:
		break;
	case 7:
		state.volume = static_cast<float>((state.controllers[7] << 7) |
			state.controllers[39]) / 16383.0f;
		break;
	case 10:
	{
		const int pan14 = (state.controllers[10] << 7) | state.controllers[42];
		state.pan = pan14 < 8192 ? (pan14 - 8192) / 8192.0f :
			(pan14 - 8192) / 8191.0f;
	}
		update_channel_voice_gains(channel);
		break;
	case 11:
		state.expression = static_cast<float>((state.controllers[11] << 7) |
			state.controllers[43]) / 16383.0f;
		break;
	case 32:
		break;
	case 39:
		state.volume = static_cast<float>((state.controllers[7] << 7) |
			state.controllers[39]) / 16383.0f;
		break;
	case 42:
	{
		const int pan14 = (state.controllers[10] << 7) | state.controllers[42];
		state.pan = pan14 < 8192 ? (pan14 - 8192) / 8192.0f :
			(pan14 - 8192) / 8191.0f;
	}
		update_channel_voice_gains(channel);
		break;
	case 43:
		state.expression = static_cast<float>((state.controllers[11] << 7) |
			state.controllers[43]) / 16383.0f;
		break;
	case 64:
	{
		const bool was_down = state.sustain_pedal;
		state.sustain_pedal = value >= 64;
		if (was_down && !state.sustain_pedal)
		{
			if (voice_model_ == VoiceModel::Cohorts)
			{
				if (cohort_engine_)
					cohort_engine_->release_sustained(*this, channel);
			}
			else
				for (auto& voice : voices_)
					if (voice.active() && voice.channel == channel && voice.note_off_pending)
						begin_release(voice);
		}
		break;
	}
	case 98:
	case 99:
		state.parameter_selection = ChannelState::ParameterSelection::Nrpn;
		break;
	case 100:
	case 101:
		state.parameter_selection = state.controllers[100] == 127 &&
			state.controllers[101] == 127
			? ChannelState::ParameterSelection::None
			: ChannelState::ParameterSelection::Rpn;
		break;
	case 6:
	case 38:
		if (state.parameter_selection == ChannelState::ParameterSelection::Rpn &&
			state.controllers[101] == 0 && state.controllers[100] == 0)
		{
			state.pitch_bend_range_semitones = state.controllers[6] +
				state.controllers[38] / 100.0f;
			update_channel_pitch_bend(channel);
		}
		break;
	case 120:
		all_sound_off(channel);
		break;
	case 121:
	{
		const bool release_pending = state.sustain_pedal;
		const uint8_t bank_msb = state.controllers[0];
		const uint8_t bank_lsb = state.controllers[32];
		const uint8_t program = state.program;
		state = ChannelState{};
		state.controllers[0] = bank_msb;
		state.controllers[32] = bank_lsb;
		state.program = program;
		if (release_pending)
		{
			if (voice_model_ == VoiceModel::Cohorts)
			{
				if (cohort_engine_)
					cohort_engine_->release_sustained(*this, channel);
			}
			else
				for (auto& voice : voices_)
					if (voice.active() && voice.channel == channel && voice.note_off_pending)
						begin_release(voice);
		}
		update_channel_voice_gains(channel);
		update_channel_pitch(channel);
		break;
	}
	case 123:
	case 124:
	case 125:
	case 126:
	case 127:
		all_notes_off(channel);
		break;
	default:
		break;
	}
}

uint8_t SynthEngine::controller_value(uint8_t channel, uint8_t controller) const noexcept
{
	return channel < 16 && controller < 128 ? channels_[channel].controllers[controller] : 0;
}

void SynthEngine::set_master_volume(uint16_t value14) noexcept
{
	value14 = (std::min)(value14, uint16_t{16383});
	master_volume_ = static_cast<float>(value14) / 16383.0f;
	if (voice_model_ == VoiceModel::Cohorts && cohort_engine_)
		cohort_engine_->invalidate_all_onset_merges();
}

void SynthEngine::program_change(uint8_t channel, uint8_t program) noexcept
{
	if (channel < 16)
	{
		if (voice_model_ == VoiceModel::Cohorts && cohort_engine_)
			cohort_engine_->invalidate_onset_merges(channel);
		channels_[channel].program = static_cast<uint8_t>((std::min)(program, uint8_t{127}));
	}
}

void SynthEngine::set_pitch_bend(uint8_t channel, uint16_t value14) noexcept
{
	if (channel >= 16)
		return;
	if (voice_model_ == VoiceModel::Cohorts && cohort_engine_)
		cohort_engine_->invalidate_onset_merges(channel);
	channels_[channel].pitch_bend_value = (std::min)(value14, uint16_t{16383});
	update_channel_pitch_bend(channel);
}

uint16_t SynthEngine::pitch_bend_value(uint8_t channel) const noexcept
{
	return channel < 16 ? channels_[channel].pitch_bend_value : 8192;
}

float SynthEngine::pitch_bend_range_semitones(uint8_t channel) const noexcept
{
	return channel < 16 ? channels_[channel].pitch_bend_range_semitones : 2.0f;
}

void SynthEngine::consume_short_message(uint32_t message) noexcept
{
	const uint8_t status = static_cast<uint8_t>(message & 0xff);
	const uint8_t data1 = static_cast<uint8_t>((message >> 8) & 0x7f);
	const uint8_t data2 = static_cast<uint8_t>((message >> 16) & 0x7f);
	const uint8_t channel = status & 0x0f;
	switch (status & 0xf0)
	{
	case 0x80:
		note_off(channel, data1);
		break;
	case 0x90:
		if (data2 == 0) note_off(channel, data1);
		else note_on(channel, data1, data2);
		break;
	case 0xb0:
		control_change(channel, data1, data2);
		break;
	case 0xc0:
		program_change(channel, data1);
		break;
	case 0xe0:
		set_pitch_bend(channel, static_cast<uint16_t>((data2 << 7) | data1));
		break;
	default:
		break;
	}
}

void SynthEngine::consume_short_messages(const uint32_t* messages, size_t count) noexcept
{
	if (!messages || count == 0)
		return;
	for (size_t index = 0; index < count;)
	{
		const uint32_t message = messages[index];
		const uint8_t status = static_cast<uint8_t>(message & 0xff);
		const uint8_t data1 = static_cast<uint8_t>((message >> 8) & 0x7f);
		const uint8_t data2 = static_cast<uint8_t>((message >> 16) & 0x7f);
		const uint8_t command = status & 0xf0;
		const uint8_t channel = status & 0x0f;
		const bool note_on_message = command == 0x90 && data2 != 0;
		const bool note_off_message = command == 0x80 || (command == 0x90 && data2 == 0);
		if (!note_on_message && !note_off_message)
		{
			consume_short_message(message);
			++index;
			continue;
		}
		size_t end = index + 1;
		while (end < count)
		{
			const uint32_t next = messages[end];
			const uint8_t next_status = static_cast<uint8_t>(next & 0xff);
			const uint8_t next_data1 = static_cast<uint8_t>((next >> 8) & 0x7f);
			const uint8_t next_data2 = static_cast<uint8_t>((next >> 16) & 0x7f);
			const uint8_t next_command = next_status & 0xf0;
			const bool next_note_on = next_command == 0x90 && next_data2 != 0;
			const bool next_note_off = next_command == 0x80 ||
				(next_command == 0x90 && next_data2 == 0);
			if ((next_status & 0x0f) != channel || next_data1 != data1 ||
				next_note_on != note_on_message || next_note_off != note_off_message ||
				(note_on_message && next_data2 != data2))
				break;
			++end;
		}
		const uint64_t run = end - index;
		if (note_on_message)
			note_on_batch(channel, data1, data2, run);
		else
			note_off_batch(channel, data1, run);
		index = end;
	}
}

void SynthEngine::render_audio(float* out, uint32_t frames) noexcept
{
	if (!out || frames == 0)
		return;
	if (voice_model_ == VoiceModel::Cohorts)
	{
		if (cohort_engine_)
			cohort_engine_->render_audio(*this, out, frames);
		else
			std::fill(out, out + static_cast<size_t>(frames) * 2, 0.0f);
		return;
	}
	std::fill(out, out + static_cast<size_t>(frames) * 2, 0.0f);

	for (auto& voice : voices_)
	{
		if (!voice.active() || !voice.region)
			continue;
		const SampleRegion& region = *voice.region;
		const bool looping = region.loop_mode == LoopMode::Forward ||
			region.loop_mode == LoopMode::PingPong ||
			(region.loop_mode == LoopMode::Sustain && voice.stage != Voice::Stage::Release);
		const bool valid_loop = looping && region.loop_start < region.loop_end &&
			region.loop_end <= region.pcm_len && region.loop_end - region.loop_start >= 2;

		for (uint32_t frame = 0; frame < frames && voice.active(); ++frame)
		{
			if (voice.pos < 0.0 || voice.pos >= region.pcm_len)
			{
				voice = Voice{};
				break;
			}
			const float envelope = advance_envelope(voice);
			if (!voice.active())
				break;

			const uint32_t index0 = static_cast<uint32_t>(voice.pos);
			uint32_t index1 = (std::min)(index0 + 1, region.pcm_len - 1);
			if (valid_loop && voice.loop_dir_fwd && index1 >= region.loop_end)
				index1 = region.loop_start;
			const float fraction = static_cast<float>(voice.pos - index0);
			const size_t offset0 = region.pcm_right ? index0 :
				static_cast<size_t>(index0) * region.channels;
			const size_t offset1 = region.pcm_right ? index1 :
				static_cast<size_t>(index1) * region.channels;
			float source_l0 = pcm_to_float(region.pcm[offset0]);
			float source_l1 = pcm_to_float(region.pcm[offset1]);
			if (voice.phase.kind != PhaseVoiceState::Kind::Coherent)
			{
				source_l0 = voice.phase.apply(source_l0, index0, false);
				source_l1 = voice.phase.apply(source_l1, index1, false);
			}
			const float sample_l = source_l0 + (source_l1 - source_l0) * fraction;
			float sample_r = sample_l;
			if (region.channels == 2)
			{
				float source_r0 = pcm_to_float(region.pcm_right ?
					region.pcm_right[index0] : region.pcm[offset0 + 1]);
				float source_r1 = pcm_to_float(region.pcm_right ?
					region.pcm_right[index1] : region.pcm[offset1 + 1]);
				if (voice.phase.kind != PhaseVoiceState::Kind::Coherent)
				{
					source_r0 = voice.phase.apply(source_r0, index0, true);
					source_r1 = voice.phase.apply(source_r1, index1, true);
				}
				sample_r = source_r0 + (source_r1 - source_r0) * fraction;
			}

			const ChannelState& channel = channels_[voice.channel];
			const float amplitude = envelope * channel.volume * channel.expression *
				master_volume_;
			out[static_cast<size_t>(frame) * 2] += sample_l * voice.gain_l * amplitude;
			out[static_cast<size_t>(frame) * 2 + 1] += sample_r * voice.gain_r * amplitude;

			voice.pos += voice.loop_dir_fwd ? voice.inc : -voice.inc;
			if (valid_loop && (region.loop_mode == LoopMode::Forward ||
				region.loop_mode == LoopMode::Sustain) && voice.pos >= region.loop_end)
			{
				const double length = region.loop_end - region.loop_start;
				voice.pos = region.loop_start + std::fmod(voice.pos - region.loop_start, length);
			}
			else if (valid_loop && region.loop_mode == LoopMode::PingPong)
			{
				const double start = region.loop_start;
				const double span = static_cast<double>(region.loop_end - 1) - start;
				const double period = span * 2.0;
				double phase = std::fmod(voice.pos - start, period);
				if (phase < 0.0) phase += period;
				voice.loop_dir_fwd = phase <= span;
				voice.pos = start + (voice.loop_dir_fwd ? phase : period - phase);
			}
			else if (!valid_loop && voice.pos >= region.pcm_len)
				voice = Voice{};
		}
	}
	stats_.rendered_frames += frames;
}

} // namespace safsyn
