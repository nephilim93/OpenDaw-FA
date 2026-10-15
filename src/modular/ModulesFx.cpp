#include "ModuleHelpers.h"
#include "ModuleLibrary.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace OpenDaw::Modular {

namespace {

using namespace detail;

inline float flushDenormal(float v) { return std::abs(v) < 1.0e-15f || !std::isfinite(v) ? 0.0f : v; }

// ── Delay ───────────────────────────────────────────────────────────────────

class Delay : public Module {
public:
    void prepare(double sr) override
    {
        sampleRate_ = sr;
        size_ = static_cast<int>(sr * 2.5) + 8;
        buf_[0].assign(size_t(size_), 0.0f);
        buf_[1].assign(size_t(size_), 0.0f);
        slewCoef_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.05 * sr)));  // ~50 ms
        reset();
    }
    void reset() override
    {
        for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.0f);
        write_ = 0;
        lp_[0] = lp_[1] = 0.0f;
        delay_ = -1.0;
    }
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const* out, const float* p) override
    {
        if (size_ <= 0) {
            for (int i = 0; i < ctx.numSamples; ++i) out[0][i] = out[1][i] = 0.0f;
            return;
        }
        static constexpr double kSyncBeats[] = { 0.0, 1.0, 0.5, 0.75, 1.0 / 3.0, 0.25, 0.375 };
        const int sync = std::clamp(static_cast<int>(p[0]), 0, 6);
        const float feedback = clampf(p[2], 0.0f, 0.95f);
        const float mix = clampf(p[3], 0.0f, 1.0f);
        const bool pingPong = p[4] >= 0.5f;
        const float tone = clampf(p[5], 20.0f, 20000.0f);
        const bool bypassTone = tone >= 19999.0f;
        const float a = static_cast<float>(1.0 - std::exp(-kTwoPi * tone / sampleRate_));

        double baseSamples = std::clamp(double(p[1]), 1.0, 2000.0) * sampleRate_ / 1000.0;
        if (sync > 0) {
            const double bpm = std::clamp(ctx.transport.bpm > 0.0 ? ctx.transport.bpm : 120.0, 20.0, 999.0);
            baseSamples = kSyncBeats[sync] * 60.0 / bpm * sampleRate_;
        }
        const double maxDelay = double(size_ - 4);
        const float* inR = connected[1] ? in[1] : in[0];

        for (int i = 0; i < ctx.numSamples; ++i) {
            const float cv = clampf(std::isfinite(in[2][i]) ? in[2][i] : 0.0f, -8.0f, 8.0f);
            const double target = std::clamp(baseSamples * std::exp2(double(cv)), 1.0, maxDelay);
            if (delay_ < 0.0) delay_ = target;
            else delay_ += (target - delay_) * slewCoef_;

            double rp = double(write_) - delay_;
            if (rp < 0.0) rp += size_;
            const int i0 = static_cast<int>(rp);
            const float frac = static_cast<float>(rp - i0);
            const int i1 = (i0 + 1) % size_;
            float wet[2];
            for (int ch = 0; ch < 2; ++ch)
                wet[ch] = buf_[ch][size_t(i0)] + (buf_[ch][size_t(i1)] - buf_[ch][size_t(i0)]) * frac;

            for (int ch = 0; ch < 2; ++ch) {
                lp_[ch] = bypassTone ? wet[ch] : lp_[ch] + (wet[ch] - lp_[ch]) * a;
                lp_[ch] = flushDenormal(lp_[ch]);
            }
            float l = in[0][i], r = inR[i];
            if (!std::isfinite(l)) l = 0.0f;
            if (!std::isfinite(r)) r = 0.0f;
            float wl, wr;
            if (pingPong) {  // mono input enters on the left, echoes bounce between sides
                wl = 0.5f * (l + r) + feedback * lp_[1];
                wr = feedback * lp_[0];
            } else {
                wl = l + feedback * lp_[0];
                wr = r + feedback * lp_[1];
            }
            buf_[0][size_t(write_)] = clampf(flushDenormal(wl), -8.0f, 8.0f);
            buf_[1][size_t(write_)] = clampf(flushDenormal(wr), -8.0f, 8.0f);
            if (++write_ >= size_) write_ = 0;

            out[0][i] = l * (1.0f - mix) + wet[0] * mix;
            out[1][i] = r * (1.0f - mix) + wet[1] * mix;
        }
    }
