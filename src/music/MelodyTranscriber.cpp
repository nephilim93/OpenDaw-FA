#include "MelodyTranscriber.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace OpenDaw::Melody {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kHopSeconds = 0.010;
constexpr double kAnalysisRate = 16000.0;    // plenty for a voice or a whistle
constexpr double kYinThreshold = 0.15;
constexpr double kYinUnvoiced = 0.35;        // no dip below this: no pitch
constexpr double kGateBelowPeakDb = 30.0;    // quieter than this under the loudest frame: silence
constexpr double kGateFloorDb = -55.0;
constexpr int kBridgeFrames = 1;             // a single unpitched frame inside a note is bridged
constexpr double kDipDb = 6.0;               // a "d" or soft "t" dips at least this much
constexpr double kDipWindowSeconds = 0.08;
constexpr double kStepSemitones = 0.75;      // pitch moved this far, and stayed: a new note
constexpr int kStepFrames = 5;               // ...for 50 ms
constexpr int kVibratoFrames = 15;           // ~one vibrato cycle (4-7 Hz): averaged out of the steadiness

double hzToMidi(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }

double median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Low-pass (windowed sinc) and keep every `factor`-th sample.
std::vector<float> decimate(const std::vector<float>& in, int factor)
{
    if (factor <= 1) return in;
    constexpr int kTaps = 63;
    const double cutoff = 0.45 / factor;   // of the input rate
    std::array<double, kTaps> h{};
    double sum = 0.0;
    for (int i = 0; i < kTaps; ++i) {
        const double m = i - (kTaps - 1) / 2.0;
        const double sinc = m == 0.0 ? 2.0 * cutoff : std::sin(2.0 * kPi * cutoff * m) / (kPi * m);
        const double window = 0.54 - 0.46 * std::cos(2.0 * kPi * i / (kTaps - 1));
        h[size_t(i)] = sinc * window;
        sum += h[size_t(i)];
    }
    for (auto& c : h) c /= sum;

    std::vector<float> out;
    out.reserve(in.size() / size_t(factor) + 1);
    const int half = (kTaps - 1) / 2;
    for (size_t n = 0; n < in.size(); n += size_t(factor)) {
        double acc = 0.0;
        for (int k = 0; k < kTaps; ++k) {
            const long long idx = (long long) n + k - half;
            if (idx >= 0 && idx < (long long) in.size()) acc += h[size_t(k)] * in[size_t(idx)];
        }
        out.push_back(float(acc));
    }
    return out;
}

