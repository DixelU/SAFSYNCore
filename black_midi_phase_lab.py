#!/usr/bin/env python3
"""Black-MIDI phase/retrigger laboratory.

Run it with:

    python -m pip install numpy scipy matplotlib streamlit
    python black_midi_phase_lab.py

The script relaunches itself under Streamlit and opens a browser UI.  A WAV can
be uploaded, or the built-in synthetic piano-like note can be used.  The DSP
engine is deliberately offline and sample-based: every render is deterministic
for a given random seed, which makes A/B comparisons repeatable.
"""

from __future__ import annotations

import io
import math
import os
import subprocess
import sys
from dataclasses import dataclass, replace
from fractions import Fraction
from pathlib import Path
from typing import Optional

import numpy as np
from scipy import signal
from scipy.io import wavfile


APP_TITLE = "Black-MIDI phase & retrigger lab"


@dataclass(frozen=True)
class RenderSettings:
    sample_rate: int = 44_100
    render_seconds: float = 2.0
    trigger_hz: float = 40.0
    dispatch_mode: str = "Overlap full samples"
    gate_duty_percent: float = 100.0
    gate_fade_ms: float = 0.0
    timing_jitter_ms: float = 0.0
    fractional_jitter_samples: float = 0.0
    phase_mode: str = "None"
    phase_strength: float = 1.0
    phase_correlation_hz: float = 250.0
    phase_variants: int = 16
    preserve_attack_ms: float = 0.0
    amplitude_jitter_percent: float = 0.0
    polarity_flip_percent: float = 0.0
    steady_state_preroll: bool = True
    output_gain_db: float = -12.0
    seed: int = 7


@dataclass
class RenderResult:
    audio: np.ndarray
    event_times: np.ndarray
    raw_peak: float
    raw_rms: float
    raw_crest_db: float
    periodicity: float
    direct_rate_db: float
    preview_scale: float


def synthetic_piano_note(fs: int = 44_100, seconds: float = 2.8,
                         fundamental_hz: float = 261.625565) -> np.ndarray:
    """Make a deterministic, bright, decaying struck-string test sample."""
    count = max(32, int(round(fs * seconds)))
    t = np.arange(count, dtype=np.float64) / fs
    note = np.zeros_like(t)
    # Slight inharmonicity makes this more piano-like than a harmonic saw.
    inharmonicity = 1.8e-4
    rng = np.random.default_rng(19)
    for harmonic in range(1, 19):
        partial_hz = fundamental_hz * harmonic * math.sqrt(
            1.0 + inharmonicity * harmonic * harmonic
        )
        if partial_hz >= 0.48 * fs:
            break
        amplitude = (harmonic ** -1.18) * math.exp(-0.055 * harmonic)
        decay = 1.0 + 0.23 * harmonic
        phase = rng.uniform(-0.15, 0.15)
        note += amplitude * np.sin(2.0 * np.pi * partial_hz * t + phase) \
            * np.exp(-decay * t)

    attack = 1.0 - np.exp(-t / 0.0017)
    body = 0.84 * np.exp(-0.75 * t) + 0.16 * np.exp(-3.8 * t)
    hammer = rng.standard_normal(count)
    hammer = signal.lfilter([1.0, -0.96], [1.0], hammer)
    hammer *= 0.055 * np.exp(-t / 0.010)
    note = note * attack * body + hammer
    note *= np.minimum(1.0, np.maximum(0.0, (seconds - t) / 0.025))
    peak = float(np.max(np.abs(note)))
    return (0.85 * note / max(peak, 1e-12)).astype(np.float64)


