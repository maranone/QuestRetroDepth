#include "audio_processor.h"

#include <algorithm>
#include <cmath>

qrd::AudioProcessor g_audio_processor;

namespace qrd {

void AudioProcessor::set_sample_rate(int sr) {
    m_sr = sr;
    m_lp.set_lowpass(200.f,  static_cast<float>(sr));
    m_hp.set_highpass(4000.f, static_cast<float>(sr));
    m_lp.reset();
    m_hp.reset();
}

void AudioProcessor::process(int16_t* buf, int frames) {
    const int m = mode.load(std::memory_order_relaxed);
    if (m == 0) return;

    constexpr float kScale = 1.f / 32768.f;
    constexpr float kClamp = 32767.f;

    // Screen-lock pan: [−1,1]; 0 when disabled. Shared from XR thread.
    const float yaw   = screen_yaw.load(std::memory_order_relaxed);
    const float pitch = head_pitch.load(std::memory_order_relaxed);
    // Constant-power pan toward screen: at max deflection (±1) one ear drops to ~40%
    const float pan = std::clamp(yaw * 0.6f, -0.6f, 0.6f);
    const float gl  = 1.f - std::max(0.f,  pan);  // left gain
    const float gr  = 1.f - std::max(0.f, -pan);  // right gain

    // Mode 1: Wide — M/S widening then screen-lock pan
    if (m == 1) {
        constexpr float kSideGain = 1.4f;
        for (int i = 0; i < frames; ++i) {
            const float L = buf[i * 2 + 0] * kScale;
            const float R = buf[i * 2 + 1] * kScale;
            const float M = (L + R) * 0.5f;
            const float S = (L - R) * 0.5f * kSideGain;
            buf[i * 2 + 0] = static_cast<int16_t>(std::clamp((M + S) * gl * 32768.f, -kClamp, kClamp));
            buf[i * 2 + 1] = static_cast<int16_t>(std::clamp((M - S) * gr * 32768.f, -kClamp, kClamp));
        }
        return;
    }

    // Modes 2 & 3: 3-band spatial EQ + screen-lock
    constexpr float kWiden = 0.2f;
    // Pitch modulation: +pitch = looking up → highs up, bass down
    const float bass_gain = 1.f - pitch * 0.3f;
    const float high_gain = 1.f + pitch * 0.3f;
    float bass_energy = 0.f;

    for (int i = 0; i < frames; ++i) {
        const float inL = buf[i * 2 + 0] * kScale;
        const float inR = buf[i * 2 + 1] * kScale;

        float bassL, bassR;
        m_lp.tick(inL, inR, bassL, bassR);

        float highL, highR;
        m_hp.tick(inL, inR, highL, highR);

        const float midL = inL - bassL - highL;
        const float midR = inR - bassR - highR;

        // Bass → mono + floor pitch modulation
        const float mono = (bassL + bassR) * 0.5f * bass_gain;

        // High → widened + ceiling pitch modulation
        const float wideL = (highL - kWiden * highR) * high_gain;
        const float wideR = (highR - kWiden * highL) * high_gain;

        // Apply screen-lock yaw pan to final output
        const float outL = (mono + midL + wideL) * gl;
        const float outR = (mono + midR + wideR) * gr;

        buf[i * 2 + 0] = static_cast<int16_t>(std::clamp(outL * 32768.f, -kClamp, kClamp));
        buf[i * 2 + 1] = static_cast<int16_t>(std::clamp(outR * 32768.f, -kClamp, kClamp));

        bass_energy += bassL * bassL + bassR * bassR;
    }

    if (m == 3 && frames > 0) {
        const float rms = sqrtf(bass_energy / (frames * 2));
        bass_rms.store(rms, std::memory_order_relaxed);
    }
}

} // namespace qrd
