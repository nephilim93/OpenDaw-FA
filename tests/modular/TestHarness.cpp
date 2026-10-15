#include "TestHarness.h"
#include <algorithm>
#include <cstring>

namespace OpenDaw::Tests {

namespace {
int failures = 0;
const TestCase* current = nullptr;
}

std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

void reportFailure(const char* file, int line, const std::string& message)
{
    ++failures;
    std::printf("  FAIL %s.%s  %s:%d  %s\n", current ? current->group : "?",
                current ? current->name : "?", file, line, message.c_str());
}

M::Patch patchFrom(const QJsonObject& json)
{
    QString error;
    auto patch = M::Patch::fromJson(json, &error);
    CHECK_MSG(patch.has_value(), error);
    return patch ? *patch : M::Patch{};
}

QJsonObject mod(const char* id, const char* type, const QJsonObject& params, const char* section)
{
    QJsonObject m{ { "id", id }, { "type", type } };
    if (!params.isEmpty()) m["params"] = params;
    if (section) m["section"] = section;
    return m;
}

QJsonObject cable(const char* from, const char* to)
{
    return { { "from", from }, { "to", to } };
}

QJsonObject patchJson(const QJsonArray& modules, const QJsonArray& cables)
{
    return { { "modules", modules }, { "cables", cables } };
}

Stereo renderPatch(M::CompiledPatch& cp, int numSamples, M::Transport transport, int blockSize)
{
    Stereo out;
    out.left.assign(size_t(numSamples), 0.0f);
    out.right.assign(size_t(numSamples), 0.0f);
    const float macros[M::kNumMacros] = {};
    for (int start = 0; start < numSamples; start += blockSize) {
        const int n = std::min(blockSize, numSamples - start);
        cp.render(out.left.data() + start, out.right.data() + start, n, macros, transport);
        transport.beat += n * transport.bpm / 60.0 / kRate;
    }
    return out;
}

ModuleRunner::ModuleRunner(const char* type, double sampleRate)
    : def_(M::findModule(type)), sampleRate_(sampleRate)
{
    CHECK_MSG(def_ != nullptr, std::string("module type ") + type);
    if (!def_) return;
    module_ = def_->create();
    module_->prepare(sampleRate);
    module_->reset();
    for (const auto& p : def_->params) params_.push_back(p.defaultValue);
}

void ModuleRunner::setParam(const char* name, float value)
{
    const int i = def_ ? def_->paramIndex(name) : -1;
    CHECK_MSG(i >= 0, std::string("param ") + name);
    if (i >= 0) params_[size_t(i)] = value;
}

std::vector<std::vector<float>> ModuleRunner::run(
    int len, const std::vector<std::pair<const char*, std::vector<float>>>& inputs,
    const M::Transport& transport)
{
    if (!def_) return {};
    std::vector<std::vector<float>> outs(def_->outputs.size(), std::vector<float>(size_t(len), 0.0f));
    const std::vector<float> zeros(M::kSubBlock, 0.0f);
    const size_t numIn = def_->inputs.size();
    std::vector<const float*> in(numIn, zeros.data());
    bool connected[M::CompiledPatch::kMaxInputs] = {};
    std::vector<float*> out(outs.size());

    M::ProcessContext ctx;
    ctx.sampleRate = sampleRate_;
    ctx.transport = transport;
    const double beatsPerSample = transport.bpm / 60.0 / sampleRate_;

    for (int start = 0; start < len; start += M::kSubBlock) {
        ctx.numSamples = std::min(M::kSubBlock, len - start);
        ctx.transport.beat = transport.beat + start * beatsPerSample;
        for (const auto& [name, signal] : inputs) {
            const int i = def_->inputIndex(name);
            CHECK_MSG(i >= 0, std::string("input ") + name);
            if (i < 0) continue;
            in[size_t(i)] = signal.data() + start;
            connected[i] = true;
        }
        for (size_t o = 0; o < outs.size(); ++o) out[o] = outs[o].data() + start;
        module_->process(ctx, in.data(), connected, out.data(), params_.data());
    }
    return outs;
}

std::vector<int> risingEdges(const std::vector<float>& signal, int from, int to)
{
    if (to < 0) to = int(signal.size());
    std::vector<int> edges;
    bool last = from > 0 && signal[size_t(from - 1)] > 0.5f;
    for (int i = from; i < to; ++i) {
        const bool high = signal[size_t(i)] > 0.5f;
        if (high && !last) edges.push_back(i);
        last = high;
    }
    return edges;
}

bool allFinite(const std::vector<float>& signal)
{
    return std::all_of(signal.begin(), signal.end(), [](float v) { return std::isfinite(v); });
}

double rms(const std::vector<float>& signal, int from, int to)
{
    double sum = 0.0;
    for (int i = from; i < to; ++i) sum += double(signal[size_t(i)]) * signal[size_t(i)];
    return to > from ? std::sqrt(sum / (to - from)) : 0.0;
}

// Runs every test whose group or "group.name" matches an argument (all tests
// when there are none). Returns the number of failed checks.
int runTests(int argc, char** argv)
{
    int ran = 0, failedTests = 0;
    for (const auto& test : registry()) {
        const std::string full = std::string(test.group) + "." + test.name;
        bool selected = argc <= 1;
        for (int a = 1; a < argc; ++a)
            if (full == argv[a] || std::strcmp(test.group, argv[a]) == 0) selected = true;
        if (!selected) continue;

        current = &test;
        const int before = failures;
        test.fn();
        current = nullptr;
        ++ran;
        const bool ok = failures == before;
        if (!ok) ++failedTests;
        std::printf("%s %s\n", ok ? "  ok  " : "FAILED", full.c_str());
    }
    if (ran == 0) {
        // A typo in a test name must not pass silently.
        std::printf("No tests matched.\n");
        return 1;
    }
    std::printf("%d tests, %d failed (%d failed checks)\n", ran, failedTests, failures);
    return failures;
}

} // namespace OpenDaw::Tests

int main(int argc, char** argv)
{
    return OpenDaw::Tests::runTests(argc, argv) == 0 ? 0 : 1;
}