def decode_wav(data: bytes, target_fs: int, max_seconds: float = 5.0) -> np.ndarray:
    """Decode PCM/float WAV, downmix it, resample it, and make it finite."""
    source_fs, samples = wavfile.read(io.BytesIO(data))
    if samples.ndim == 2:
        samples = np.mean(samples.astype(np.float64), axis=1)
    elif samples.ndim != 1:
        raise ValueError("The WAV must be mono or stereo.")

    if np.issubdtype(samples.dtype, np.integer):
        info = np.iinfo(samples.dtype)
        scale = float(max(abs(info.min), info.max))
        samples = samples.astype(np.float64) / scale
    else:
        samples = samples.astype(np.float64)
    samples = np.nan_to_num(samples)

    if source_fs != target_fs:
        ratio = Fraction(target_fs, int(source_fs)).limit_denominator(2000)
        samples = signal.resample_poly(samples, ratio.numerator, ratio.denominator)
    samples = samples[: max(32, int(target_fs * max_seconds))]
    if samples.size < 32:
        raise ValueError("The WAV is too short for the experiment.")

    samples -= float(np.mean(samples))
    # A small end taper prevents an uploaded, abruptly cut file from adding an
    # unrelated boundary click.  Its beginning is intentionally untouched.
    taper_n = min(samples.size, max(1, int(0.008 * target_fs)))
    samples[-taper_n:] *= np.linspace(1.0, 0.0, taper_n)
    peak = float(np.max(np.abs(samples)))
    if peak < 1e-12:
        raise ValueError("The WAV contains no measurable signal.")
    return 0.85 * samples / peak


def _attack_blend(original: np.ndarray, changed: np.ndarray, fs: int,
                  preserve_ms: float) -> np.ndarray:
    if preserve_ms <= 0.0:
        return changed
    hold = int(round(preserve_ms * 1e-3 * fs))
    if hold >= original.size:
        return original.copy()
    fade = max(1, min(int(0.010 * fs), original.size - hold))
    blend = np.ones(original.size, dtype=np.float64)
    blend[:hold] = 0.0
    u = np.linspace(0.0, 1.0, fade)
    blend[hold:hold + fade] = u * u * (3.0 - 2.0 * u)
    return original * (1.0 - blend) + changed * blend


def _phase_variant(sample: np.ndarray, fs: int, mode: str, strength: float,
                   correlation_hz: float, preserve_attack_ms: float,
                   rng: np.random.Generator,
                   analytic_quadrature: Optional[np.ndarray]) -> np.ndarray:
    """Return one real signal with a perturbed phase and unchanged FFT magnitudes.

    The optional attack-preserving blend intentionally gives up exact magnitude
    preservation in exchange for keeping the onset recognizable.
    """
    strength = float(np.clip(strength, 0.0, 1.0))
    if mode == "None" or strength == 0.0:
        return sample

    if mode == "One phase angle (analytic rotation)":
        assert analytic_quadrature is not None
        angle = rng.uniform(-np.pi * strength, np.pi * strength)
        changed = np.cos(angle) * sample - np.sin(angle) * analytic_quadrature
    else:
        spectrum = np.fft.rfft(sample)
        bins = spectrum.size
        if mode == "Independent FFT-bin noise":
            delta = rng.uniform(-np.pi * strength, np.pi * strength, bins)
        elif mode == "Smooth FFT phase field":
            bin_hz = fs / sample.size
            stride = max(1, int(round(correlation_hz / max(bin_hz, 1e-12))))
            anchors_x = np.arange(0, bins, stride, dtype=np.float64)
            if anchors_x.size == 0 or anchors_x[-1] != bins - 1:
                anchors_x = np.append(anchors_x, bins - 1)
            anchors_y = rng.uniform(-np.pi * strength, np.pi * strength,
                                    anchors_x.size)
            delta = np.interp(np.arange(bins), anchors_x, anchors_y)
        else:
            raise ValueError(f"Unknown phase mode: {mode}")

        # DC and Nyquist coefficients must stay real for a real iFFT.
        delta[0] = 0.0
        if sample.size % 2 == 0:
            delta[-1] = 0.0
        changed = np.fft.irfft(spectrum * np.exp(1j * delta), n=sample.size)

    # Phase-only transforms preserve energy, not necessarily peak level.
    changed_rms = math.sqrt(float(np.mean(changed * changed)) + 1e-30)
    original_rms = math.sqrt(float(np.mean(sample * sample)) + 1e-30)
    changed = changed * (original_rms / changed_rms)
    return _attack_blend(sample, changed, fs, preserve_attack_ms)


def _mix_fractional(destination: np.ndarray, source: np.ndarray,
                    start_sample: float, gain: float) -> None:
    """Add source at a fractional sample position using linear interpolation."""
    integer = math.floor(start_sample)
    fraction = start_sample - integer
    for offset, weight in ((integer, 1.0 - fraction), (integer + 1, fraction)):
        if weight <= 1e-15:
            continue
        dst_first = max(0, offset)
        src_first = max(0, -offset)
        amount = min(destination.size - dst_first, source.size - src_first)
        if amount > 0:
            destination[dst_first:dst_first + amount] += (
                gain * weight * source[src_first:src_first + amount]
            )


