#include "core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace
{
int failures = 0;

void check(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << '\n';
		++failures;
	}
}

safsyn::Soundfont make_bank(uint32_t sample_rate, bool stereo = false,
	float attack = 0.0f, float release = 0.02f)
{
	safsyn::Soundfont bank;
	const uint32_t frames = 257;
	bank.sfz_pcm.emplace_back(static_cast<size_t>(frames) * (stereo ? 2 : 1));
	auto& pcm = bank.sfz_pcm.back();
	for (uint32_t frame = 0; frame < frames; ++frame)
	{
		const double phase = 2.0 * 3.14159265358979323846 * frame / 31.0;
		pcm[static_cast<size_t>(frame) * (stereo ? 2 : 1)] =
			static_cast<int16_t>(std::sin(phase) * 12000.0);
		if (stereo)
			pcm[static_cast<size_t>(frame) * 2 + 1] =
				static_cast<int16_t>(std::cos(phase * 0.73) * 9000.0);
	}
	safsyn::SampleRegion region;
	region.logical_sample_id = stereo ? 0x53544552454fULL : 0x4d4f4e4fULL;
	region.pcm = pcm.data();
	region.pcm_len = frames;
	region.sample_rate = sample_rate;
	region.channels = stereo ? 2 : 1;
	region.root_key = 60;
	region.loop_mode = safsyn::LoopMode::Forward;
	region.loop_start = 0;
	region.loop_end = frames;
	region.attack = attack;
	region.decay = 0.015f;
	region.sustain = 0.72f;
	region.release = release;
	bank.regions.push_back(region);
	return bank;
}

safsyn::Soundfont make_linked_stereo_bank(uint32_t sample_rate)
{
	safsyn::Soundfont bank;
	constexpr uint32_t frames = 257;
	bank.sfz_pcm.emplace_back(frames);
	bank.sfz_pcm.emplace_back(frames);
	for (uint32_t frame = 0; frame < frames; ++frame)
	{
		bank.sfz_pcm[0][frame] = static_cast<int16_t>(
			std::sin(frame * 0.19) * 11000.0);
		bank.sfz_pcm[1][frame] = static_cast<int16_t>(
			std::cos(frame * 0.13) * 7000.0);
	}
	safsyn::SampleRegion region;
	region.logical_sample_id = 0x4c494e4b4544ULL;
	region.pcm = bank.sfz_pcm[0].data();
	region.pcm_right = bank.sfz_pcm[1].data();
	region.pcm_len = frames;
	region.sample_rate = sample_rate;
	region.channels = 2;
	region.root_key = 60;
	region.loop_mode = safsyn::LoopMode::Forward;
	region.loop_start = 0;
	region.loop_end = frames;
	region.attack = 0.0f;
	region.sustain = 1.0f;
	region.release = 0.02f;
	bank.regions.push_back(region);
	return bank;
}

std::vector<float> render(safsyn::SynthEngine& engine, uint32_t frames,
	uint32_t block = 0)
{
	std::vector<float> audio(static_cast<size_t>(frames) * 2);
	if (block == 0)
		block = frames;
	uint32_t cursor = 0;
	while (cursor < frames)
	{
		const uint32_t count = (std::min)(block, frames - cursor);
		engine.render_audio(audio.data() + static_cast<size_t>(cursor) * 2, count);
		cursor += count;
	}
	return audio;
}

bool close_audio(const std::vector<float>& left, const std::vector<float>& right,
	double absolute = 2e-4, double relative = 2e-5)
{
	if (left.size() != right.size())
		return false;
	for (size_t index = 0; index < left.size(); ++index)
	{
		const double difference = std::abs(static_cast<double>(left[index]) - right[index]);
		const double scale = (std::max)(std::abs(static_cast<double>(left[index])),
			std::abs(static_cast<double>(right[index])));
		if (difference > absolute + relative * scale)
			return false;
	}
	return true;
}

void print_largest_error(const std::vector<float>& left, const std::vector<float>& right)
{
	size_t largest_index = 0;
	double largest = 0.0;
	for (size_t index = 0; index < (std::min)(left.size(), right.size()); ++index)
	{
		const double difference = std::abs(static_cast<double>(left[index]) - right[index]);
		if (difference > largest)
		{
			largest = difference;
			largest_index = index;
		}
	}
	std::cerr << "largest lifecycle error index=" << largest_index << " reference="
		<< left[largest_index] << " cohort=" << right[largest_index]
		<< " absolute=" << largest << '\n';
}

