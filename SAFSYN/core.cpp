#ifdef _WIN32
#include "pch.h"
#endif
#include "core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace safsyn
{

// ============================================================
//  Shared helpers
// ============================================================

static float timecents_to_sec(int tc)
{
	// SF2 spec: t = 2^(tc/1200).  tc=-12000 → ~0.001 s (effectively instantaneous).
	return std::pow(2.0f, tc / 1200.0f);
}

static float cb_to_linear(int cb)
{
	// Centibels: 0 = full gain; positive values attenuate.
	return std::pow(10.0f, -cb / 200.0f);
}

// ============================================================
//  SF2 raw structs  (must be 1-byte packed)
// ============================================================

#pragma pack(push, 1)

struct SF2Phdr
{	       // 38 bytes
	char     name[20];
	uint16_t preset;
	uint16_t bank;
	uint16_t bag_idx;
	uint32_t library, genre, morphology;
};
struct SF2Bag { uint16_t gen_idx, mod_idx; };  //  4 bytes
struct SF2Gen
{				 //  4 bytes
	uint16_t oper;
	union
	{
		struct { uint8_t lo, hi; } range;
		int16_t  s16;
		uint16_t u16;
	} amount;
};
struct SF2Inst { char name[20]; uint16_t bag_idx; };  // 22 bytes
struct SF2Shdr
{	       // 46 bytes
	char     name[20];
	uint32_t start, end, loop_start, loop_end;
	uint32_t sample_rate;
	uint8_t  pitch;
	int8_t   pitch_correction;
	uint16_t sample_link, sample_type;
};

#pragma pack(pop)

// Generator IDs used when building regions
enum : uint16_t
{
	GEN_StartAddrsOffset = 0,
	GEN_EndAddrsOffset = 1,
	GEN_StartloopAddrsOffset = 2,
	GEN_EndloopAddrsOffset = 3,
	GEN_StartAddrsCoarse = 4,
	GEN_EndAddrsCoarse = 12,
	GEN_Pan = 17,
	GEN_AttackVolEnv = 34,
	GEN_HoldVolEnv = 35,
	GEN_DecayVolEnv = 36,
	GEN_SustainVolEnv = 37,
	GEN_ReleaseVolEnv = 38,
	GEN_Instrument = 41,
	GEN_KeyRange = 43,
	GEN_VelRange = 44,
	GEN_StartloopCoarse = 45,
	GEN_InitialAttenuation = 48,
	GEN_EndloopCoarse = 50,
	GEN_CoarseTune = 51,
	GEN_FineTune = 52,
	GEN_SampleID = 53,
	GEN_SampleModes = 54,
	GEN_ScaleTuning = 56,
	GEN_ExclusiveClass = 57,
	GEN_OverridingRootKey = 58,
	GEN_COUNT = 61,
};

// Compact set of generators accumulated while walking the SF2 zone hierarchy
struct GenSet
{
	bool   present[GEN_COUNT] = {};
	SF2Gen gens[GEN_COUNT] = {};

	void apply(const SF2Gen& g)
	{
		if (g.oper < GEN_COUNT) { present[g.oper] = true; gens[g.oper] = g; }
	}
	// Fill in missing entries from a parent (global) zone
	void merge_defaults(const GenSet& parent)
	{
		for (int i = 0; i < GEN_COUNT; i++)
			if (!present[i] && parent.present[i]) { present[i] = true; gens[i] = parent.gens[i]; }
	}

	int16_t  s16(uint16_t id, int16_t  def = 0)   const { return (id < GEN_COUNT && present[id]) ? gens[id].amount.s16 : def; }
	uint16_t u16(uint16_t id, uint16_t def = 0)   const { return (id < GEN_COUNT && present[id]) ? gens[id].amount.u16 : def; }
	uint8_t  lo(uint16_t id, uint8_t  def = 0)   const { return (id < GEN_COUNT && present[id]) ? gens[id].amount.range.lo : def; }
	uint8_t  hi(uint16_t id, uint8_t  def = 127) const { return (id < GEN_COUNT && present[id]) ? gens[id].amount.range.hi : def; }
};

static int16_t combined_s16(const GenSet& preset, const GenSet& instrument,
	uint16_t id, int16_t instrument_default = 0)
{
	const int value = static_cast<int>(instrument.s16(id, instrument_default)) +
		static_cast<int>(preset.s16(id, 0));
	return static_cast<int16_t>(std::clamp(value, -32768, 32767));
}

static std::string sf2_name(const char* name, size_t size)
{
	size_t end = 0;
	while (end < size && name[end] != '\0')
		++end;
	while (end > 0 && name[end - 1] == ' ')
		--end;
	return std::string(name, end);
}

static bool build_seed_loader_region(const SF2Shdr& sh, const GenSet& generators,
	Soundfont& sf, uint32_t smpl_frames, SampleRegion& r)
{
	const int32_t s_ofs = generators.s16(GEN_StartAddrsOffset, 0) +
		generators.s16(GEN_StartAddrsCoarse, 0) * 32768;
	const int32_t e_ofs = generators.s16(GEN_EndAddrsOffset, 0) +
		generators.s16(GEN_EndAddrsCoarse, 0) * 32768;
	const int32_t ls_ofs = generators.s16(GEN_StartloopAddrsOffset, 0) +
		generators.s16(GEN_StartloopCoarse, 0) * 32768;
	const int32_t le_ofs = generators.s16(GEN_EndloopAddrsOffset, 0) +
		generators.s16(GEN_EndloopCoarse, 0) * 32768;

	const uint32_t abs_s = static_cast<uint32_t>(static_cast<int32_t>(sh.start) + s_ofs);
	uint32_t abs_e = static_cast<uint32_t>(static_cast<int32_t>(sh.end) + e_ofs);
	const uint32_t abs_ls = static_cast<uint32_t>(
		static_cast<int32_t>(sh.loop_start) + ls_ofs);
	const uint32_t abs_le = static_cast<uint32_t>(
		static_cast<int32_t>(sh.loop_end) + le_ofs);
	if (abs_e > smpl_frames)
		abs_e = smpl_frames;
	if (abs_s >= abs_e)
		return false;

	r.lo_key = generators.lo(GEN_KeyRange, 0);
	r.hi_key = generators.hi(GEN_KeyRange, 127);
	r.lo_vel = generators.lo(GEN_VelRange, 0);
	r.hi_vel = generators.hi(GEN_VelRange, 127);
	const int16_t root = generators.s16(GEN_OverridingRootKey, -1);
	r.root_key = root >= 0 && root <= 127 ? static_cast<uint8_t>(root) : sh.pitch;
	r.pcm = sf.pcm_pool.data() + abs_s;
	r.pcm_len = abs_e - abs_s;
	r.sample_rate = sh.sample_rate;
	r.channels = 1;

	const uint16_t modes = generators.u16(GEN_SampleModes, 0);
	if (((modes & 3) == 1 || (modes & 3) == 3) &&
		abs_ls >= abs_s && abs_le <= abs_e && abs_ls < abs_le)
	{
		r.loop_mode = (modes & 3) == 3 ? LoopMode::Sustain : LoopMode::Forward;
		r.loop_start = abs_ls - abs_s;
		r.loop_end = abs_le - abs_s;
	}

	r.coarse_tune = generators.s16(GEN_CoarseTune, 0);
	r.fine_tune = generators.s16(GEN_FineTune, 0) + sh.pitch_correction;
	r.scale_tuning = generators.u16(GEN_ScaleTuning, 100);
	r.attack = timecents_to_sec(generators.s16(GEN_AttackVolEnv, -12000));
	r.hold = timecents_to_sec(generators.s16(GEN_HoldVolEnv, -12000));
	r.decay = timecents_to_sec(generators.s16(GEN_DecayVolEnv, -12000));
	const int16_t sustain_cb = std::clamp<int16_t>(
		generators.s16(GEN_SustainVolEnv, 0), 0, 1000);
	r.sustain = cb_to_linear(sustain_cb);
	r.release = timecents_to_sec(generators.s16(GEN_ReleaseVolEnv, -12000));
	r.pan = generators.s16(GEN_Pan, 0) / 500.0f;
	r.attenuation = cb_to_linear(generators.s16(GEN_InitialAttenuation, 0));
	r.exclusive_class = generators.u16(GEN_ExclusiveClass, 0);
	return true;
}

// ============================================================
//  SF2 loader
// ============================================================

bool load_sf2(const char* path, Soundfont& sf)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;

	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	rewind(f);
	std::vector<uint8_t> buf((size_t)fsz);
	fread(buf.data(), 1, (size_t)fsz, f);
	fclose(f);

	if (fsz < 12) return false;
	if (memcmp(buf.data(), "RIFF", 4) != 0) return false;
	if (memcmp(buf.data() + 8, "sfbk", 4) != 0) return false;

	auto rd32 = [](const uint8_t* p) -> uint32_t
	{
		return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
	};

	// Walk top-level LIST chunks to locate sdta and pdta
	const uint8_t* sdta = nullptr, * pdta = nullptr;
	uint32_t sdta_sz = 0, pdta_sz = 0;
	{
		size_t pos = 12;
		while (pos + 8 <= (size_t)fsz)
		{
			const uint8_t* cp = buf.data() + pos;
			uint32_t csz = rd32(cp + 4);
			if (memcmp(cp, "LIST", 4) == 0 && pos + 12 <= (size_t)fsz)
			{
				if (memcmp(cp + 8, "sdta", 4) == 0) { sdta = cp + 12; sdta_sz = csz - 4; }
				if (memcmp(cp + 8, "pdta", 4) == 0) { pdta = cp + 12; pdta_sz = csz - 4; }
			}
			pos += 8 + csz + (csz & 1);
		}
	}
	if (!sdta || !pdta) return false;

	// Extract smpl chunk
	const int16_t* smpl = nullptr; uint32_t smpl_frames = 0;
	{
		size_t pos = 0;
		while (pos + 8 <= sdta_sz)
		{
			const uint8_t* cp = sdta + pos;
			uint32_t csz = rd32(cp + 4);
			if (memcmp(cp, "smpl", 4) == 0) { smpl = (const int16_t*)(cp + 8); smpl_frames = csz / 2; }
			pos += 8 + csz + (csz & 1);
		}
	}
	if (!smpl || smpl_frames == 0) return false;
	sf = Soundfont{};
	sf.pcm_pool.assign(smpl, smpl + smpl_frames);

	// Extract all pdta sub-chunks
	struct { const uint8_t* p; uint32_t sz; } C[7] = {};
	enum { PHDR, PBAG, PGEN, INST, IBAG, IGEN, SHDR };
	static const char* names[7] = {"phdr","pbag","pgen","inst","ibag","igen","shdr"};
	{
		size_t pos = 0;
		while (pos + 8 <= pdta_sz)
		{
			const uint8_t* cp = pdta + pos;
			uint32_t csz = rd32(cp + 4);
			for (int i = 0; i < 7; i++)
				if (memcmp(cp, names[i], 4) == 0) { C[i].p = cp + 8; C[i].sz = csz; }
			pos += 8 + csz + (csz & 1);
		}
	}
	for (int i = 0; i < 7; i++) if (!C[i].p) return false;

	auto n = [&](int i, size_t stride) -> size_t { return C[i].sz / stride; };
	auto PHDR_ = [&](size_t i) { return (const SF2Phdr*)(C[PHDR].p) + i; };
	auto PBAG_ = [&](size_t i) { return (const SF2Bag*)(C[PBAG].p) + i; };
	auto PGEN_ = [&](size_t i) { return (const SF2Gen*)(C[PGEN].p) + i; };
	auto INST_ = [&](size_t i) { return (const SF2Inst*)(C[INST].p) + i; };
	auto IBAG_ = [&](size_t i) { return (const SF2Bag*)(C[IBAG].p) + i; };
	auto IGEN_ = [&](size_t i) { return (const SF2Gen*)(C[IGEN].p) + i; };
	auto SHDR_ = [&](size_t i) { return (const SF2Shdr*)(C[SHDR].p) + i; };

	size_t nPhdr = n(PHDR, sizeof(SF2Phdr));
	size_t nPbag = n(PBAG, sizeof(SF2Bag));
	size_t nPgen = n(PGEN, sizeof(SF2Gen));
	size_t nInst = n(INST, sizeof(SF2Inst));
	size_t nIbag = n(IBAG, sizeof(SF2Bag));
	size_t nIgen = n(IGEN, sizeof(SF2Gen));
	size_t nShdr = n(SHDR, sizeof(SF2Shdr));

	// Build SampleRegions by walking preset → instrument → sample hierarchy
	for (size_t pi = 0; pi + 1 < nPhdr; pi++)
	{
		const SF2Phdr* preset_header = PHDR_(pi);
		PresetInfo preset;
		preset.bank = preset_header->bank;
		preset.program = preset_header->preset;
		preset.name = sf2_name(preset_header->name, sizeof(preset_header->name));
		preset.first_region = sf.regions.size();

		uint16_t pbag_lo = PHDR_(pi)->bag_idx;
		uint16_t pbag_hi = PHDR_(pi + 1)->bag_idx;
		bool     first_p = true;
		GenSet   preset_global;

		for (uint16_t pbi = pbag_lo; pbi < pbag_hi && (size_t)(pbi + 1) < nPbag; pbi++)
		{
			uint16_t pgen_lo = PBAG_(pbi)->gen_idx;
			uint16_t pgen_hi = PBAG_(pbi + 1)->gen_idx;

			GenSet pzone;
			int    inst_idx = -1;
			for (uint16_t gi = pgen_lo; gi < pgen_hi && gi < nPgen; gi++)
			{
				const SF2Gen* g = PGEN_(gi);
				if (g->oper == GEN_Instrument) inst_idx = g->amount.u16;
				else pzone.apply(*g);
			}
			if (inst_idx < 0) { if (first_p) preset_global = pzone; first_p = false; continue; }
			first_p = false;
			pzone.merge_defaults(preset_global);

			if ((size_t)inst_idx + 1 >= nInst) continue;
			uint16_t ibag_lo = INST_(inst_idx)->bag_idx;
			uint16_t ibag_hi = INST_(inst_idx + 1)->bag_idx;
			bool     first_i = true;
			GenSet   inst_global;

			for (uint16_t ibi = ibag_lo; ibi < ibag_hi && (size_t)(ibi + 1) < nIbag; ibi++)
			{
				uint16_t igen_lo = IBAG_(ibi)->gen_idx;
				uint16_t igen_hi = IBAG_(ibi + 1)->gen_idx;

				GenSet izone;
				int    shdr_idx = -1;
				for (uint16_t gi = igen_lo; gi < igen_hi && gi < nIgen; gi++)
				{
					const SF2Gen* g = IGEN_(gi);
					if (g->oper == GEN_SampleID) shdr_idx = g->amount.u16;
					else izone.apply(*g);
				}
				if (shdr_idx < 0) { if (first_i) inst_global = izone; first_i = false; continue; }
				first_i = false;

				if ((size_t)shdr_idx >= nShdr) continue;
				const SF2Shdr* sh = SHDR_(shdr_idx);

				// Skip ROM samples and right-channel stereo pairs (handled via left).
				if (sh->sample_type & 0x8000) continue;
				const uint16_t sample_type = sh->sample_type & 0x7fff;
				if (sample_type == 2) continue; // rightSample

				izone.merge_defaults(inst_global);
				SampleRegion seed_region;
				if (build_seed_loader_region(*sh, izone, sf, smpl_frames, seed_region))
					sf.stress_regions.push_back(seed_region);

				// Preset generator amounts are adjustments to instrument amounts.
				const int32_t s_ofs = combined_s16(pzone, izone, GEN_StartAddrsOffset) +
					static_cast<int32_t>(combined_s16(pzone, izone, GEN_StartAddrsCoarse)) * 32768;
				const int32_t e_ofs = combined_s16(pzone, izone, GEN_EndAddrsOffset) +
					static_cast<int32_t>(combined_s16(pzone, izone, GEN_EndAddrsCoarse)) * 32768;
				const int32_t ls_ofs = combined_s16(pzone, izone, GEN_StartloopAddrsOffset) +
					static_cast<int32_t>(combined_s16(pzone, izone, GEN_StartloopCoarse)) * 32768;
				const int32_t le_ofs = combined_s16(pzone, izone, GEN_EndloopAddrsOffset) +
					static_cast<int32_t>(combined_s16(pzone, izone, GEN_EndloopCoarse)) * 32768;

				uint32_t abs_s = (uint32_t)((int32_t)sh->start + s_ofs);
				uint32_t abs_e = (uint32_t)((int32_t)sh->end + e_ofs);
				uint32_t abs_ls = (uint32_t)((int32_t)sh->loop_start + ls_ofs);
				uint32_t abs_le = (uint32_t)((int32_t)sh->loop_end + le_ofs);

				if (abs_e > smpl_frames) abs_e = smpl_frames;
				if (abs_s >= abs_e) continue;

				SampleRegion r;
				r.preset_bank = preset.bank;
				r.preset_program = preset.program;
				r.lo_key = (std::max)(pzone.lo(GEN_KeyRange, 0), izone.lo(GEN_KeyRange, 0));
				r.hi_key = (std::min)(pzone.hi(GEN_KeyRange, 127), izone.hi(GEN_KeyRange, 127));
				r.lo_vel = (std::max)(pzone.lo(GEN_VelRange, 0), izone.lo(GEN_VelRange, 0));
				r.hi_vel = (std::min)(pzone.hi(GEN_VelRange, 127), izone.hi(GEN_VelRange, 127));
				if (r.lo_key > r.hi_key || r.lo_vel > r.hi_vel)
					continue;

				{
					int16_t ovr = izone.s16(GEN_OverridingRootKey, -1);
					r.root_key = (ovr >= 0 && ovr <= 127) ? (uint8_t)ovr : sh->pitch;
				}

				r.pcm_len = abs_e - abs_s;
				r.sample_rate = sh->sample_rate;
				r.channels = 1;
				r.pcm = sf.pcm_pool.data() + abs_s;
				if (sample_type == 4 && sh->sample_link < nShdr)
				{
					const SF2Shdr* linked = SHDR_(sh->sample_link);
					const uint16_t linked_type = linked->sample_type & 0x7fff;
					const int64_t linked_start = static_cast<int64_t>(linked->start) + s_ofs;
					const int64_t linked_end = static_cast<int64_t>(linked->end) + e_ofs;
					if (!(linked->sample_type & 0x8000) && linked_type == 2 &&
						linked->sample_link == shdr_idx && linked_start >= 0 &&
						linked_end > linked_start && linked_end <= smpl_frames &&
						linked->sample_rate == sh->sample_rate)
					{
						r.pcm_len = (std::min)(r.pcm_len,
							static_cast<uint32_t>(linked_end - linked_start));
						r.pcm_right = sf.pcm_pool.data() + linked_start;
						r.channels = 2;
					}
				}

				{
					uint16_t modes = izone.u16(GEN_SampleModes, 0);
					if ((modes & 3) == 1 || (modes & 3) == 3)
					{
						if (abs_ls >= abs_s && abs_le <= abs_e && abs_ls < abs_le)
						{
							r.loop_mode = (modes & 3) == 3 ? LoopMode::Sustain : LoopMode::Forward;
							r.loop_start = abs_ls - abs_s;
							r.loop_end = abs_le - abs_s;
						}
					}
				}

				r.coarse_tune = combined_s16(pzone, izone, GEN_CoarseTune);
				r.fine_tune = static_cast<int16_t>(std::clamp(
					static_cast<int>(combined_s16(pzone, izone, GEN_FineTune)) +
					sh->pitch_correction, -32768, 32767));
				r.scale_tuning = static_cast<uint16_t>(std::clamp(
					static_cast<int>(izone.s16(GEN_ScaleTuning, 100)) +
					static_cast<int>(pzone.s16(GEN_ScaleTuning, 0)), 0, 1200));

				r.attack = timecents_to_sec(std::clamp<int>(
					combined_s16(pzone, izone, GEN_AttackVolEnv, -12000), -12000, 8000));
				r.hold = timecents_to_sec(std::clamp<int>(
					combined_s16(pzone, izone, GEN_HoldVolEnv, -12000), -12000, 5000));
				r.decay = timecents_to_sec(std::clamp<int>(
					combined_s16(pzone, izone, GEN_DecayVolEnv, -12000), -12000, 8000));
				{
					int16_t sus_cb = static_cast<int16_t>(std::clamp<int>(
						combined_s16(pzone, izone, GEN_SustainVolEnv), 0, 1440));
					r.sustain = cb_to_linear(sus_cb);
				}
				r.release = timecents_to_sec(std::clamp<int>(
					combined_s16(pzone, izone, GEN_ReleaseVolEnv, -12000), -12000, 8000));

				const GenSet& pan_generators = r.channels == 2 ? inst_global : izone;
				r.pan = std::clamp(combined_s16(pzone, pan_generators, GEN_Pan) / 500.0f,
					-1.0f, 1.0f);
				r.attenuation = cb_to_linear(static_cast<int16_t>(std::clamp<int>(
					combined_s16(pzone, izone, GEN_InitialAttenuation), 0, 1440)));
				r.exclusive_class = izone.u16(GEN_ExclusiveClass, 0);

				sf.regions.push_back(r);
			}
		}
		preset.region_count = sf.regions.size() - preset.first_region;
		sf.presets.push_back(std::move(preset));
	}
	return !sf.regions.empty();
}