def _analysis_metrics(raw: np.ndarray, fs: int, trigger_hz: float) -> tuple[float, float, float]:
    peak = float(np.max(np.abs(raw))) if raw.size else 0.0
    rms = math.sqrt(float(np.mean(raw * raw)) + 1e-30)
    crest = 20.0 * math.log10(max(peak, 1e-15) / max(rms, 1e-15))
    lag = max(1, int(round(fs / trigger_hz)))
    if lag >= raw.size:
        periodicity = 0.0
    else:
        left, right = raw[:-lag], raw[lag:]
        denom = math.sqrt(float(np.dot(left, left) * np.dot(right, right)))
        periodicity = float(np.dot(left, right) / denom) if denom > 1e-20 else 0.0

    window = signal.windows.hann(raw.size, sym=False)
    spectrum = np.fft.rfft(raw * window)
    frequencies = np.fft.rfftfreq(raw.size, 1.0 / fs)
    magnitudes = np.abs(spectrum)
    rate_amplitude = float(np.interp(trigger_hz, frequencies, magnitudes))
    reference = float(np.max(magnitudes)) if magnitudes.size else 1.0
    direct_rate_db = 20.0 * math.log10(max(rate_amplitude, 1e-15) /
                                       max(reference, 1e-15))
    return crest, periodicity, direct_rate_db


def render(sample: np.ndarray, settings: RenderSettings) -> RenderResult:
    fs = settings.sample_rate
    out_n = max(32, int(round(settings.render_seconds * fs)))
    interval = 1.0 / settings.trigger_hz
    rng = np.random.default_rng(settings.seed)

    pre_seconds = (sample.size / fs + 2.0 * interval) \
        if settings.steady_state_preroll else 0.0
    first_index = -int(math.ceil(pre_seconds / interval))
    last_index = int(math.ceil(settings.render_seconds / interval)) + 1
    nominal = np.arange(first_index, last_index + 1, dtype=np.float64) * interval

    timing = rng.uniform(-settings.timing_jitter_ms * 1e-3,
                         settings.timing_jitter_ms * 1e-3,
                         nominal.size)
    sub_sample = rng.uniform(-settings.fractional_jitter_samples / fs,
                             settings.fractional_jitter_samples / fs,
                             nominal.size)
    actual = nominal + timing + sub_sample
    order = np.argsort(actual)
    actual = actual[order]

    phase_rng = np.random.default_rng(settings.seed + 10_007)
    pool_size = 1 if settings.phase_mode == "None" else max(
        1, min(int(settings.phase_variants), 64)
    )
    quadrature = None
    if settings.phase_mode == "One phase angle (analytic rotation)":
        quadrature = np.imag(signal.hilbert(sample))
    pool = [
        _phase_variant(sample, fs, settings.phase_mode, settings.phase_strength,
                       settings.phase_correlation_hz, settings.preserve_attack_ms,
                       phase_rng, quadrature)
        for _ in range(pool_size)
    ]
    pool_choices = rng.integers(0, pool_size, actual.size)

    amplitude_spread = settings.amplitude_jitter_percent * 0.01
    amplitudes = 1.0 + rng.uniform(-amplitude_spread, amplitude_spread, actual.size)
    flips = rng.random(actual.size) < settings.polarity_flip_percent * 0.01
    amplitudes[flips] *= -1.0

    raw = np.zeros(out_n, dtype=np.float64)
    fade_samples = int(round(settings.gate_fade_ms * 1e-3 * fs))
    for event_index, event_time in enumerate(actual):
        voice = pool[int(pool_choices[event_index])]
        if settings.dispatch_mode == "Chop/restart at next dispatch":
            if event_index + 1 < actual.size:
                available = max(0.0, actual[event_index + 1] - event_time)
            else:
                available = interval
            gate_seconds = available * settings.gate_duty_percent * 0.01
            gate_samples = max(1, min(voice.size, int(round(gate_seconds * fs))))
            voice = voice[:gate_samples].copy()
            local_fade = min(fade_samples, voice.size)
            if local_fade > 0:
                # A linear release keeps the parameter's meaning obvious.
                voice[-local_fade:] *= np.linspace(1.0, 0.0, local_fade)
        _mix_fractional(raw, voice, event_time * fs,
                        float(amplitudes[event_index]))

    gain = 10.0 ** (settings.output_gain_db / 20.0)
    raw *= gain
    crest, periodicity, direct_rate_db = _analysis_metrics(
        raw, fs, settings.trigger_hz
    )
    raw_peak = float(np.max(np.abs(raw)))
    raw_rms = math.sqrt(float(np.mean(raw * raw)) + 1e-30)
    # The plotted metrics remain pre-safety-scale.  Only browser playback/WAV
    # conversion is protected from integer clipping.
    preview_scale = min(1.0, 0.98 / max(raw_peak, 1e-15))
    audio = (raw * preview_scale).astype(np.float32)
    visible_events = actual[(actual >= 0.0) & (actual <= settings.render_seconds)]
    return RenderResult(audio, visible_events, raw_peak, raw_rms, crest,
                        periodicity, direct_rate_db, preview_scale)


