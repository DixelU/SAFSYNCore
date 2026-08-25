#include "core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <vector>

namespace
{
int failures = 0;
constexpr double pi = 3.1415926535897932384626433832795;

void check(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << '\n';
		++failures;
	}
}

safsyn::Soundfont make_bank(const std::vector<int16_t>& samples, uint32_t sample_rate,
	uint8_t channels = 1, bool loop = false, uint32_t loop_start = 0,
	uint32_t loop_end = 0)
{
	safsyn::Soundfont bank;
	bank.sfz_pcm.push_back(samples);
	safsyn::SampleRegion region;
	region.logical_sample_id = 0x5048415345ULL;
	region.pcm = bank.sfz_pcm.back().data();
	region.pcm_len = static_cast<uint32_t>(samples.size() / channels);
	region.sample_rate = sample_rate;
	region.channels = channels;
	region.root_key = 60;
	region.lo_key = 60;
	region.hi_key = 62;
	region.attack = 0.0f;
	region.hold = 0.0f;
	region.decay = 0.0f;
	region.sustain = 1.0f;
	region.release = 0.0f;
	if (loop)
	{
		region.loop_mode = safsyn::LoopMode::Forward;
		region.loop_start = loop_start;
		region.loop_end = loop_end;
	}
	bank.regions.push_back(region);
	return bank;
}

std::vector<int16_t> make_signal(size_t frames, uint8_t channels = 1)
{
	std::vector<int16_t> samples(frames * channels);
	for (size_t frame = 0; frame < frames; ++frame)
	{
		const double value = 6000.0 * std::sin(2.0 * pi * 5.0 * frame / frames) +
			2500.0 * std::sin(2.0 * pi * 11.0 * frame / frames + 0.31) +
			700.0 + (frame % 2 == 0 ? 900.0 : -900.0);
		samples[frame * channels] = static_cast<int16_t>(std::llround(value));
		if (channels == 2)
			samples[frame * 2 + 1] = samples[frame * 2];
	}
	return samples;
}

std::vector<float> render(safsyn::SynthEngine& engine, uint32_t frames,
	uint32_t first_block = 0)
{
	std::vector<float> audio(static_cast<size_t>(frames) * 2);
	if (first_block == 0 || first_block >= frames)
		engine.render_audio(audio.data(), frames);
	else
	{
		engine.render_audio(audio.data(), first_block);
		engine.render_audio(audio.data() + static_cast<size_t>(first_block) * 2,
			frames - first_block);
	}
	return audio;
}

std::vector<double> left_channel(const std::vector<float>& audio)
{
	std::vector<double> result(audio.size() / 2);
	for (size_t frame = 0; frame < result.size(); ++frame)
		result[frame] = audio[frame * 2];
	return result;
}

std::vector<double> dft_magnitudes(const std::vector<double>& input)
{
	std::vector<double> result(input.size() / 2 + 1);
	for (size_t bin = 0; bin < result.size(); ++bin)
	{
		std::complex<double> value;
		for (size_t index = 0; index < input.size(); ++index)
		{
			const double angle = -2.0 * pi * bin * index / input.size();
			value += input[index] * std::complex<double>(std::cos(angle), std::sin(angle));
		}
		result[bin] = std::abs(value);
	}
	return result;
}

safsyn::PhaseSettings settings(safsyn::PhaseMode mode, uint32_t pool = 8,
	uint64_t seed = 7)
{
	safsyn::PhaseSettings result;
	result.mode = mode;
	result.pool_size = pool;
	result.seed = seed;
	return result;
}

void test_strength_zero_is_coherent()
{
	auto bank = make_bank(make_signal(96), 9600);
	safsyn::SynthEngine coherent(9600, 4), zero(9600, 4);
	coherent.set_soundfont(&bank);
	zero.set_soundfont(&bank);
	auto phase = settings(safsyn::PhaseMode::IndependentBins, 8);
	phase.strength = 0.0f;
	zero.set_phase_settings(phase);
	coherent.note_on(0, 60, 100);
	zero.note_on(0, 60, 100);
	check(render(coherent, 96) == render(zero, 96),
		"phase strength zero is exactly the coherent path");
	check(zero.phase_cache_stats().cache_bytes == 0,
		"strength zero constructs no phase cache");
}