// ============================================================
//  WAV loader  (used by SFZ)
// ============================================================

static bool load_wav(
	const char* path,
	std::vector<int16_t>& out,
	uint32_t& out_rate,
	uint8_t& out_ch)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long fsz = ftell(f); rewind(f);
	std::vector<uint8_t> buf((size_t)fsz);
	fread(buf.data(), 1, (size_t)fsz, f);
	fclose(f);

	if (fsz < 44) return false;
	if (memcmp(buf.data(), "RIFF", 4) != 0) return false;
	if (memcmp(buf.data() + 8, "WAVE", 4) != 0) return false;

	auto u16 = [](const uint8_t* p) -> uint16_t { return p[0] | (uint16_t(p[1]) << 8); };
	auto u32 = [](const uint8_t* p) -> uint32_t
	{
		return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
	};

	uint16_t fmt_tag = 0, channels = 0, bps = 0;
	uint32_t sample_rate = 0;
	const uint8_t* data_ptr = nullptr; uint32_t data_sz = 0;

	size_t pos = 12;
	while (pos + 8 <= (size_t)fsz)
	{
		const uint8_t* cp = buf.data() + pos;
		uint32_t csz = u32(cp + 4);
		if (memcmp(cp, "fmt ", 4) == 0 && csz >= 16)
		{
			fmt_tag = u16(cp + 8);
			channels = u16(cp + 10);
			sample_rate = u32(cp + 12);
			bps = u16(cp + 22);
		}
		else if (memcmp(cp, "data", 4) == 0)
		{
			data_ptr = cp + 8; data_sz = csz;
		}
		pos += 8 + csz + (csz & 1);
	}

	if (!data_ptr || !sample_rate || channels == 0) return false;
	if (fmt_tag != 1 && fmt_tag != 3) return false; // PCM or IEEE float only

	out_rate = sample_rate;
	out_ch = (uint8_t)std::min<uint16_t>(channels, 2);

	uint32_t bytes_per_sample = bps / 8;
	uint32_t frame_stride = bytes_per_sample * channels;
	uint32_t frames = data_sz / frame_stride;
	out.resize((size_t)frames * out_ch);

	for (uint32_t fi = 0; fi < frames; fi++)
	{
		const uint8_t* fp = data_ptr + fi * frame_stride;
		for (uint8_t c = 0; c < out_ch; c++)
		{
			const uint8_t* sp = fp + c * bytes_per_sample;
			int16_t s = 0;
			if (fmt_tag == 1)
			{
				switch (bps)
				{
					case  8: s = (int16_t)(((int)*sp - 128) << 8); break;
					case 16: s = (int16_t)(sp[0] | (sp[1] << 8)); break;
					case 24:
					{
						int32_t v = sp[0] | (sp[1] << 8) | (sp[2] << 16);
						if (v & 0x800000) v |= ~0xFFFFFF;
						s = (int16_t)(v >> 8);
						break;
					}
					case 32:
					{
						int32_t v; memcpy(&v, sp, 4);
						s = (int16_t)(v >> 16);
						break;
					}
				}
			}
			else
			{ // IEEE float 32
				float fv; memcpy(&fv, sp, 4);
				fv = std::clamp(fv, -1.0f, 1.0f);
				s = (int16_t)(fv * 32767.0f);
			}
			out[fi * out_ch + c] = s;
		}
	}
	return true;
}