private:
    double sampleRate_ = 44100.0;
    std::array<std::vector<float>, 2> buf_;
    int size_ = 0, write_ = 0;
    float lp_[2] = { 0.0f, 0.0f };
    double delay_ = -1.0;
    float slewCoef_ = 0.001f;
};

// ── Reverb (Freeverb) ───────────────────────────────────────────────────────

class Reverb : public Module {
public:
    void prepare(double sr) override
    {
        sampleRate_ = sr;
        static constexpr int kComb[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        static constexpr int kAllpass[4] = { 556, 441, 341, 225 };
        static constexpr int kSpread = 23;
        const double s = sr / 44100.0;
        for (int ch = 0; ch < 2; ++ch) {
            for (int c = 0; c < 8; ++c)
                combs_[ch][c].buf.assign(size_t(std::max(1, int((kComb[c] + ch * kSpread) * s))), 0.0f);
            for (int a = 0; a < 4; ++a)
                allpasses_[ch][a].buf.assign(size_t(std::max(1, int((kAllpass[a] + ch * kSpread) * s))), 0.0f);
        }
        pre_.assign(size_t(sr * 0.21) + 4, 0.0f);
        reset();
    }
    void reset() override
    {
        for (auto& chs : combs_) for (auto& c : chs) { std::fill(c.buf.begin(), c.buf.end(), 0.0f); c.pos = 0; c.store = 0.0f; }
        for (auto& chs : allpasses_) for (auto& a : chs) { std::fill(a.buf.begin(), a.buf.end(), 0.0f); a.pos = 0; }
        std::fill(pre_.begin(), pre_.end(), 0.0f);
        prePos_ = 0;
    }
    void process(const ProcessContext& ctx, const float* const* in, const bool* connected,
                 float* const* out, const float* p) override
    {
        const float* inR = connected[1] ? in[1] : in[0];
        if (pre_.empty()) {
            for (int i = 0; i < ctx.numSamples; ++i) { out[0][i] = in[0][i]; out[1][i] = inR[i]; }
            return;
        }
        const float room = clampf(p[0], 0.0f, 1.0f) * 0.28f + 0.7f;
        const float damp = clampf(p[1], 0.0f, 1.0f) * 0.4f;
        const float width = clampf(p[2], 0.0f, 1.0f);
        const float mix = clampf(p[3], 0.0f, 1.0f);
        const int preSize = int(pre_.size());
        const int preSamples = std::clamp(int(clampf(p[4], 0.0f, 200.0f) * sampleRate_ / 1000.0), 0, preSize - 1);
        const float wet1 = 3.0f * (width * 0.5f + 0.5f);
        const float wet2 = 3.0f * ((1.0f - width) * 0.5f);

        for (int i = 0; i < ctx.numSamples; ++i) {
            float l = in[0][i], r = inR[i];
            if (!std::isfinite(l)) l = 0.0f;
            if (!std::isfinite(r)) r = 0.0f;
            pre_[size_t(prePos_)] = (l + r) * 0.015f;
            int rp = prePos_ - preSamples;
            if (rp < 0) rp += preSize;
            const float x = pre_[size_t(rp)];
            if (++prePos_ >= preSize) prePos_ = 0;

            float o[2];
            for (int ch = 0; ch < 2; ++ch) {
                float acc = 0.0f;
                for (auto& c : combs_[ch]) {
                    const float y = c.buf[size_t(c.pos)];
                    c.store = flushDenormal(y * (1.0f - damp) + c.store * damp);
                    c.buf[size_t(c.pos)] = flushDenormal(x + c.store * room);
                    if (++c.pos >= int(c.buf.size())) c.pos = 0;
                    acc += y;
                }
                for (auto& a : allpasses_[ch]) {
                    const float b = a.buf[size_t(a.pos)];
                    a.buf[size_t(a.pos)] = flushDenormal(acc + b * 0.5f);
                    acc = b - acc;
                    if (++a.pos >= int(a.buf.size())) a.pos = 0;
                }
                o[ch] = acc;
            }
            const float wl = o[0] * wet1 + o[1] * wet2;
            const float wr = o[1] * wet1 + o[0] * wet2;
            out[0][i] = clampf(l * (1.0f - mix) + wl * mix, -8.0f, 8.0f);
            out[1][i] = clampf(r * (1.0f - mix) + wr * mix, -8.0f, 8.0f);
        }
    }
private:
    struct Comb { std::vector<float> buf; int pos = 0; float store = 0.0f; };
    struct Allpass { std::vector<float> buf; int pos = 0; };
    double sampleRate_ = 44100.0;
    std::array<std::array<Comb, 8>, 2> combs_;
    std::array<std::array<Allpass, 4>, 2> allpasses_;
    std::vector<float> pre_;
    int prePos_ = 0;
};

} // namespace