void configure_pair(safsyn::SynthEngine& reference, safsyn::SynthEngine& cohorts,
	const safsyn::Soundfont& bank, const safsyn::PhaseSettings& phase = {})
{
	reference.set_soundfont(&bank);
	reference.set_phase_settings(phase);
	cohorts.set_voice_model(safsyn::VoiceModel::Cohorts);
	cohorts.set_soundfont(&bank);
	cohorts.set_phase_settings(phase);
}

void test_duplicate_equivalence(safsyn::PhaseSettings phase, bool stereo,
	const char* message)
{
	constexpr uint32_t copies = 1000;
	auto bank = make_bank(4000, stereo);
	safsyn::SynthEngine reference(4000, copies + 8);
	safsyn::SynthEngine cohorts(4000, 8);
	configure_pair(reference, cohorts, bank, phase);
	for (uint32_t index = 0; index < copies; ++index)
		reference.note_on(0, 60, 100);
	cohorts.note_on_batch(0, 60, 100, copies);
	const auto expected = render(reference, 160);
	const auto actual = render(cohorts, 160);
	check(close_audio(expected, actual), message);
	check(cohorts.stats().logical_voices_started == copies &&
		cohorts.stats().logical_voices_merged == copies - 1 &&
		cohorts.stats().peak_active_logical_voices == copies &&
		cohorts.stats().peak_active_cohorts == 1 &&
		cohorts.stats().maximum_cohort_multiplicity == copies,
		"duplicate batch reports one physical cohort and all logical identities");
}

void test_coherent_and_phase_duplicates()
{
	test_duplicate_equivalence({}, false,
		"coherent cohort multiplicity matches generously sized individual voices");
	safsyn::PhaseSettings finite;
	finite.mode = safsyn::PhaseMode::Analytic;
	finite.pool_size = 32;
	finite.seed = 17;
	test_duplicate_equivalence(finite, false,
		"finite analytic coefficient sums match individual event phases");
	safsyn::PhaseSettings continuous = finite;
	continuous.continuous = true;
	continuous.preserve_attack_ms = 8.0f;
	test_duplicate_equivalence(continuous, true,
		"continuous stereo analytic sums and protected attacks match individual voices");
	safsyn::PhaseSettings polarity;
	polarity.mode = safsyn::PhaseMode::RandomPolarity;
	polarity.seed = 91;
	test_duplicate_equivalence(polarity, false,
		"random-polarity signed multiplicity matches individual voices");
}

void test_singleton_bit_exact_modes()
{
	auto bank = make_bank(3200, true);
	std::vector<safsyn::PhaseSettings> phases;
	for (const auto mode : {safsyn::PhaseMode::Coherent, safsyn::PhaseMode::RandomPolarity,
		safsyn::PhaseMode::Analytic, safsyn::PhaseMode::SmoothField,
		safsyn::PhaseMode::IndependentBins})
	{
		safsyn::PhaseSettings phase;
		phase.mode = mode;
		phase.seed = 44;
		phase.pool_size = 8;
		phases.push_back(phase);
	}
	phases[2].continuous = true;
	for (const auto& phase : phases)
	{
		safsyn::SynthEngine reference(3200, 4), cohort(3200, 4);
		configure_pair(reference, cohort, bank, phase);
		reference.note_on(0, 60, 99);
		cohort.note_on(0, 60, 99);
		const auto expected = render(reference, 96);
		const auto actual = render(cohort, 96);
		check(expected == actual,
			"an unmerged cohort preserves the exact individual phase arithmetic path");
	}
}

std::vector<float> lifecycle_render(safsyn::SynthEngine& engine, bool batched)
{
	std::vector<float> audio;
	auto append = [&](uint32_t frames, uint32_t block = 0) {
		auto part = render(engine, frames, block);
		audio.insert(audio.end(), part.begin(), part.end());
	};
	if (batched)
		engine.note_on_batch(0, 60, 110, 12);
	else
		for (uint32_t index = 0; index < 12; ++index) engine.note_on(0, 60, 110);
	append(7);
	if (batched) engine.note_off_batch(0, 60, 3);
	else for (uint32_t index = 0; index < 3; ++index) engine.note_off(0, 60);
	append(9);
	engine.control_change(0, 64, 127);
	if (batched) engine.note_off_batch(0, 60, 4);
	else for (uint32_t index = 0; index < 4; ++index) engine.note_off(0, 60);
	append(5);
	engine.control_change(0, 101, 0);
	engine.control_change(0, 100, 0);
	engine.control_change(0, 6, 12);
	engine.set_pitch_bend(0, 10240);
	engine.control_change(0, 10, 35);
	engine.control_change(0, 7, 103);
	engine.control_change(0, 11, 89);
	append(11);
	engine.control_change(0, 64, 0);
	append(35);
	if (batched) engine.note_off_batch(0, 60, 5);
	else for (uint32_t index = 0; index < 5; ++index) engine.note_off(0, 60);
	append(100);
	return audio;
}

