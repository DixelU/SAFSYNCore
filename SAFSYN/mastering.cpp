#include "mastering.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace safsyn
{

bool mastering_settings_valid(const MasteringSettings& settings) noexcept
{
	return std::isfinite(settings.output_gain_db) &&
		settings.output_gain_db >= -120.0 && settings.output_gain_db <= 60.0 &&
		std::isfinite(settings.limiter_ceiling_db) &&
		settings.limiter_ceiling_db >= -60.0 && settings.limiter_ceiling_db <= 0.0 &&
		std::isfinite(settings.limiter_lookahead_ms) &&
		settings.limiter_lookahead_ms >= 0.0 && settings.limiter_lookahead_ms <= 100.0 &&
		std::isfinite(settings.limiter_release_ms) &&
		settings.limiter_release_ms >= 1.0 && settings.limiter_release_ms <= 5000.0;
}

StereoMasteringProcessor::StereoMasteringProcessor(uint32_t sample_rate,
	const MasteringSettings& settings) : settings_(settings)
{
	if (sample_rate == 0 || !mastering_settings_valid(settings))
		throw std::invalid_argument("invalid mastering settings");
	stats_.output_gain_linear = std::pow(10.0, settings.output_gain_db / 20.0);
	if (settings.limiter_enabled)
	{
		ceiling_ = std::pow(10.0, settings.limiter_ceiling_db / 20.0);
		stats_.lookahead_frames = static_cast<uint64_t>(std::llround(
			settings.limiter_lookahead_ms * sample_rate / 1000.0));
		const double release_frames = settings.limiter_release_ms * sample_rate / 1000.0;
		release_coefficient_ = std::exp(-1.0 / release_frames);
	}
}

void StereoMasteringProcessor::push_frame(float left, float right,
	std::vector<float>& output)
{
	const float gained_left = static_cast<float>(
		static_cast<double>(left) * stats_.output_gain_linear);
	const float gained_right = static_cast<float>(
		static_cast<double>(right) * stats_.output_gain_linear);
	const double peak = (std::max)(std::abs(static_cast<double>(gained_left)),
		std::abs(static_cast<double>(gained_right)));
	const uint64_t index = next_index_++;
	pending_.push_back({gained_left, gained_right, peak, index});
	while (!maximums_.empty() && maximums_.back().second <= peak)
		maximums_.pop_back();
	maximums_.push_back({index, peak});
	if (pending_.size() > stats_.lookahead_frames)
		emit_oldest(output);
}

void StereoMasteringProcessor::emit_oldest(std::vector<float>& output)
{
	if (pending_.empty())
		return;
	double desired_gain = 1.0;
	if (settings_.limiter_enabled && !maximums_.empty() && maximums_.front().second > ceiling_)
		desired_gain = ceiling_ / maximums_.front().second;
	if (desired_gain < limiter_gain_)
		limiter_gain_ = desired_gain;
	else if (settings_.limiter_enabled)
		limiter_gain_ = desired_gain +
			release_coefficient_ * (limiter_gain_ - desired_gain);
	limiter_gain_ = (std::min)(limiter_gain_, desired_gain);

	const Frame frame = pending_.front();
	output.push_back(static_cast<float>(static_cast<double>(frame.left) * limiter_gain_));
	output.push_back(static_cast<float>(static_cast<double>(frame.right) * limiter_gain_));
	stats_.minimum_limiter_gain = (std::min)(stats_.minimum_limiter_gain, limiter_gain_);
	if (limiter_gain_ < 1.0 - 1e-12)
		++stats_.limited_frames;
	pending_.pop_front();
	if (!maximums_.empty() && maximums_.front().first == frame.index)
		maximums_.pop_front();
}

void StereoMasteringProcessor::process(const float* interleaved_stereo, uint32_t frames,
	std::vector<float>& output)
{
	if (finished_)
		throw std::logic_error("mastering processor already finished");
	if (frames != 0 && !interleaved_stereo)
		throw std::invalid_argument("null mastering input");
	for (uint32_t frame = 0; frame < frames; ++frame)
		push_frame(interleaved_stereo[static_cast<size_t>(frame) * 2],
			interleaved_stereo[static_cast<size_t>(frame) * 2 + 1], output);
}

void StereoMasteringProcessor::finish(std::vector<float>& output)
{
	if (finished_)
		return;
	while (!pending_.empty())
		emit_oldest(output);
	finished_ = true;
	stats_.maximum_gain_reduction_db = stats_.minimum_limiter_gain >= 1.0 ? 0.0 :
		(stats_.minimum_limiter_gain <= 0.0 ? 300.0 :
			-20.0 * std::log10(stats_.minimum_limiter_gain));
}

} // namespace safsyn