// ============================================================
//  SFZ parser
// ============================================================

// Converts SFZ note names (C4, C#4, Db4, 60) to MIDI number
static int sfz_note_to_midi(const std::string& s)
{
	if (s.empty()) return -1;
	if (std::isdigit((unsigned char)s[0]) || s[0] == '-')
		return std::stoi(s);
	static const char* NOTE = "C_D_EF_G_A_B";
	int semi = -1;

	for (int i = 0; i < 12; i++)
	{
		if (NOTE[i] != '_' && std::toupper((unsigned char)s[0]) == NOTE[i])
		{
			semi = i; break;
		}
	}

	if (semi < 0)
		return -1;

	size_t i = 1;
	if (i < s.size() && s[i] == '#')
	{
		semi++;
		i++;
	}
	else if (i < s.size() && std::toupper((unsigned char)s[i]) == 'B')
	{
		semi--;
		i++;
	}
	int octave = (i < s.size()) ? std::stoi(s.substr(i)) : 4;
	return (octave + 1) * 12 + semi;
}

static std::string sfz_parent_dir(const std::string& p)
{
	size_t n = p.find_last_of("/\\");
	return (n == std::string::npos) ? std::string(".") : p.substr(0, n);
}

// Parse one logical line: call header_cb("<region>") or opcode_cb("key","value")
// Correctly handles values with spaces (e.g. sample=my file.wav)
static void sfz_parse_line(std::string src,
	const std::function<void(std::string)>& header_cb,
	const std::function<void(std::string, std::string)>& opcode_cb)
{
	// Strip comment
	size_t c = src.find("//");
	if (c != std::string::npos) src.resize(c);

	size_t i = 0, n = src.size();
	while (i < n)
	{
		while (i < n && std::isspace((unsigned char)src[i])) i++;
		if (i >= n) break;

		if (src[i] == '<')
		{
			size_t e = src.find('>', i);
			if (e == std::string::npos) break;
			header_cb(src.substr(i + 1, e - i - 1));
			i = e + 1;
			continue;
		}

		// Scan key (up to '=')
		size_t key_s = i;
		while (i < n && src[i] != '=' && !std::isspace((unsigned char)src[i]) && src[i] != '<') i++;
		if (i >= n || src[i] != '=') { i++; continue; }
		std::string key = src.substr(key_s, i - key_s);
		i++; // skip '='

		size_t val_s = i;
		size_t val_e = n; // default: rest of line

		// Scan ahead for the next "word=" or "<" that would start the next token
		for (size_t j = i; j < n; )
		{
			if (src[j] == '<') { val_e = j; break; }
			if (std::isspace((unsigned char)src[j]))
			{
				// Skip whitespace to find potential next key
				size_t k = j;
				while (k < n && std::isspace((unsigned char)src[k])) k++;
				if (k >= n || src[k] == '<') { j = k; continue; }
				// Scan the next word
				size_t w = k;
				while (w < n && !std::isspace((unsigned char)src[w]) && src[w] != '=') w++;
				if (w < n && src[w] == '=')
				{
					val_e = j;   // value ends before this whitespace
					i = k;       // next iteration starts at next key
					break;
				}
				j = w;
			}
			else
			{
				j++;
			}
		}

		std::string val = src.substr(val_s, val_e - val_s);
		while (!val.empty() && std::isspace((unsigned char)val.back())) val.pop_back();
		if (!key.empty() && !val.empty())
			opcode_cb(key, val);

		i = val_e;
	}
}

