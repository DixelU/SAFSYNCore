#include "core.h"

#include <algorithm>
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
}

SynthEngine::SynthEngine(uint32_t sample_rate, size_t voice_capacity)
	: voices_((std::max)(voice_capacity, size_t{1})),
	  sample_rate_((std::max)(sample_rate, uint32_t{1}))
{
	reset();
}

size_t SynthEngine::active_voice_count() const noexcept
{
	return static_cast<size_t>(std::count_if(voices_.begin(), voices_.end(),
		[](const Voice& voice) { return voice.active(); }));
}

void SynthEngine::set_soundfont(const Soundfont* soundfont) noexcept
{
	silence_all();
	soundfont_ = soundfont;
}

void SynthEngine::reset() noexcept
{
	silence_all();
	for (auto& channel : channels_)
		channel = ChannelState{};
	next_serial_ = 1;
	stats_ = {};
}

void SynthEngine::silence_all() noexcept
{
	for (auto& voice : voices_)
		voice = Voice{};
}

SynthEngine::Voice* SynthEngine::allocate_voice() noexcept
{
	for (auto& voice : voices_)
		if (!voice.active())
			return &voice;

	auto candidate = voices_.begin();
	for (auto it = voices_.begin(); it != voices_.end(); ++it)
	{
		const bool it_releasing = it->stage == Voice::Stage::Release;
		const bool candidate_releasing = candidate->stage == Voice::Stage::Release;
		if ((it_releasing && !candidate_releasing) ||
			(it_releasing == candidate_releasing &&
				(it->env < candidate->env ||
					(it->env == candidate->env && it->serial < candidate->serial))))
			candidate = it;
	}
	++stats_.stolen_voices;
	return &*candidate;
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
	const float base_gain = region.attenuation * (velocity / 127.0f);
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
		(static_cast<uint16_t>(channel_state.bank_msb) << 7) | channel_state.bank_lsb);
	const auto& render_regions = all_regions_mode_ && !soundfont_->stress_regions.empty()
		? soundfont_->stress_regions : soundfont_->regions;
	for (const auto& region : render_regions)
	{
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

		Voice* voice = allocate_voice();
		*voice = Voice{};
		voice->region = &region;
		voice->note = note;
		voice->velocity = velocity;
		voice->channel = channel;
		voice->inc = compute_increment(region, note, channels_[channel].pitch_bend_semitones);
		voice->serial = serial;
		compute_gains(region, channel, velocity, voice->gain_l, voice->gain_r);
		begin_envelope(*voice);
		++stats_.started_voices;
	}
	stats_.peak_active_voices = (std::max)(stats_.peak_active_voices, active_voice_count());
}

void SynthEngine::note_off(uint8_t channel, uint8_t note) noexcept
{
	if (channel >= 16)
		return;
	uint64_t newest_serial = 0;
	for (const auto& voice : voices_)
		if (voice.active() && voice.channel == channel && voice.note == note)
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

void SynthEngine::update_channel_voice_gains(uint8_t channel) noexcept
{
	for (auto& voice : voices_)
		if (voice.active() && voice.channel == channel)
			compute_gains(*voice.region, channel, voice.velocity, voice.gain_l, voice.gain_r);
}

void SynthEngine::update_channel_pitch(uint8_t channel) noexcept
{
	for (auto& voice : voices_)
		if (voice.active() && voice.channel == channel)
			voice.inc = compute_increment(*voice.region, voice.note,
				channels_[channel].pitch_bend_semitones);
}

void SynthEngine::all_notes_off(uint8_t channel) noexcept
{
	for (auto& voice : voices_)
		if (voice.active() && voice.channel == channel &&
			voice.region->loop_mode != LoopMode::OneShot)
			begin_release(voice);
}

void SynthEngine::control_change(uint8_t channel, uint8_t controller, uint8_t value) noexcept
{
	if (channel >= 16)
		return;
	auto& state = channels_[channel];
	switch (controller)
	{
	case 0:
		state.bank_msb = value;
		break;
	case 7:
		state.volume = value / 127.0f;
		break;
	case 10:
		state.pan = std::clamp((static_cast<int>(value) - 64) / 63.0f, -1.0f, 1.0f);
		update_channel_voice_gains(channel);
		break;
	case 11:
		state.expression = value / 127.0f;
		break;
	case 32:
		state.bank_lsb = value;
		break;
	case 64:
	{
		const bool was_down = state.sustain_pedal;
		state.sustain_pedal = value >= 64;
		if (was_down && !state.sustain_pedal)
			for (auto& voice : voices_)
				if (voice.active() && voice.channel == channel && voice.note_off_pending)
					begin_release(voice);
		break;
	}
	case 121:
	{
		const bool release_pending = state.sustain_pedal;
		const uint8_t bank_msb = state.bank_msb;
		const uint8_t bank_lsb = state.bank_lsb;
		const uint8_t program = state.program;
		state = ChannelState{};
		state.bank_msb = bank_msb;
		state.bank_lsb = bank_lsb;
		state.program = program;
		if (release_pending)
			for (auto& voice : voices_)
				if (voice.active() && voice.channel == channel && voice.note_off_pending)
					begin_release(voice);
		update_channel_voice_gains(channel);
		update_channel_pitch(channel);
		break;
	}
	case 123:
		all_notes_off(channel);
		break;
	default:
		break;
	}
}

void SynthEngine::program_change(uint8_t channel, uint8_t program) noexcept
{
	if (channel < 16)
		channels_[channel].program = static_cast<uint8_t>((std::min)(program, uint8_t{127}));
}

void SynthEngine::set_pitch_bend(uint8_t channel, uint16_t value14) noexcept
{
	if (channel >= 16)
		return;
	value14 = (std::min)(value14, uint16_t{16383});
	channels_[channel].pitch_bend_semitones =
		(static_cast<int>(value14) - 8192) / 8192.0f * 2.0f;
	update_channel_pitch(channel);
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

void SynthEngine::render_audio(float* out, uint32_t frames) noexcept
{
	if (!out || frames == 0)
		return;
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
			const float source_l0 = pcm_to_float(region.pcm[offset0]);
			const float source_l1 = pcm_to_float(region.pcm[offset1]);
			const float sample_l = source_l0 + (source_l1 - source_l0) * fraction;
			float sample_r = sample_l;
			if (region.channels == 2)
			{
				const float source_r0 = pcm_to_float(region.pcm_right ?
					region.pcm_right[index0] : region.pcm[offset0 + 1]);
				const float source_r1 = pcm_to_float(region.pcm_right ?
					region.pcm_right[index1] : region.pcm[offset1 + 1]);
				sample_r = source_r0 + (source_r1 - source_r0) * fraction;
			}

			const ChannelState& channel = channels_[voice.channel];
			const float amplitude = envelope * channel.volume * channel.expression;
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