void test_determinism_and_block_invariance()
{
	auto bank = make_bank(make_signal(127), 12700);
	std::vector<safsyn::PhaseSettings> cases;
	for (const auto mode : {safsyn::PhaseMode::RandomPolarity, safsyn::PhaseMode::Analytic,
		safsyn::PhaseMode::SmoothField, safsyn::PhaseMode::IndependentBins})
		cases.push_back(settings(mode, 8, 99));
	auto continuous = settings(safsyn::PhaseMode::Analytic, 1, 99);
	continuous.continuous = true;
	cases.push_back(continuous);
	for (const auto& phase : cases)
	{
		safsyn::SynthEngine whole(12700, 8), split(12700, 8);
		whole.set_soundfont(&bank);
		split.set_soundfont(&bank);
		whole.set_phase_settings(phase);
		split.set_phase_settings(phase);
		whole.note_on(0, 60, 97);
		whole.note_on(0, 60, 97);
		split.note_on(0, 60, 97);
		split.note_on(0, 60, 97);
		check(render(whole, 127) == render(split, 127, 31),
			"phase output is bit-identical across render block sizes");
		check(whole.phase_cache_stats().cached_variants ==
			split.phase_cache_stats().cached_variants,
			"deterministic runs construct the same lazy variants");
	}
}

void test_pool_one_is_mutually_coherent()
{
	auto bank = make_bank(make_signal(128), 12800);
	safsyn::SynthEngine single(12800, 4), doubled(12800, 4);
	single.set_soundfont(&bank);
	doubled.set_soundfont(&bank);
	const auto phase = settings(safsyn::PhaseMode::Analytic, 1, 123);
	single.set_phase_settings(phase);
	doubled.set_phase_settings(phase);
	single.note_on(0, 60, 110);
	doubled.note_on(0, 60, 110);
	doubled.note_on(0, 60, 110);
	const auto one = render(single, 128);
	const auto two = render(doubled, 128);
	bool exact_double = true;
	for (size_t index = 0; index < one.size(); ++index)
		exact_double = exact_double && two[index] == one[index] * 2.0f;
	check(exact_double, "analytic pool size one keeps repeated triggers mutually coherent");
	check(doubled.phase_cache_stats().cached_variants == 1,
		"analytic pool one uses one transformed phase identity");
}

void test_pool_sizes_generate_distinct_variants()
{
	auto bank = make_bank(make_signal(32), 3200);
	uint64_t previous = 0;
	for (uint32_t pool : {8u, 32u, 64u})
	{
		safsyn::SynthEngine engine(3200, 1);
		engine.set_soundfont(&bank);
		engine.set_phase_settings(settings(safsyn::PhaseMode::IndependentBins, pool, 77));
		for (uint32_t event = 0; event < 2048; ++event)
			engine.note_on(0, 60, 100);
		const auto count = engine.phase_cache_stats().cached_variants;
		check(count > previous && count <= pool,
			"larger FFT pools deterministically expose more distinct variants");
		previous = count;
	}
	check(previous == 64, "pool size 64 reaches all deterministic variants");
}

void test_continuous_analytic_is_not_pool_quantized()
{
	auto bank = make_bank(make_signal(96), 9600);
	std::array<std::vector<float>, 3> outputs;
	for (size_t advance = 0; advance < outputs.size(); ++advance)
	{
		safsyn::SynthEngine engine(9600, 4);
		engine.set_soundfont(&bank);
		auto phase = settings(safsyn::PhaseMode::Analytic, 1, 42);
		phase.continuous = true;
		engine.set_phase_settings(phase);
		for (size_t dummy = 0; dummy < advance; ++dummy)
			engine.note_on(0, 59, 100); // advances stable event serial without a region
		engine.note_on(0, 60, 100);
		outputs[advance] = render(engine, 96);
		check(engine.phase_cache_stats().cached_variants == 0,
			"continuous analytic mode does not use finite cached angles");
	}
	check(outputs[0] != outputs[1] && outputs[1] != outputs[2] && outputs[0] != outputs[2],
		"continuous analytic angles vary beyond a pool of one");
}