void test_lifecycle_split_and_controls()
{
	auto bank = make_bank(2000, true, 0.02f, 0.025f);
	safsyn::PhaseSettings phase;
	phase.mode = safsyn::PhaseMode::Analytic;
	phase.continuous = true;
	phase.seed = 1234;
	phase.preserve_attack_ms = 12.0f;
	safsyn::SynthEngine reference(2000, 32);
	safsyn::SynthEngine cohorts(2000, 4);
	configure_pair(reference, cohorts, bank, phase);
	const auto expected = lifecycle_render(reference, false);
	const auto actual = lifecycle_render(cohorts, true);
	const bool equivalent = close_audio(expected, actual, 3e-4, 3e-5);
	if (!equivalent) print_largest_error(expected, actual);
	check(equivalent, "attack/decay note-offs, sustain release, pitch, pan, volume, and expression match");
	check(cohorts.stats().cohort_splits != 0 && cohorts.stats().cohorts_created > 1,
		"individual note-offs split release cohorts from sustaining multiplicity");
}

void test_controller_contract_equivalence()
{
	auto bank = make_bank(2000, true, 0.0f, 0.004f);
	safsyn::SynthEngine reference(2000, 64), cohorts(2000, 4);
	configure_pair(reference, cohorts, bank);
	std::vector<float> expected, actual;
	auto append = [&](uint32_t frames) {
		auto reference_part = render(reference, frames);
		auto cohort_part = render(cohorts, frames);
		expected.insert(expected.end(), reference_part.begin(), reference_part.end());
		actual.insert(actual.end(), cohort_part.begin(), cohort_part.end());
	};
	for (uint32_t index = 0; index < 12; ++index)
		reference.note_on(0, 60, 110);
	cohorts.note_on_batch(0, 60, 110, 12);
	reference.control_change(0, 7, 73); cohorts.control_change(0, 7, 73);
	reference.control_change(0, 39, 91); cohorts.control_change(0, 39, 91);
	reference.control_change(0, 11, 88); cohorts.control_change(0, 11, 88);
	reference.control_change(0, 43, 27); cohorts.control_change(0, 43, 27);
	reference.control_change(0, 10, 100); cohorts.control_change(0, 10, 100);
	reference.control_change(0, 42, 11); cohorts.control_change(0, 42, 11);
	reference.set_master_volume(12000); cohorts.set_master_volume(12000);
	append(17);
	reference.control_change(0, 64, 127); cohorts.control_change(0, 64, 127);
	reference.control_change(0, 123, 0); cohorts.control_change(0, 123, 0);
	append(9);
	reference.control_change(0, 64, 0); cohorts.control_change(0, 64, 0);
	append(12);
	for (uint32_t index = 0; index < 7; ++index)
		reference.note_on(0, 64, 100);
	cohorts.note_on_batch(0, 64, 100, 7);
	append(3);
	reference.control_change(0, 120, 0); cohorts.control_change(0, 120, 0);
	append(5);
	check(close_audio(expected, actual) && reference.active_voice_count() == 0 &&
		cohorts.active_voice_count() == 0 && cohorts.active_cohort_count() == 0,
		"14-bit controllers, master volume, sustained All Notes Off, and CC120 match cohorts");
}