bool load_sfz(const char* path, Soundfont& sf)
{
	std::ifstream file(path);
	if (!file.is_open()) return false;
	std::string dir = sfz_parent_dir(path);

	struct Desc
	{
		int   lo_key = 0, hi_key = 127;
		int   lo_vel = 0, hi_vel = 127;
		int   pitch_keycenter = 60;
		std::string sample;
		int   loop_mode = 0;  // 0=no_loop, 1=continuous, 2=one_shot, 3=sustain
		int   loop_start = 0, loop_end = 0;
		int   transpose = 0;
		float tune = 0.0f;
		int   scale_tuning = 100;
		float ampeg_attack = 0.001f;
		float ampeg_hold = 0.0f;
		float ampeg_decay = 0.0f;
		float ampeg_sustain = 100.0f;
		float ampeg_release = 0.05f;
		float volume = 0.0f;   // dB
		float pan = 0.0f;   // -100..100
		int   group = 0;
	};

	auto parse_opcode = [](Desc& d, const std::string& k, const std::string& v)
	{
		try
		{
			if (k == "sample")	  d.sample = v;
			else if (k == "lokey")	   d.lo_key = sfz_note_to_midi(v);
			else if (k == "hikey")	   d.hi_key = sfz_note_to_midi(v);
			else if (k == "lovel")	   d.lo_vel = std::stoi(v);
			else if (k == "hivel")	   d.hi_vel = std::stoi(v);
			else if (k == "key")	     d.lo_key = d.hi_key = d.pitch_keycenter = sfz_note_to_midi(v);
			else if (k == "pitch_keycenter") d.pitch_keycenter = sfz_note_to_midi(v);
			else if (k == "loop_mode")
			{
				if (v == "loop_continuous") d.loop_mode = 1;
				else if (v == "one_shot")	d.loop_mode = 2;
				else if (v == "loop_sustain")    d.loop_mode = 3;
				else			     d.loop_mode = 0;
			}
			else if (k == "loop_start" || k == "loopstart") d.loop_start = std::stoi(v);
			else if (k == "loop_end" || k == "loopend")   d.loop_end = std::stoi(v);
			else if (k == "transpose")       d.transpose = std::stoi(v);
			else if (k == "tune")	    d.tune = std::stof(v);
			else if (k == "scale_tuning")    d.scale_tuning = std::stoi(v);
			else if (k == "ampeg_attack")    d.ampeg_attack = std::stof(v);
			else if (k == "ampeg_hold")      d.ampeg_hold = std::stof(v);
			else if (k == "ampeg_decay")     d.ampeg_decay = std::stof(v);
			else if (k == "ampeg_sustain")   d.ampeg_sustain = std::stof(v);
			else if (k == "ampeg_release")   d.ampeg_release = std::stof(v);
			else if (k == "volume")	  d.volume = std::stof(v);
			else if (k == "pan")	     d.pan = std::stof(v);
			else if (k == "group")	   d.group = std::stoi(v);
		}
		catch (...) {}
	};

	auto commit_region = [&](const Desc& d)
	{
		if (d.sample.empty()) return;

		std::string sample_path = dir + "/" + d.sample;
		std::replace(sample_path.begin(), sample_path.end(), '\\', '/');

		sf.sfz_pcm.emplace_back();
		uint32_t rate = 44100; uint8_t nch = 1;
		if (!load_wav(sample_path.c_str(), sf.sfz_pcm.back(), rate, nch))
		{
			sf.sfz_pcm.pop_back();
			return;
		}

		SampleRegion r;
		r.lo_key = (uint8_t)std::clamp(d.lo_key, 0, 127);
		r.hi_key = (uint8_t)std::clamp(d.hi_key, 0, 127);
		r.lo_vel = (uint8_t)std::clamp(d.lo_vel, 0, 127);
		r.hi_vel = (uint8_t)std::clamp(d.hi_vel, 0, 127);
		r.root_key = (uint8_t)std::clamp(d.pitch_keycenter, 0, 127);

		r.pcm = sf.sfz_pcm.back().data();
		r.pcm_len = (uint32_t)(sf.sfz_pcm.back().size() / nch);
		r.sample_rate = rate;
		r.channels = nch;

		if (d.loop_mode == 1) r.loop_mode = LoopMode::Forward;
		else if (d.loop_mode == 2) r.loop_mode = LoopMode::OneShot;
		else if (d.loop_mode == 3) r.loop_mode = LoopMode::Sustain;
		else		       r.loop_mode = LoopMode::None;
		r.loop_start = (uint32_t)d.loop_start;
		r.loop_end = (d.loop_end > 0) ? (uint32_t)d.loop_end : r.pcm_len;
		if (r.loop_end > r.pcm_len) r.loop_end = r.pcm_len;

		r.coarse_tune = (int16_t)d.transpose;
		r.fine_tune = (int16_t)d.tune;
		r.scale_tuning = (uint16_t)d.scale_tuning;

		r.attack = (std::max)(d.ampeg_attack, 0.0f);
		r.hold = (std::max)(d.ampeg_hold, 0.0f);
		r.decay = (std::max)(d.ampeg_decay, 0.0f);
		r.sustain = std::clamp(d.ampeg_sustain / 100.0f, 0.0f, 1.0f);
		r.release = (std::max)(d.ampeg_release, 0.0f);

		r.pan = std::clamp(d.pan / 100.0f, -1.0f, 1.0f);
		r.attenuation = std::pow(10.0f, d.volume / 20.0f);

		r.exclusive_class = (uint16_t)d.group;

		sf.regions.push_back(r);
	};

	// Four-level hierarchy: global → master → group → region
	// When entering a child header, copy current parent state as starting point.
	Desc global_d, master_d, group_d, region_d;
	enum class H { None, Global, Master, Group, Region } hdr = H::None;

	std::string line;
	while (std::getline(file, line))
	{
		sfz_parse_line(line,
			[&](std::string hdr_name)
		{
			// Entering a new header — commit pending region first
			if (hdr == H::Region) commit_region(region_d);

			if (hdr_name == "global") { hdr = H::Global; global_d = Desc{}; }
			else if (hdr_name == "master") { hdr = H::Master; master_d = global_d; master_d.sample.clear(); }
			else if (hdr_name == "group") { hdr = H::Group;  group_d = master_d; group_d.sample.clear(); }
			else if (hdr_name == "region") { hdr = H::Region; region_d = group_d;  region_d.sample.clear(); }
		},
			[&](std::string k, std::string v)
		{
			switch (hdr)
			{
				case H::Global: parse_opcode(global_d, k, v); break;
				case H::Master: parse_opcode(master_d, k, v); break;
				case H::Group:  parse_opcode(group_d, k, v); break;
				case H::Region: parse_opcode(region_d, k, v); break;
				default: break;
			}
		});
	}
	if (hdr == H::Region)
		commit_region(region_d);

	return !sf.regions.empty();
}

} // namespace safsyn