void test_fft_magnitudes_and_dc_nyquist()
{
	auto bank = make_bank(make_signal(64), 6400);
	safsyn::SynthEngine coherent(6400, 2), changed(6400, 2);
	coherent.set_soundfont(&bank);
	changed.set_soundfont(&bank);
	changed.set_phase_settings(settings(safsyn::PhaseMode::IndependentBins, 1, 13));
	coherent.note_on(0, 60, 127);
	changed.note_on(0, 60, 127);
	const auto original_magnitude = dft_magnitudes(left_channel(render(coherent, 64)));
	const auto changed_magnitude = dft_magnitudes(left_channel(render(changed, 64)));
	double worst_relative = 0.0;
	const double significant = *std::max_element(original_magnitude.begin(),
		original_magnitude.end()) * 1e-5;
	for (size_t bin = 0; bin < original_magnitude.size(); ++bin)
		if (original_magnitude[bin] > significant)
			worst_relative = (std::max)(worst_relative,
				std::abs(changed_magnitude[bin] - original_magnitude[bin]) /
				original_magnitude[bin]);
	if (worst_relative >= 0.001)
		std::cerr << "FFT worst relative magnitude error: " << worst_relative << '\n';
	check(worst_relative < 0.001, "FFT phase variants preserve magnitudes within tolerance");
	check(std::abs(changed_magnitude.front() - original_magnitude.front()) < 0.00001 &&
		std::abs(changed_magnitude.back() - original_magnitude.back()) < 0.00001,
		"FFT variants preserve DC and Nyquist bins");
}

void test_attack_preservation()
{
	auto bank = make_bank(make_signal(64), 1000);
	safsyn::SynthEngine coherent(1000, 2), protected_phase(1000, 2);
	coherent.set_soundfont(&bank);
	protected_phase.set_soundfont(&bank);
	auto phase = settings(safsyn::PhaseMode::SmoothField, 1, 9);
	phase.preserve_attack_ms = 5.0f;
	protected_phase.set_phase_settings(phase);
	coherent.note_on(0, 60, 127);
	protected_phase.note_on(0, 60, 127);
	const auto original = render(coherent, 64);
	const auto changed = render(protected_phase, 64);
	check(std::equal(original.begin(), original.begin() + 10, changed.begin()),
		"attack preservation keeps the requested prefix exactly unchanged");
	check(!std::equal(original.begin() + 30, original.end(), changed.begin() + 30),
		"phase processing resumes after the protected attack transition");
}

void test_stereo_uses_one_phase_sheet()
{
	auto bank = make_bank(make_signal(64, 2), 6400, 2);
	safsyn::SynthEngine engine(6400, 2);
	engine.set_soundfont(&bank);
	engine.set_phase_settings(settings(safsyn::PhaseMode::SmoothField, 1, 54));
	engine.note_on(0, 60, 127);
	const auto audio = render(engine, 64);
	bool linked = true;
	float worst_ratio = 0.0f;
	for (size_t frame = 0; frame < 64; ++frame)
		if (std::abs(audio[frame * 2]) > 1e-6f)
		{
			worst_ratio = (std::max)(worst_ratio,
				std::abs(audio[frame * 2 + 1] / audio[frame * 2] - 1.0f));
			linked = linked && worst_ratio < 0.0002f;
		}
	if (!linked)
		std::cerr << "Stereo worst shared-sheet ratio error: " << worst_ratio << '\n';
	check(linked, "stereo partners receive the same FFT phase sheet");
}

void test_loop_seams_and_render_cache_stability()
{
	std::vector<int16_t> samples(64);
	for (size_t index = 0; index < samples.size(); ++index)
		samples[index] = static_cast<int16_t>(std::llround(
			10000.0 * std::sin(2.0 * pi * index / 16.0)));
	auto bank = make_bank(samples, 64, 1, true, 16, 48);
	for (const auto mode : {safsyn::PhaseMode::Analytic, safsyn::PhaseMode::SmoothField,
		safsyn::PhaseMode::IndependentBins})
	{
		safsyn::SynthEngine engine(64, 2);
		engine.set_soundfont(&bank);
		engine.set_phase_settings(settings(mode, 1, 5));
		engine.note_on(0, 60, 127);
		const auto before = engine.phase_cache_stats();
		const auto audio = render(engine, 144);
		const auto after = engine.phase_cache_stats();
		check(before.cache_bytes == after.cache_bytes &&
			before.cached_variants == after.cached_variants,
			"render_audio constructs no phase cache entries or variants");
		float largest_seam = 0.0f;
		for (size_t frame : {48u, 80u, 112u})
			largest_seam = (std::max)(largest_seam,
				std::abs(audio[frame * 2] - audio[(frame - 1) * 2]));
		check(largest_seam < 0.20f, "periodic phase representation keeps loop seams bounded");
	}
}