void test_linked_stereo_and_one_shot()
{
	auto linked = make_linked_stereo_bank(3000);
	safsyn::PhaseSettings phase;
	phase.mode = safsyn::PhaseMode::Analytic;
	phase.continuous = true;
	phase.seed = 808;
	safsyn::SynthEngine reference(3000, 520), cohorts(3000, 4);
	configure_pair(reference, cohorts, linked, phase);
	for (uint32_t index = 0; index < 500; ++index) reference.note_on(0, 60, 101);
	cohorts.note_on_batch(0, 60, 101, 500);
	check(close_audio(render(reference, 96), render(cohorts, 96)),
		"linked planar stereo samples retain independent analytic coefficient sums");

	auto one_shot = make_bank(1000);
	one_shot.regions[0].loop_mode = safsyn::LoopMode::OneShot;
	safsyn::SynthEngine one_reference(1000, 16), one_cohorts(1000, 2);
	configure_pair(one_reference, one_cohorts, one_shot);
	for (uint32_t index = 0; index < 5; ++index) one_reference.note_on(0, 60, 100);
	one_cohorts.note_on_batch(0, 60, 100, 5);
	auto before_reference = render(one_reference, 8);
	auto before_cohorts = render(one_cohorts, 8);
	for (uint32_t index = 0; index < 5; ++index) one_reference.note_off(0, 60);
	one_cohorts.note_off_batch(0, 60, 5);
	auto after_reference = render(one_reference, 80);
	auto after_cohorts = render(one_cohorts, 80);
	before_reference.insert(before_reference.end(), after_reference.begin(), after_reference.end());
	before_cohorts.insert(before_cohorts.end(), after_cohorts.begin(), after_cohorts.end());
	check(close_audio(before_reference, before_cohorts),
		"one-shot cohorts ignore note-off and continue sample playback like individual voices");

	auto layered = make_bank(1000);
	layered.regions[0].hi_vel = 100;
	auto one_shot_region = layered.regions[0];
	one_shot_region.logical_sample_id = 0x4f4e4553484f54ULL;
	one_shot_region.lo_vel = 101;
	one_shot_region.hi_vel = 127;
	one_shot_region.loop_mode = safsyn::LoopMode::OneShot;
	layered.regions.push_back(one_shot_region);
	safsyn::SynthEngine layered_reference(1000, 8), layered_cohorts(1000, 2);
	configure_pair(layered_reference, layered_cohorts, layered);
	layered_reference.note_on(0, 60, 90);
	layered_cohorts.note_on(0, 60, 90);
	layered_reference.note_on(0, 60, 120);
	layered_cohorts.note_on(0, 60, 120);
	auto layered_expected = render(layered_reference, 8);
	auto layered_actual = render(layered_cohorts, 8);
	layered_reference.note_off(0, 60);
	layered_reference.note_off(0, 60);
	layered_cohorts.note_off_batch(0, 60, 2);
	auto blocked_expected = render(layered_reference, 16);
	auto blocked_actual = render(layered_cohorts, 16);
	layered_expected.insert(layered_expected.end(), blocked_expected.begin(), blocked_expected.end());
	layered_actual.insert(layered_actual.end(), blocked_actual.begin(), blocked_actual.end());
	auto expired_expected = render(layered_reference, 260);
	auto expired_actual = render(layered_cohorts, 260);
	layered_expected.insert(layered_expected.end(), expired_expected.begin(), expired_expected.end());
	layered_actual.insert(layered_actual.end(), expired_actual.begin(), expired_actual.end());
	layered_reference.note_off(0, 60);
	layered_cohorts.note_off(0, 60);
	auto released_expected = render(layered_reference, 40);
	auto released_actual = render(layered_cohorts, 40);
	layered_expected.insert(layered_expected.end(), released_expected.begin(), released_expected.end());
	layered_actual.insert(layered_actual.end(), released_actual.begin(), released_actual.end());
	check(close_audio(layered_expected, layered_actual),
		"newer one-shot identities block older same-note LIFO releases until sample completion");
}

void test_cross_run_merging_and_dynamic_growth()
{
	auto bank = make_bank(1000);
	safsyn::SynthEngine merged(1000, 2);
	merged.set_voice_model(safsyn::VoiceModel::Cohorts);
	merged.set_soundfont(&bank);
	merged.note_on_batch(0, 60, 100, 3);
	merged.note_on_batch(0, 61, 100, 2);
	merged.note_on_batch(0, 60, 100, 4);
	check(merged.active_voice_count() == 9 && merged.active_cohort_count() == 2 &&
		merged.stats().logical_voices_merged == 7,
		"compatible cohorts merge across interleaved same-sample note runs");
	merged.note_off_batch(0, 60, 7);
	check(merged.active_cohort_count() == 2 && merged.stats().cohort_merges >= 8,
		"simultaneous releases from separate logical batches join one exact release cohort");

	safsyn::SynthEngine dynamic(1000, 1);
	dynamic.set_voice_model(safsyn::VoiceModel::Cohorts);
	dynamic.set_soundfont(&bank);
	for (uint32_t index = 0; index < 1024; ++index)
	{
		dynamic.control_change(0, 1, static_cast<uint8_t>(index & 0x7f));
		dynamic.note_on(0, static_cast<uint8_t>(index & 0x7f),
			static_cast<uint8_t>(64 + (index & 0x3f)));
	}
	check(dynamic.active_cohort_count() == 1024 && dynamic.active_voice_count() == 1024 &&
		dynamic.stats().stolen_voices == 0 && dynamic.stats().cohort_capacity_steals == 0,
		"offline cohorts grow beyond the constructor reserve without implicit stealing");
}