def wav_bytes(audio: np.ndarray, fs: int) -> bytes:
    payload = io.BytesIO()
    pcm = np.clip(audio, -1.0, 1.0)
    pcm = np.round(pcm * 32767.0).astype(np.int16)
    wavfile.write(payload, fs, pcm)
    return payload.getvalue()


def spectrum(audio: np.ndarray, fs: int) -> tuple[np.ndarray, np.ndarray]:
    window = signal.windows.hann(audio.size, sym=False)
    values = np.fft.rfft(audio.astype(np.float64) * window)
    frequency = np.fft.rfftfreq(audio.size, 1.0 / fs)
    amplitude = np.abs(values) / max(float(np.sum(window)) * 0.5, 1e-15)
    db = 20.0 * np.log10(np.maximum(amplitude, 1e-12))
    return frequency, db


def make_figure(current: RenderResult, baseline: RenderResult,
                settings: RenderSettings, view_ms: float,
                spectrum_ceiling_hz: float):
    import matplotlib.pyplot as plt

    fs = settings.sample_rate
    figure, axes = plt.subplots(2, 2, figsize=(13.2, 8.0), constrained_layout=True)
    ax_wave, ax_low, ax_wide, ax_spec = axes.flat

    view_seconds = min(settings.render_seconds, view_ms * 1e-3)
    start = max(0.0, min(settings.render_seconds - view_seconds,
                         0.33 * settings.render_seconds))
    first = int(start * fs)
    last = min(current.audio.size, int((start + view_seconds) * fs))
    times = np.arange(first, last) / fs
    ax_wave.plot(times * 1000.0, baseline.audio[first:last], color="#999999",
                 lw=0.8, alpha=0.65, label="baseline")
    ax_wave.plot(times * 1000.0, current.audio[first:last], color="#5b5bd6",
                 lw=0.9, label="current")
    for event in current.event_times:
        if start <= event <= start + view_seconds:
            ax_wave.axvline(event * 1000.0, color="#e05a47", lw=0.45, alpha=0.35)
    ax_wave.set(title="Waveform (red = actual dispatch)", xlabel="time (ms)",
                ylabel="amplitude")
    ax_wave.legend(loc="upper right", fontsize=8)
    ax_wave.grid(alpha=0.17)

    f_cur, d_cur = spectrum(current.audio, fs)
    f_base, d_base = spectrum(baseline.audio, fs)
    low_ceiling = min(fs * 0.5, max(120.0, settings.trigger_hz * 8.0))
    low_mask = f_cur <= low_ceiling
    ax_low.plot(f_base[low_mask], d_base[low_mask], color="#999999", lw=0.8,
                alpha=0.7, label="baseline")
    ax_low.plot(f_cur[low_mask], d_cur[low_mask], color="#5b5bd6", lw=1.0,
                label="current")
    for multiple in range(1, 9):
        marker = multiple * settings.trigger_hz
        if marker <= low_ceiling:
            ax_low.axvline(marker, color="#e05a47", lw=0.55, alpha=0.45)
    ax_low.set(title="Low spectrum (red = trigger harmonics)", xlabel="Hz",
               ylabel="dBFS", ylim=(-120, 6), xlim=(0, low_ceiling))
    ax_low.grid(alpha=0.17)

    wide_mask = (f_cur >= 20.0) & (f_cur <= spectrum_ceiling_hz)
    ax_wide.semilogx(f_base[wide_mask], d_base[wide_mask], color="#999999",
                     lw=0.7, alpha=0.6, label="baseline")
    ax_wide.semilogx(f_cur[wide_mask], d_cur[wide_mask], color="#5b5bd6", lw=0.8,
                     label="current")
    ax_wide.set(title="Wide spectrum", xlabel="Hz", ylabel="dBFS",
                ylim=(-120, 6), xlim=(20, spectrum_ceiling_hz))
    ax_wide.grid(alpha=0.17, which="both")

    nperseg = min(2048, max(128, 2 ** int(math.floor(math.log2(current.audio.size)))))
    f_s, t_s, sxx = signal.spectrogram(
        current.audio, fs=fs, window="hann", nperseg=nperseg,
        noverlap=3 * nperseg // 4, scaling="spectrum", mode="magnitude"
    )
    spec_mask = f_s <= spectrum_ceiling_hz
    image = ax_spec.pcolormesh(
        t_s, f_s[spec_mask],
        20.0 * np.log10(np.maximum(sxx[spec_mask], 1e-9)),
        shading="auto", vmin=-105, vmax=-15, cmap="magma"
    )
    ax_spec.set(title="Current spectrogram", xlabel="time (s)", ylabel="Hz",
                ylim=(0, spectrum_ceiling_hz))
    figure.colorbar(image, ax=ax_spec, label="dB")
    return figure


