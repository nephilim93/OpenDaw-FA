#include "FaustStarters.h"

namespace OpenDaw::Modular {

const std::vector<CodeStarter>& codeStarters()
{
    static const std::vector<CodeStarter> starters = {
        { "bitcrusher", "Bitcrusher: fewer bits and a lower sample rate for lo-fi grit and aliasing.", R"faust(import("stdfaust.lib");
bits = hslider("bits", 8, 1, 16, 1);
ds   = hslider("downsample", 4, 1, 32, 1);
mix  = hslider("mix", 1, 0, 1, 0.01);
cnt  = (+(1) : %(max(1, int(ds)))) ~ _;
hold(x) = select2(cnt == 0, _, x) ~ _;
quant(x) = floor(x * L + 0.5) / L with { L = pow(2, bits - 1); };
process = _ <: (quant : hold : *(mix)), *(1 - mix) :> max(-1) : min(1);
)faust" },
        { "saturator", "Saturator: smooth cubic soft clipping; offset adds even harmonics.", R"faust(import("stdfaust.lib");
drive  = hslider("drive", 0.5, 0, 1, 0.01);
offset = hslider("offset", 0, 0, 0.5, 0.01);
mix    = hslider("mix", 1, 0, 1, 0.01);
process = _ <: (ef.cubicnl(drive, offset) : *(mix)), *(1 - mix) :> _;
)faust" },
        { "phaser", "Phaser: four swept notches with feedback.", R"faust(import("stdfaust.lib");
rate  = hslider("rate", 0.3, 0.01, 5, 0.01);
depth = hslider("depth", 1, 0, 1, 0.01);
fb    = hslider("feedback", 0.5, -0.95, 0.95, 0.01);
process = pf.phaser2_mono(4, 0, 1000, 200, 1.5, 5000, rate, depth, fb, 0);
)faust" },
        { "flanger", "Flanger: a short swept delay with feedback, for jet-like sweeps.", R"faust(import("stdfaust.lib");
rate  = hslider("rate", 0.2, 0.01, 5, 0.01);
ms    = hslider("delay", 3, 0.5, 10, 0.1);
depth = hslider("depth", 0.7, 0, 1, 0.01);
fb    = hslider("feedback", 0.5, -0.95, 0.95, 0.01);
del   = ba.sec2samp(ms / 1000) * (1 + os.osc(rate)) * 0.5;
process = pf.flanger_mono(2048, del, depth, fb, 0);
)faust" },
        { "compressor", "Compressor: evens out levels; makeup gain brings the level back up.", R"faust(import("stdfaust.lib");
thresh  = hslider("threshold", -18, -60, 0, 0.1);
ratio   = hslider("ratio", 4, 1, 20, 0.1);
attack  = hslider("attack", 0.005, 0.0005, 0.1, 0.0005);
release = hslider("release", 0.1, 0.01, 1, 0.01);
makeup  = hslider("makeup", 6, 0, 24, 0.1);
process = co.compressor_mono(ratio, thresh, attack, release) : *(ba.db2linear(makeup)) : max(-1) : min(1);
)faust" },
        { "ladder", "Ladder filter: a Moog-style 24 dB/octave lowpass with resonance.", R"faust(import("stdfaust.lib");
cutoff = hslider("cutoff", 1200, 40, 12000, 1);
res    = hslider("resonance", 0.5, 0, 0.95, 0.01);
process = ve.moog_vcf_2bn(res, si.smoo(cutoff));
)faust" },
    };
    return starters;
}

} // namespace OpenDaw::Modular