// Modules: delay, reverb.
void addFxModules(std::vector<ModuleDef>& defs)
{
    using K = PortKind;

    defs.push_back({ "delay", "Stereo echo. Echoes repeat every 'time' ms (or a tempo-synced "
        "note value), each one 'feedback' times the previous, darkened by 'tone'. ping_pong "
        "bounces echoes left/right. Use mix 1 for wet only (e.g. on a send). Global section only: place it "
        "after 'voices' (voices.left/right -> delay -> output) so echoes keep ringing after notes "
        "are released.",
        Section::Global,
        { { "left", K::Audio, "" }, { "right", K::Audio, "Optional; left is used if unconnected" },
          { "time", K::CV, "Multiplies the delay time by 2^cv (1 = double, -1 = half)" } },
        { { "left", K::Audio, "" }, { "right", K::Audio, "" } },
        { choice("sync", { "off", "1/4", "1/8", "1/8d", "1/8t", "1/16", "1/16d" }, 0,
                 "Tempo-synced delay time; 'off' uses the time param"),
          param("time", 1.0f, 2000.0f, 375.0f, "ms", "Delay time when sync is off"),
          param("feedback", 0.0f, 0.95f, 0.4f, "", "Echo level relative to the previous echo (0 = one echo)"),
          param("mix", 0.0f, 1.0f, 0.35f, "", "0 = dry only, 1 = echoes only"),
          choice("ping_pong", { "off", "on" }, 0, "Alternate echoes between left and right"),
          param("tone", 200.0f, 20000.0f, 8000.0f, "Hz", "Low-pass cutoff in the feedback path; lower = darker repeats") },
        make<Delay>() });

    defs.push_back({ "reverb", "Stereo room/hall reverb (Freeverb), global section only: put it "
        "after 'voices', before 'output'.",
        Section::Global,
        { { "left", K::Audio, "" }, { "right", K::Audio, "Optional; left is used if unconnected" } },
        { { "left", K::Audio, "" }, { "right", K::Audio, "" } },
        { param("size", 0.0f, 1.0f, 0.6f, "", "Room size / decay length (1 = very long tail)"),
          param("damping", 0.0f, 1.0f, 0.4f, "", "High-frequency absorption; higher = darker tail"),
          param("width", 0.0f, 1.0f, 1.0f, "", "Stereo width (0 = mono)"),
          param("mix", 0.0f, 1.0f, 0.3f, "", "0 = dry only, 1 = reverb only"),
          param("predelay", 0.0f, 200.0f, 10.0f, "ms", "Gap before the reverb starts") },
        make<Reverb>() });
}

} // namespace OpenDaw::Modular
