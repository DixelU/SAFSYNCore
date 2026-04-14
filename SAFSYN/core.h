#pragma once
#include <cstdint>
#include <vector>

enum class LoopMode : uint8_t { None, Forward, PingPong, OneShot };

struct SampleRegion {
    // Key / velocity mapping
    uint8_t  lo_key   = 0;
    uint8_t  hi_key   = 127;
    uint8_t  lo_vel   = 0;
    uint8_t  hi_vel   = 127;
    uint8_t  root_key = 60;

    // Audio (16-bit PCM, interleaved if stereo)
    // Points into Soundfont::pcm_pool (SF2) or Soundfont::sfz_pcm[n] (SFZ).
    const int16_t* pcm         = nullptr;
    uint32_t       pcm_len     = 0;      // frames (samples per channel)
    uint32_t       sample_rate = 44100;
    uint8_t        channels    = 1;      // 1 = mono, 2 = stereo

    // Loop (frame indices, relative to pcm[0])
    uint32_t loop_start = 0;
    uint32_t loop_end   = 0;
    LoopMode loop_mode  = LoopMode::None;

    // Tuning
    int16_t  coarse_tune  = 0;    // semitones
    int16_t  fine_tune    = 0;    // cents
    uint16_t scale_tuning = 100;  // cents per semitone (100 = normal pitch tracking)

    // Volume envelope (times in seconds; sustain is linear 0..1)
    float attack  = 0.001f;
    float hold    = 0.0f;
    float decay   = 0.0f;
    float sustain = 1.0f;
    float release = 0.05f;

    // Mix
    float pan         = 0.0f;  // -1 (left) .. 1 (right)
    float attenuation = 1.0f;  // linear gain

    // Exclusive class: voices with the same nonzero class mute each other (hi-hats, etc.)
    uint16_t exclusive_class = 0;
};

struct Soundfont {
    std::vector<SampleRegion>         regions;
    std::vector<int16_t>              pcm_pool;  // SF2: all sample data lives here
    std::vector<std::vector<int16_t>> sfz_pcm;   // SFZ: one buffer per loaded sample file
};

bool load_sf2(const char* path, Soundfont& sf);
bool load_sfz(const char* path, Soundfont& sf);

void init();
void consume_short_msg(uint32_t msg);
