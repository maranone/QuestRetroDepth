#pragma once

#include <atomic>
#include <cstdint>
#include <cmath>

namespace qrd {

// Direct Form II Transposed biquad filter — operates on interleaved stereo int16.
struct Biquad {
    float b0 = 1.f, b1 = 0.f, b2 = 0.f;
    float a1 = 0.f, a2 = 0.f;
    float s1l = 0.f, s2l = 0.f;
    float s1r = 0.f, s2r = 0.f;

    void set_lowpass(float fc, float sr, float Q = 0.7071f) {
        const float w0 = 2.f * 3.14159265f * fc / sr;
        const float cosw = cosf(w0);
        const float alpha = sinf(w0) / (2.f * Q);
        const float a0inv = 1.f / (1.f + alpha);
        b0 = ((1.f - cosw) / 2.f) * a0inv;
        b1 = (1.f - cosw) * a0inv;
        b2 = b0;
        a1 = (-2.f * cosw) * a0inv;
        a2 = (1.f - alpha) * a0inv;
    }

    void set_highpass(float fc, float sr, float Q = 0.7071f) {
        const float w0 = 2.f * 3.14159265f * fc / sr;
        const float cosw = cosf(w0);
        const float alpha = sinf(w0) / (2.f * Q);
        const float a0inv = 1.f / (1.f + alpha);
        b0 = ((1.f + cosw) / 2.f) * a0inv;
        b1 = -(1.f + cosw) * a0inv;
        b2 = b0;
        a1 = (-2.f * cosw) * a0inv;
        a2 = (1.f - alpha) * a0inv;
    }

    // Process one stereo sample through the filter, returns filtered sample.
    void tick(float inL, float inR, float& outL, float& outR) {
        outL = b0 * inL + s1l;
        s1l  = b1 * inL - a1 * outL + s2l;
        s2l  = b2 * inL - a2 * outL;

        outR = b0 * inR + s1r;
        s1r  = b1 * inR - a1 * outR + s2r;
        s2r  = b2 * inR - a2 * outR;
    }

    void reset() { s1l = s2l = s1r = s2r = 0.f; }
};

// Processes the AAudio output buffer in-place.
// Thread-safe for mode/bass_rms: written by the XR thread, read by the audio callback.
struct AudioProcessor {
    std::atomic<int>   mode{0};         // 0=off  1=wide  2=spatial  3=spatial+haptic
    std::atomic<float> bass_rms{0.f};   // read by XR frame loop to drive haptics
    std::atomic<float> screen_yaw{0.f}; // [-1,1]: +1=screen to right; 0 when screen lock off
    std::atomic<float> head_pitch{0.f}; // [-1,1]: +1=looking up;      0 when screen lock off

    void set_sample_rate(int sr);

    // Called from audio_data_callback on the AAudio thread.
    // buf: interleaved stereo int16, frames stereo frames.
    void process(int16_t* buf, int frames);

private:
    Biquad m_lp;   // low-pass  ~200 Hz → bass band
    Biquad m_hp;   // high-pass ~4 kHz  → high band
    int    m_sr = 44100;
};

} // namespace qrd

extern qrd::AudioProcessor g_audio_processor;