def run_app() -> None:
    import matplotlib.pyplot as plt
    import streamlit as st

    st.set_page_config(page_title=APP_TITLE, layout="wide")
    st.title(APP_TITLE)
    st.caption(
        "A deterministic A/B bench for periodic sample dispatch, hard retriggering, "
        "scheduler jitter, and several meanings of ‘phase jitter’."
    )

    with st.sidebar:
        st.header("Source")
        fs = st.selectbox("Sample rate", [44_100, 48_000], index=0)
        uploaded = st.file_uploader("Optional mono/stereo PCM WAV", type=["wav"])
        source_note = st.slider("Built-in note frequency (Hz)", 55.0, 880.0,
                                261.625565, 0.5, disabled=uploaded is not None)
        render_seconds = st.slider("Render length (s)", 0.5, 5.0, 2.0, 0.25)

        st.header("Dispatch")
        trigger_hz = st.slider("Dispatch frequency (Hz)", 1.0, 500.0, 40.0, 1.0)
        st.caption(f"Interval: {1000.0 / trigger_hz:.4f} ms")
        dispatch_mode = st.radio(
            "Voice behavior",
            ["Overlap full samples", "Chop/restart at next dispatch"],
        )
        gate_duty = st.slider("Gate duty (% of actual interval)", 1.0, 100.0,
                              100.0, 1.0,
                              disabled=dispatch_mode == "Overlap full samples")
        gate_fade = st.slider("Release fade (ms)", 0.0, 20.0, 0.0, 0.1,
                              disabled=dispatch_mode == "Overlap full samples")
        timing_jitter = st.slider("Dispatch-time jitter ±ms", 0.0, 25.0, 0.0, 0.05)
        fractional_jitter = st.slider("Extra fractional jitter ±samples", 0.0, 2.0,
                                      0.0, 0.01)
        steady_state = st.checkbox("Pre-roll to steady state", value=True)

        st.header("Phase per dispatch")
        phase_mode = st.selectbox(
            "Phase operation",
            ["None", "One phase angle (analytic rotation)",
             "Smooth FFT phase field", "Independent FFT-bin noise"],
        )
        phase_strength = st.slider("Phase jitter strength", 0.0, 1.0, 1.0, 0.01,
                                   disabled=phase_mode == "None")
        phase_variants = st.slider("Different phase sheets in pool", 1, 64, 16, 1,
                                   disabled=phase_mode == "None")
        phase_correlation = st.slider(
            "Smooth-field correlation width (Hz)", 1.0, 4000.0, 250.0, 1.0,
            disabled=phase_mode != "Smooth FFT phase field",
        )
        preserve_attack = st.slider("Preserve original attack (ms)", 0.0, 200.0,
                                    0.0, 1.0, disabled=phase_mode == "None")

        st.header("Other deliberate vandalism")
        amplitude_jitter = st.slider("Per-dispatch amplitude jitter ±%", 0.0, 100.0,
                                     0.0, 1.0)
        polarity_flip = st.slider("Random polarity-flip probability (%)", 0.0, 100.0,
                                  0.0, 1.0)
        output_gain = st.slider("Voice/output gain (dB)", -48.0, 0.0, -12.0, 0.5)
        seed = st.number_input("Random seed", min_value=0, max_value=2_147_483_647,
                               value=7, step=1)

    try:
        if uploaded is None:
            source = synthetic_piano_note(fs, 2.8, source_note)
            source_name = "synthetic piano-like C4"
        else:
            source = decode_wav(uploaded.getvalue(), fs)
            source_name = uploaded.name
    except Exception as exc:
        st.error(f"Could not use that WAV: {exc}")
        st.stop()

    settings = RenderSettings(
        sample_rate=fs,
        render_seconds=render_seconds,
        trigger_hz=trigger_hz,
        dispatch_mode=dispatch_mode,
        gate_duty_percent=gate_duty,
        gate_fade_ms=gate_fade,
        timing_jitter_ms=timing_jitter,
        fractional_jitter_samples=fractional_jitter,
        phase_mode=phase_mode,
        phase_strength=phase_strength,
        phase_correlation_hz=phase_correlation,
        phase_variants=phase_variants,
        preserve_attack_ms=preserve_attack,
        amplitude_jitter_percent=amplitude_jitter,
        polarity_flip_percent=polarity_flip,
        steady_state_preroll=steady_state,
        output_gain_db=output_gain,
        seed=int(seed),
    )
    baseline_settings = replace(
        settings,
        timing_jitter_ms=0.0,
        fractional_jitter_samples=0.0,
        phase_mode="None",
        phase_strength=0.0,
        amplitude_jitter_percent=0.0,
        polarity_flip_percent=0.0,
    )

    with st.spinner("Rendering deterministic A/B pair…"):
        current = render(source, settings)
        baseline = render(source, baseline_settings)

    st.subheader("Listen")
    audio_current, audio_baseline = st.columns(2)
    with audio_current:
        st.markdown("**Current settings**")
        current_wav = wav_bytes(current.audio, fs)
        st.audio(current_wav, format="audio/wav")
        st.download_button("Download current WAV", current_wav,
                           "phase-lab-current.wav", "audio/wav")
    with audio_baseline:
        st.markdown("**Baseline: exact timing, identical phase**")
        baseline_wav = wav_bytes(baseline.audio, fs)
        st.audio(baseline_wav, format="audio/wav")
        st.download_button("Download baseline WAV", baseline_wav,
                           "phase-lab-baseline.wav", "audio/wav")

    if current.preview_scale < 0.999999:
        reduction = -20.0 * math.log10(current.preview_scale)
        st.warning(
            f"The raw current render peaked at {current.raw_peak:.3f}; playback was "
            f"safety-scaled by {reduction:.2f} dB. Metrics below use the unscaled mix."
        )

    m1, m2, m3, m4 = st.columns(4)
    m1.metric("Raw peak", f"{current.raw_peak:.4f}",
              f"{20 * math.log10(max(current.raw_peak, 1e-15) / max(baseline.raw_peak, 1e-15)):+.2f} dB vs baseline")
    m2.metric("Raw RMS", f"{current.raw_rms:.4f}",
              f"{20 * math.log10(max(current.raw_rms, 1e-15) / max(baseline.raw_rms, 1e-15)):+.2f} dB vs baseline")
    m3.metric("Autocorrelation at 1/rate", f"{current.periodicity:+.4f}",
              f"{current.periodicity - baseline.periodicity:+.4f} vs baseline")
    m4.metric("Direct rate bin / strongest bin", f"{current.direct_rate_db:.1f} dB",
              f"{current.direct_rate_db - baseline.direct_rate_db:+.1f} dB vs baseline")

    plot_controls = st.columns(2)
    with plot_controls[0]:
        view_ms = st.slider("Waveform view width (ms)", 10.0, 500.0, 120.0, 5.0)
    with plot_controls[1]:
        spectrum_ceiling = st.slider("Spectrum/spectrogram ceiling (Hz)", 500.0,
                                     min(20_000.0, fs / 2.0), 6000.0, 100.0)
    figure = make_figure(current, baseline, settings, view_ms, spectrum_ceiling)
    st.pyplot(figure, use_container_width=True)
    plt.close(figure)

    with st.expander("What the Fourier transform says here", expanded=True):
        st.markdown(
            r"""
For identical copies dispatched at exactly $T$-spaced times,

$$Y(f)=X(f)\sum_n e^{-i2\pi f nT}. $$

So a **finite train does not translate the sample to new frequencies**; it
multiplies its existing spectrum by an interference comb.  In the ideal
infinite periodic limit, however,

$$Y(f)=\frac1T\sum_k X(k/T)\,\delta(f-k/T).$$

The periodic sum therefore samples the source spectrum on a line grid spaced
by $1/T$.  The coefficient at $1/T$ can genuinely be zero, while higher lines
still establish that period and can evoke a missing-fundamental pitch.  Hard
chopping contributes broadband edge energy; nonlinear clipping or limiting can
also create components that linear superposition cannot.

Timing jitter blurs the comb.  A different phase sheet per event means the
events are no longer shifted copies of one fixed $x(t)$, so the simple product
formula stops applying.  “Different phase sheets in pool = 1” is a useful
control: it changes the sample, but keeps every dispatch mutually coherent.
"""
        )

    with st.expander("Suggested experiment sequence"):
        st.markdown(
            """
1. Use **Overlap full samples**, zero jitter, and phase mode **None**. Sweep the
   dispatch rate and inspect the red harmonic grid.
2. Add only **fractional-sample jitter**. This is the closest model of clean,
   controlled scheduler timing error because it applies a linear phase slope.
3. Set **One phase angle**, strength 1, and compare phase-pool sizes 1 and 64.
4. Try **Smooth FFT phase field**, then reduce its correlation width toward
   independent-bin noise. Listen for the transient turning into a cloud.
5. Switch to **Chop/restart**, leave the release fade at zero, then introduce a
   2–5 ms fade. This separates the hard-edge contribution from coherent overlap.
6. If the direct rate bin becomes weak but the pitch remains, that is evidence
   for periodic higher harmonics / missing-fundamental perception rather than a
   literal strong spectral component at the repetition rate.
"""
        )
    st.caption(f"Source: {source_name} · {source.size / fs:.3f} s · {fs} Hz")