void test_block_and_seed_determinism()
{
	auto bank = make_bank(3200, true);
	safsyn::PhaseSettings phase;
	phase.mode = safsyn::PhaseMode::Analytic;
	phase.continuous = true;
	phase.seed = 55;
	safsyn::SynthEngine whole(3200, 4), split(3200, 4), repeat(3200, 4);
	for (auto* engine : {&whole, &split, &repeat})
	{
		engine->set_voice_model(safsyn::VoiceModel::Cohorts);
		engine->set_soundfont(&bank);
		engine->set_phase_settings(phase);
		engine->note_on_batch(0, 60, 100, 257);
	}
	const auto a = render(whole, 193);
	const auto b = render(split, 193, 7);
	const auto c = render(repeat, 193);
	check(a == b && a == c,
		"cohort output is bit-identical across block sizes and repeated deterministic seeds");
}

void test_safety_limit_stealing()
{
	auto bank = make_bank(1000);
	safsyn::SynthEngine first(1000, 2), second(1000, 2);
	for (auto* engine : {&first, &second})
	{
		engine->set_voice_model(safsyn::VoiceModel::Cohorts, 1);
		engine->set_soundfont(&bank);
		engine->note_on_batch(0, 60, 100, 8);
		engine->note_on_batch(0, 64, 100, 5);
	}
	const auto first_audio = render(first, 32);
	const auto second_audio = render(second, 32);
	check(first_audio == second_audio && first.stats().cohort_capacity_steals == 1 &&
		first.stats().stolen_voices == 8 && first.active_cohort_count() == 1 &&
		first.active_voice_count() == 5,
		"explicit cohort ceiling steals deterministically and accounts logical multiplicity");
}

void test_parallel_cohort_render()
{
	constexpr uint32_t cohort_count = 1024;
	auto bank = make_bank(4000, true);
	safsyn::SynthEngine scalar(4000, cohort_count), parallel(4000, cohort_count),
		repeat(4000, cohort_count);
	for (auto* engine : {&scalar, &parallel, &repeat})
	{
		engine->set_voice_model(safsyn::VoiceModel::Cohorts, cohort_count);
		engine->set_soundfont(&bank);
	}
	parallel.set_render_threads(4);
	repeat.set_render_threads(4);
	for (uint32_t index = 0; index < cohort_count; ++index)
	{
		for (auto* engine : {&scalar, &parallel, &repeat})
		{
			engine->control_change(0, 1, static_cast<uint8_t>(index & 0x7f));
			engine->note_on(0, static_cast<uint8_t>(index & 0x7f),
				static_cast<uint8_t>(64 + (index & 0x3f)));
		}
	}
	const auto expected = render(scalar, 27, 9);
	const auto actual = render(parallel, 27, 9);
	const auto repeated = render(repeat, 27, 9);
	check(close_audio(expected, actual) && actual == repeated &&
		parallel.render_threads() == 4 &&
		parallel.stats().parallel_render_calls == 3 &&
		parallel.stats().parallel_rendered_frames == 27,
		"persistent threaded cohort rendering matches scalar output within tolerance");
	scalar.control_change(0, 120, 0);
	parallel.control_change(0, 120, 0);
	repeat.control_change(0, 120, 0);
	check(scalar.active_voice_count() == 0 && parallel.active_voice_count() == 0 &&
		repeat.active_voice_count() == 0,
		"threaded rendering retires and clears cohorts on the serial owner thread");
}
}

int main()
{
	test_coherent_and_phase_duplicates();
	test_singleton_bit_exact_modes();
	test_lifecycle_split_and_controls();
	test_controller_contract_equivalence();
	test_linked_stereo_and_one_shot();
	test_cross_run_merging_and_dynamic_growth();
	test_block_and_seed_determinism();
	test_safety_limit_stealing();
	test_parallel_cohort_render();
	if (failures != 0)
	{
		std::cerr << failures << " cohort test(s) failed\n";
		return 1;
	}
	std::cout << "All logical-voice cohort tests passed\n";
	return 0;
}