// One YIN estimate (de Cheveigné & Kawahara 2002) over x[start ...].
void yin(const std::vector<float>& x, size_t start, int window, int tauMin, int tauMax, double rate,
         std::vector<double>& d, Frame& f)
{
    d.assign(size_t(tauMax) + 2, 0.0);
    for (int tau = 1; tau <= tauMax + 1; ++tau) {
        double acc = 0.0;
        const float* a = x.data() + start;
        const float* b = a + tau;
        for (int j = 0; j < window; ++j) {
            const double diff = double(a[j]) - double(b[j]);
            acc += diff * diff;
        }
        d[size_t(tau)] = acc;
    }
    // Cumulative mean normalised difference.
    double running = 0.0;
    d[0] = 1.0;
    for (int tau = 1; tau <= tauMax + 1; ++tau) {
        running += d[size_t(tau)];
        d[size_t(tau)] = running > 0.0 ? d[size_t(tau)] * tau / running : 1.0;
    }

    int best = -1;
    for (int tau = tauMin; tau <= tauMax; ++tau) {
        if (d[size_t(tau)] < kYinThreshold) {
            while (tau + 1 <= tauMax && d[size_t(tau + 1)] < d[size_t(tau)]) ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0) {
        best = tauMin;
        for (int tau = tauMin; tau <= tauMax; ++tau)
            if (d[size_t(tau)] < d[size_t(best)]) best = tau;
        if (d[size_t(best)] > kYinUnvoiced) {
            f.hz = 0.0;
            f.confidence = std::clamp(1.0 - d[size_t(best)], 0.0, 1.0);
            return;
        }
    }
    // Parabolic interpolation around the dip.
    double tau = best;
    if (best > 1 && best < tauMax + 1) {
        const double y0 = d[size_t(best - 1)], y1 = d[size_t(best)], y2 = d[size_t(best + 1)];
        const double denom = y0 - 2.0 * y1 + y2;
        if (std::abs(denom) > 1.0e-12) tau += std::clamp(0.5 * (y0 - y2) / denom, -0.5, 0.5);
    }
    f.hz = rate / tau;
    f.confidence = std::clamp(1.0 - d[size_t(best)], 0.0, 1.0);
}

struct Segment {
    int first = 0, last = 0;   // frame indices, inclusive
    double midi = 0.0;         // median sung pitch
    double spread = 0.0;       // semitones, steadiness
    double meanConfidence = 0.0;
    double peakDb = -120.0;
    bool joinsNext = false;    // no silence between it and the next one
};

void measure(Segment& s, const std::vector<Frame>& frames, const std::vector<double>& midi)
{
    // The median ignores scoops into the note and fall-offs at its end.
    std::vector<double> core;
    double conf = 0.0;
    int voiced = 0;
    s.peakDb = -120.0;
    for (int i = s.first; i <= s.last; ++i) {
        s.peakDb = std::max(s.peakDb, frames[size_t(i)].db);
        if (midi[size_t(i)] <= 0.0) continue;
        conf += frames[size_t(i)].confidence;
        ++voiced;
        core.push_back(midi[size_t(i)]);
    }
    s.midi = median(core);
    s.meanConfidence = voiced ? conf / voiced : 0.0;
    // Steadiness: how far the pitch wanders, with vibrato averaged out.
    std::vector<double> smooth;
    if (int(core.size()) >= kVibratoFrames) {
        double acc = 0.0;
        for (size_t q = 0; q < core.size(); ++q) {
            acc += core[q];
            if (q >= size_t(kVibratoFrames)) acc -= core[q - size_t(kVibratoFrames)];
            if (q + 1 >= size_t(kVibratoFrames)) smooth.push_back(acc / kVibratoFrames);
        }
    } else {
        smooth = core;
    }
    double var = 0.0;
    for (double m : smooth) var += (m - s.midi) * (m - s.midi);
    s.spread = smooth.empty() ? 0.0 : std::sqrt(var / double(smooth.size()));
}

// Krumhansl-Kessler key profiles.
constexpr std::array<double, 12> kMajor{ 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
constexpr std::array<double, 12> kMinor{ 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

double correlate(const std::array<double, 12>& hist, const std::array<double, 12>& profile, int tonic)
{
    const double mh = std::accumulate(hist.begin(), hist.end(), 0.0) / 12.0;
    const double mp = std::accumulate(profile.begin(), profile.end(), 0.0) / 12.0;
    double num = 0.0, dh = 0.0, dp = 0.0;
    for (int i = 0; i < 12; ++i) {
        const double h = hist[size_t((i + tonic) % 12)] - mh;
        const double p = profile[size_t(i)] - mp;
        num += h * p;
        dh += h * h;
        dp += p * p;
    }
    return dh > 0.0 && dp > 0.0 ? num / std::sqrt(dh * dp) : 0.0;
}

} // namespace

std::string keyName(int tonic, bool minor)
{
    static const char* kNames[12] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    return std::string(kNames[((tonic % 12) + 12) % 12]) + (minor ? " minor" : " major");
}

Frame detectPitch(const std::vector<float>& recent, double sampleRate, const Options& options)
{
    Frame f;
    if (recent.empty() || sampleRate <= 0.0) return f;
    const int factor = std::max(1, int(std::lround(sampleRate / kAnalysisRate)));
    const double rate = sampleRate / factor;
    const int tauMin = std::max(2, int(std::floor(rate / options.maxHz)));
    const int tauMax = std::max(tauMin + 2, int(std::ceil(rate / options.minHz)));
    const int window = std::max(tauMax, int(std::lround(rate * 0.02)));
    const size_t span = size_t(window + tauMax + 2);
    // Only the end matters; keep a little extra for the filter to settle.
    const size_t keep = std::min(recent.size(), (span + 64) * size_t(factor));
    std::vector<float> x = decimate(std::vector<float>(recent.end() - long(keep), recent.end()), factor);
    if (x.size() < span) return f;
    double mean = 0.0;
    for (auto v : x) mean += v;
    mean /= double(x.size());
    for (auto& v : x) v = float(v - mean);

    const size_t start = x.size() - span;
    double energy = 0.0;
    for (int j = 0; j < window; ++j) energy += double(x[start + size_t(j)]) * x[start + size_t(j)];
    const double rms = std::sqrt(energy / window);
    f.db = rms > 1.0e-9 ? 20.0 * std::log10(rms) : -120.0;
    f.seconds = double(recent.size()) / sampleRate;
    std::vector<double> scratch;
    yin(x, start, window, tauMin, tauMax, rate, scratch, f);
    if (f.db < kGateFloorDb) f.hz = 0.0;
    return f;
}

Result transcribe(const std::vector<float>& input, double sampleRate, const Options& options)
{
    Result result;
    if (input.empty() || sampleRate <= 0.0) return result;

    // ── Analysis signal: ~16 kHz, no DC ──
    const int factor = std::max(1, int(std::lround(sampleRate / kAnalysisRate)));
    std::vector<float> x = decimate(input, factor);
    const double rate = sampleRate / factor;
    {
        double prevIn = 0.0, prevOut = 0.0;
        const double r = 1.0 - 2.0 * kPi * 30.0 / rate;
        for (auto& v : x) {
            const double out = v - prevIn + r * prevOut;
            prevIn = v;
            prevOut = out;
            v = float(out);
        }
    }

    const int tauMin = std::max(2, int(std::floor(rate / options.maxHz)));
    const int tauMax = std::max(tauMin + 2, int(std::ceil(rate / options.minHz)));
    const int window = std::max(tauMax, int(std::lround(rate * 0.02)));
    const int hop = std::max(1, int(std::lround(rate * kHopSeconds)));
    const size_t span = size_t(window + tauMax + 2);
    if (x.size() < span) return result;

    // ── Frames: pitch, confidence, loudness ──
    std::vector<double> scratch;
    for (size_t start = 0; start + span <= x.size(); start += size_t(hop)) {
        Frame f;
        f.seconds = (double(start) + window / 2.0) / rate;
        double energy = 0.0;
        for (int j = 0; j < window; ++j) energy += double(x[start + size_t(j)]) * x[start + size_t(j)];
        const double rms = std::sqrt(energy / window);
        f.db = rms > 1.0e-9 ? 20.0 * std::log10(rms) : -120.0;
        yin(x, start, window, tauMin, tauMax, rate, scratch, f);
        result.frames.push_back(f);
    }
    auto& frames = result.frames;
    const int count = int(frames.size());

    double peakDb = -120.0;
    for (const auto& f : frames) peakDb = std::max(peakDb, f.db);
    const double gate = std::max(kGateFloorDb, peakDb - kGateBelowPeakDb);

    // Pitched frames, as MIDI (0 = none).
    std::vector<double> midi(size_t(count), 0.0);
    for (int i = 0; i < count; ++i) {
        auto& f = frames[size_t(i)];
        if (f.hz <= 0.0 || f.db < gate) {
            f.hz = 0.0;
            continue;
        }
        midi[size_t(i)] = hzToMidi(f.hz);
    }
    // Octave slips: a frame an octave off its neighbours' median joins them.
    {
        std::vector<double> fixed = midi;
        for (int i = 0; i < count; ++i) {
            if (midi[size_t(i)] <= 0.0) continue;
            std::vector<double> around;
            for (int k = std::max(0, i - 3); k <= std::min(count - 1, i + 3); ++k)
                if (k != i && midi[size_t(k)] > 0.0) around.push_back(midi[size_t(k)]);
            if (around.size() < 3) continue;
            const double m = median(around);
            const double diff = midi[size_t(i)] - m;
            if (std::abs(std::abs(diff) - 12.0) < 1.5 || std::abs(std::abs(diff) - 24.0) < 1.5)
                fixed[size_t(i)] = midi[size_t(i)] - 12.0 * std::round(diff / 12.0);
        }
        midi.swap(fixed);
    }

    // ── Runs of pitched frames (silence or a consonant between them) ──
    std::vector<Segment> segments;
    for (int i = 0; i < count;) {
        if (midi[size_t(i)] <= 0.0) { ++i; continue; }
        int last = i;
        int j = i + 1;
        while (j < count) {
            if (midi[size_t(j)] > 0.0) { last = j; ++j; continue; }
            int gap = 0;
            while (j + gap < count && midi[size_t(j + gap)] <= 0.0) ++gap;
            if (gap > kBridgeFrames || j + gap >= count) break;
            j += gap;
        }
        // Split the run at loudness dips ("da da") and at pitch steps.
        std::vector<int> cuts;
        const int dipReach = std::max(2, int(std::lround(kDipWindowSeconds / kHopSeconds)));
        for (int k = i + 2; k < last - 1; ++k) {
            const double here = frames[size_t(k)].db;
            if (here > frames[size_t(k - 1)].db || here > frames[size_t(k + 1)].db) continue;
            double left = -120.0, right = -120.0;
            for (int q = std::max(i, k - dipReach); q < k; ++q) left = std::max(left, frames[size_t(q)].db);
            for (int q = k + 1; q <= std::min(last, k + dipReach); ++q) right = std::max(right, frames[size_t(q)].db);
            if (left - here >= kDipDb && right - here >= kDipDb && (cuts.empty() || k - cuts.back() > 3))
                cuts.push_back(k);
        }
        std::vector<std::pair<int, int>> pieces;
        int from = i;
        for (int c : cuts) {
            pieces.push_back({ from, c - 1 });
            from = c + 1;
        }
        pieces.push_back({ from, last });

        for (size_t p = 0; p < pieces.size(); ++p) {
            auto [a, b] = pieces[p];
            // Pitch steps: the pitch moved and stayed for kStepFrames.
            int segStart = a;
            std::vector<double> sung;
            for (int k = a; k <= b; ++k) {
                if (midi[size_t(k)] > 0.0) sung.push_back(midi[size_t(k)]);
                if (sung.size() < 3 || k + kStepFrames > b) continue;
                const double current = median(sung);
                bool moved = true;
                std::vector<double> ahead;
                for (int q = k + 1; q <= k + kStepFrames; ++q) {
                    const double m = midi[size_t(q)];
                    if (m <= 0.0 || std::abs(m - current) < kStepSemitones) { moved = false; break; }
                    ahead.push_back(m);
                }
                if (!moved) continue;
                const auto [lo, hi] = std::minmax_element(ahead.begin(), ahead.end());
                if (*hi - *lo > 1.0 || (*lo < current) != (*hi < current)) continue;
                Segment s{ segStart, k };
                s.joinsNext = true;
                segments.push_back(s);
                segStart = k + 1;
                sung.clear();
            }
            Segment s{ segStart, b };   // a dip or a gap after it: a new attack, not legato
            segments.push_back(s);
        }
        i = std::max(j, last + 1);
    }
    for (auto& s : segments) measure(s, frames, midi);

    // ── Too short: legato slivers join a neighbour, lone blips go ──
    const int minFrames = std::max(1, int(std::lround(options.minNoteSeconds / kHopSeconds)));
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t k = 0; k < segments.size(); ++k) {
            auto& s = segments[k];
            if (s.last - s.first + 1 >= minFrames) continue;
            const bool prevJoins = k > 0 && segments[k - 1].joinsNext;
            const bool nextJoins = s.joinsNext && k + 1 < segments.size();
            if (!prevJoins && !nextJoins) {
                segments.erase(segments.begin() + long(k));
            } else {
                const bool intoPrev = prevJoins && (!nextJoins
                    || std::abs(segments[k - 1].midi - s.midi) <= std::abs(segments[k + 1].midi - s.midi));
                if (intoPrev) {
                    segments[k - 1].last = s.last;
                    segments[k - 1].joinsNext = s.joinsNext;
                    measure(segments[k - 1], frames, midi);
                } else {
                    segments[k + 1].first = s.first;
                    measure(segments[k + 1], frames, midi);
                }
                segments.erase(segments.begin() + long(k));
            }
            changed = true;
            break;
        }
    }
    // Legato neighbours that ended up on the same pitch are one note.
    for (size_t k = 0; k + 1 < segments.size();) {
        if (segments[k].joinsNext && std::abs(segments[k].midi - segments[k + 1].midi) < 0.5) {
            segments[k].last = segments[k + 1].last;
            segments[k].joinsNext = segments[k + 1].joinsNext;
            measure(segments[k], frames, midi);
            segments.erase(segments.begin() + long(k + 1));
        } else {
            ++k;
        }
    }
    if (segments.empty()) return result;

    // ── Tuning: the singer's overall offset (circular mean of the cents) ──
    double sx = 0.0, sy = 0.0;
    for (const auto& s : segments) {
        const double w = s.last - s.first + 1;
        const double frac = s.midi - std::round(s.midi);
        sx += w * std::cos(2.0 * kPi * frac);
        sy += w * std::sin(2.0 * kPi * frac);
    }
    const double offset = std::atan2(sy, sx) / (2.0 * kPi);   // semitones, -0.5..0.5
    result.tuningCents = offset * 100.0;

    // ── Notes ──
    double loudest = -120.0;
    for (const auto& s : segments) loudest = std::max(loudest, s.peakDb);
    std::vector<int> pitches;
    for (const auto& s : segments) {
        Note n;
        n.startSeconds = frames[size_t(s.first)].seconds - kHopSeconds / 2.0;
        n.endSeconds = frames[size_t(s.last)].seconds + kHopSeconds / 2.0;
        const double corrected = s.midi - offset;
        n.pitch = int(std::lround(corrected));
        n.cents = (corrected - n.pitch) * 100.0;
        const double steadiness = std::clamp(1.0 - s.spread / 1.0, 0.2, 1.0);
        const double offPitch = std::clamp(1.0 - std::abs(n.cents) / 50.0, 0.0, 1.0);
        n.confidence = std::clamp(s.meanConfidence * steadiness * (0.5 + 0.5 * offPitch)
                                      * (n.endSeconds - n.startSeconds < 0.1 ? 0.8 : 1.0), 0.0, 1.0);
        n.velocity = std::clamp(int(std::lround(100.0 + (s.peakDb - loudest) * 2.0)), 30, 120);
        result.notes.push_back(n);
        pitches.push_back(n.pitch);
    }

    // ── Octave: bring whistles (and very low hums) into range ──
    int shift = 0;
    if (options.autoOctave) {
        std::vector<double> p(pitches.begin(), pitches.end());
        const double mid = median(p);
        while (mid + shift > 84.0) shift -= 12;   // above C6
        while (mid + shift < 36.0) shift += 12;   // below C2
    }
    result.octaveShift = shift;
    for (auto& n : result.notes) n.pitch = std::clamp(n.pitch + shift + options.transpose, 0, 127);

    // ── Key, weighted by duration ──
    std::array<double, 12> hist{};
    for (const auto& n : result.notes) hist[size_t(n.pitch % 12)] += n.endSeconds - n.startSeconds;
    double best = -2.0;
    for (int tonic = 0; tonic < 12; ++tonic)
        for (bool minor : { false, true }) {
            const double c = correlate(hist, minor ? kMinor : kMajor, tonic);
            if (c > best) {
                best = c;
                result.keyTonic = tonic;
                result.keyMinor = minor;
            }
        }
    result.keyConfidence = best;
    return result;
}

} // namespace OpenDaw::Melody