void test_voice_stealing_does_not_reassign_survivor_phases()
{
	auto bank = make_bank(make_signal(128), 12800);
	auto phase = settings(safsyn::PhaseMode::Analytic, 1, 222);
	phase.continuous = true;
	safsyn::SynthEngine stolen(12800, 2), explicit_stop(12800, 3);
	stolen.set_soundfont(&bank);
	explicit_stop.set_soundfont(&bank);
	stolen.set_phase_settings(phase);
	explicit_stop.set_phase_settings(phase);
	for (uint8_t note : {60, 61, 62})
	{
		stolen.note_on(0, note, 100);
		explicit_stop.note_on(0, note, 100);
	}
	explicit_stop.note_off(0, 60);
	check(stolen.stats().stolen_voices == 1, "voice-stealing fixture actually steals one voice");
	check(render(stolen, 96) == render(explicit_stop, 96),
		"voice stealing does not alter phases assigned to surviving events");
}

void test_modes_preserve_timing_and_voice_counts()
{
	auto bank = make_bank(make_signal(96), 9600);
	for (const auto mode : {safsyn::PhaseMode::Coherent, safsyn::PhaseMode::RandomPolarity,
		safsyn::PhaseMode::Analytic, safsyn::PhaseMode::SmoothField,
		safsyn::PhaseMode::IndependentBins})
	{
		safsyn::SynthEngine engine(9600, 8);
		engine.set_soundfont(&bank);
		engine.set_phase_settings(settings(mode, 8, 19));
		engine.note_on(0, 60, 100);
		engine.note_on(0, 61, 100);
		engine.note_on(0, 62, 100);
		render(engine, 96, 17);
		check(engine.stats().started_voices == 3 && engine.stats().rendered_frames == 96,
			"phase mode preserves dispatch count and rendered timestamps");
		check(engine.phase_cache_stats().failures == 0,
			"phase preprocessing succeeds without coherent fallback");
	}
}

void test_seed_changes_only_phase_assignment()
{
	auto bank = make_bank(make_signal(96), 9600);
	safsyn::SynthEngine first(9600, 8), second(9600, 8);
	first.set_soundfont(&bank);
	second.set_soundfont(&bank);
	auto first_phase = settings(safsyn::PhaseMode::Analytic, 1, 11);
	auto second_phase = settings(safsyn::PhaseMode::Analytic, 1, 12);
	first_phase.continuous = true;
	second_phase.continuous = true;
	first.set_phase_settings(first_phase);
	second.set_phase_settings(second_phase);
	for (uint8_t note : {60, 61, 62})
	{
		first.note_on(0, note, 100);
		second.note_on(0, note, 100);
	}
	const auto first_audio = render(first, 96, 17);
	const auto second_audio = render(second, 96, 31);
	check(first.stats().started_voices == second.stats().started_voices &&
		first.stats().rendered_frames == second.stats().rendered_frames,
		"changing phase seed does not alter dispatch count or timing");
	check(first_audio != second_audio,
		"changing phase seed changes deterministic phase assignment");
}
} // namespace

int main()
{
	test_strength_zero_is_coherent();
	test_determinism_and_block_invariance();
	test_pool_one_is_mutually_coherent();
	test_pool_sizes_generate_distinct_variants();
	test_continuous_analytic_is_not_pool_quantized();
	test_fft_magnitudes_and_dc_nyquist();
	test_attack_preservation();
	test_stereo_uses_one_phase_sheet();
	test_loop_seams_and_render_cache_stability();
	test_voice_stealing_does_not_reassign_survivor_phases();
	test_modes_preserve_timing_and_voice_counts();
	test_seed_changes_only_phase_assignment();
	if (failures != 0)
	{
		std::cerr << failures << " phase test(s) failed\n";
		return 1;
	}
	std::cout << "All phase policy tests passed\n";
	return 0;
}