def _inside_streamlit() -> bool:
    try:
        from streamlit.runtime.scriptrunner import get_script_run_ctx
        return get_script_run_ctx(suppress_warning=True) is not None
    except Exception:
        return False


def _self_test() -> None:
    fs = 12_000
    source = synthetic_piano_note(fs, 0.25, 220.0)
    base = RenderSettings(sample_rate=fs, render_seconds=0.3, trigger_hz=40.0,
                          steady_state_preroll=True)
    results = [
        render(source, base),
        render(source, replace(base, timing_jitter_ms=1.5)),
        render(source, replace(base, phase_mode="One phase angle (analytic rotation)")),
        render(source, replace(base, phase_mode="Smooth FFT phase field")),
        render(source, replace(base, phase_mode="Independent FFT-bin noise")),
        render(source, replace(base, dispatch_mode="Chop/restart at next dispatch",
                               gate_fade_ms=2.0)),
    ]
    assert all(r.audio.shape == (int(fs * 0.3),) for r in results)
    assert all(np.all(np.isfinite(r.audio)) for r in results)
    assert wav_bytes(results[0].audio, fs)[:4] == b"RIFF"
    print("Self-test passed:", len(results), "render paths")


def main() -> None:
    if "--self-test" in sys.argv:
        _self_test()
        return
    if _inside_streamlit():
        run_app()
        return
    try:
        import streamlit  # noqa: F401
    except ImportError:
        raise SystemExit(
            "Streamlit is not installed. Run:\n"
            "  python -m pip install numpy scipy matplotlib streamlit\n"
            "then launch this script again."
        )
    from streamlit.web import cli as streamlit_cli

    os.environ.setdefault("STREAMLIT_BROWSER_GATHER_USAGE_STATS", "false")
    sys.argv = ["streamlit", "run", str(Path(__file__).resolve())]
    raise SystemExit(streamlit_cli.main())


if __name__ == "__main__":
    main()
