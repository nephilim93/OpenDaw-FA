#include "ModuleHelpers.h"
#include "ModuleLibrary.h"

#if OPENDAW_WITH_FAUST
#include <faust/dsp/llvm-dsp.h>
#include <faust/gui/UI.h>
#include <faust/gui/meta.h>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <regex>
#include <thread>
#endif

// "faust" code modules: DSP written in Faust and compiled at runtime by
// libfaust's LLVM backend. Without OPENDAW_WITH_FAUST only the placeholder
// definition exists, so patches with code modules still load (silently).

namespace OpenDaw::Modular {

namespace {

// The placeholder has no outputs, so it has nothing to write.
class SilentModule : public Module {
public:
    void process(const ProcessContext&, const float* const*, const bool*, float* const*,
                 const float*) override {}
};

#if OPENDAW_WITH_FAUST

static_assert(sizeof(FAUSTFLOAT) == sizeof(float), "the modular engine runs on floats");

std::string checkSource(const std::string& source)
{
    if (source.find_first_not_of(" \t\r\n") == std::string::npos)
        return "The Faust code is empty; it needs at least a 'process = ...;' line.";
    if (int(source.size()) > kMaxCodeSize)
        return "The Faust code is " + std::to_string(source.size()) + " bytes; the limit is "
               + std::to_string(kMaxCodeSize) + ".";

    // These call native code or read files, which a patch must never do.
    static const std::regex banned(R"(\b(ffunction|fconstant|fvariable|soundfile)\b)");
    std::smatch match;
    if (std::regex_search(source, match, banned)) {
        const auto before = source.begin() + match.position(0);
        const int line = 1 + int(std::count(source.begin(), before, '\n'));
        return "line " + std::to_string(line) + ": '" + match.str(1)
               + "' is not allowed in code modules.";
    }
    return {};
}

// libfaust's compiler and factory table are not safe to use from two threads.
std::mutex& faustMutex()
{
    static std::mutex m;
    return m;
}

// Owns a compiled factory; shared by the definition and every DSP instance.
struct Factory {
    explicit Factory(llvm_dsp_factory* f) : factory(f) {}
    ~Factory()
    {
        const std::lock_guard<std::mutex> lock(faustMutex());
        deleteDSPFactory(factory);
    }
    llvm_dsp_factory* factory;
};

// Walks a DSP's controls in declaration order. The definition and every
// instance use it, so parameter i is the same control everywhere.
class ControlCollector : public UI {
public:
    std::vector<ParamDef> params;
    std::vector<FAUSTFLOAT*> zones;

    void openTabBox(const char*) override {}
    void openHorizontalBox(const char*) override {}
    void openVerticalBox(const char*) override {}
    void closeBox() override {}

    void addButton(const char* label, FAUSTFLOAT* zone) override
    {
        add(label, zone, 0.0f, 1.0f, 0.0f, "Button: 1 while pressed");
    }
    void addCheckButton(const char* label, FAUSTFLOAT* zone) override
    {
        add(label, zone, 0.0f, 1.0f, 0.0f, "Checkbox: 0 or 1");
    }
    void addVerticalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min,
                           FAUSTFLOAT max, FAUSTFLOAT) override
    {
        add(label, zone, min, max, init, {});
    }
    void addHorizontalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min,
                             FAUSTFLOAT max, FAUSTFLOAT) override
    {
        add(label, zone, min, max, init, {});
    }
    void addNumEntry(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT init, FAUSTFLOAT min,
                     FAUSTFLOAT max, FAUSTFLOAT) override
    {
        add(label, zone, min, max, init, {});
    }
    // Bargraphs are outputs for a GUI; there is nowhere to show them.
    void addHorizontalBargraph(const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
    void addVerticalBargraph(const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
    void addSoundfile(const char*, const char*, Soundfile**) override {}

    // Faust declares a control's metadata just before adding it.
    void declare(FAUSTFLOAT* zone, const char* key, const char* value) override
    {
        if (zone && key && value && std::string(key) == "unit") units_[zone] = value;
        if (zone && key && value && std::string(key) == "tooltip") tooltips_[zone] = value;
    }

private:
    void add(const char* label, FAUSTFLOAT* zone, float min, float max, float init,
             std::string description)
    {
        // Labels can repeat across groups; parameter names can't.
        const std::string base = label && *label ? label : "param" + std::to_string(params.size() + 1);
        std::string name = base;
        for (int n = 2; std::any_of(params.begin(), params.end(),
                                    [&](const ParamDef& p) { return p.name == name; }); ++n)
            name = base + "_" + std::to_string(n);

        if (max < min) std::swap(min, max);
        if (auto it = tooltips_.find(zone); it != tooltips_.end()) description = it->second;
        ParamDef def{ name, min, max, detail::clampf(init, min, max), {}, {}, description };
        if (auto it = units_.find(zone); it != units_.end()) def.unit = it->second;
        params.push_back(std::move(def));
        zones.push_back(zone);
    }

    std::map<FAUSTFLOAT*, std::string> units_, tooltips_;
};

class DescriptionReader : public Meta {
public:
    std::string name, description;
    void declare(const char* key, const char* value) override
    {
        if (!key || !value) return;
        if (std::string(key) == "name") name = value;
        if (std::string(key) == "description") description = value;
    }
};

// One running DSP instance. Created on the message thread; process() and
// reset() run on the audio thread and only touch preallocated memory.
class FaustModule : public Module {
public:
    FaustModule(std::shared_ptr<Factory> factory, int numInputs, int numOutputs)
        : factory_(std::move(factory)),
          inputCopies_(size_t(numInputs)), inputs_(size_t(numInputs)), outputs_(size_t(numOutputs))
    {
        const std::lock_guard<std::mutex> lock(faustMutex());
        dsp_.reset(factory_->factory->createDSPInstance());
        if (!dsp_) return;
        ControlCollector controls;
        dsp_->buildUserInterface(&controls);
        zones_ = controls.zones;
        for (size_t i = 0; i < inputs_.size(); ++i) inputs_[i] = inputCopies_[i].data();
    }

    ~FaustModule() override
    {
        const std::lock_guard<std::mutex> lock(faustMutex());
        dsp_.reset();
    }

    void prepare(double sampleRate) override
    {
        if (dsp_) dsp_->init(int(std::lround(sampleRate)));
    }

    void reset() override
    {
        if (dsp_) dsp_->instanceClear();
    }

    void process(const ProcessContext& ctx, const float* const* inputs, const bool*,
                 float* const* outputs, const float* params) override
    {
        const int n = ctx.numSamples;
        if (!dsp_) {
            for (size_t o = 0; o < outputs_.size(); ++o) std::fill(outputs[o], outputs[o] + n, 0.0f);
            return;
        }
        for (size_t i = 0; i < zones_.size(); ++i) *zones_[i] = params[i];
        // Copied, so a cable from this module's own output can't alias its input.
        for (size_t i = 0; i < inputs_.size(); ++i)
            std::copy(inputs[i], inputs[i] + n, inputCopies_[i].begin());
        for (size_t o = 0; o < outputs_.size(); ++o) outputs_[o] = outputs[o];

        dsp_->compute(n, inputs_.data(), outputs_.data());

        // A NaN would poison every module downstream (and stay in feedback
        // loops), so it becomes silence and the DSP's state restarts.
        bool bad = false;
        for (size_t o = 0; o < outputs_.size(); ++o)
            for (int i = 0; i < n; ++i)
                if (!std::isfinite(outputs[o][i])) { outputs[o][i] = 0.0f; bad = true; }
        if (bad) dsp_->instanceClear();
    }

private:
    std::shared_ptr<Factory> factory_;
    std::unique_ptr<dsp> dsp_;
    std::vector<FAUSTFLOAT*> zones_;
    std::vector<std::array<float, kSubBlock>> inputCopies_;
    std::vector<FAUSTFLOAT*> inputs_, outputs_;
};

// "f1 : 3 : ERROR : syntax error" -> "line 3: syntax error". Errors inside
// an imported library keep the library's name.
std::string formatErrors(const std::string& name, const std::string& raw)
{
    static const std::regex located(R"(^(.*?)\s*:\s*(\d+)\s*:\s*ERROR\s*:\s*(.*)$)");
    std::string out;
    size_t start = 0;
    while (start < raw.size()) {
        size_t end = raw.find('\n', start);
        if (end == std::string::npos) end = raw.size();
        std::string line = raw.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        if (line.empty()) continue;
        std::smatch m;
        if (std::regex_match(line, m, located))
            line = (m.str(1) == name ? std::string() : m.str(1) + " ") + "line " + m.str(2) + ": "
                   + m.str(3);
        else if (line.rfind("ERROR : ", 0) == 0)
            line.erase(0, 8);
        out += (out.empty() ? "" : "; ") + line;
    }
    if (out.empty()) return "the Faust compiler failed without a message.";
    return out.back() == '.' ? out : out + ".";
}

std::string portName(const char* base, int index, int count)
{
    return count == 1 ? base : base + std::to_string(index + 1);
}

// Compiles on a worker thread so a pathological program can't hang the
// message thread. A compile that times out finishes (and is freed) in the
// background.
llvm_dsp_factory* compileWithTimeout(const std::string& name, const std::string& source,
                                     double timeoutSeconds, std::string& error)
{
    struct Job {
        std::mutex m;
        std::condition_variable done;
        bool finished = false, abandoned = false;
        llvm_dsp_factory* factory = nullptr;
        std::string error;
    };
    auto job = std::make_shared<Job>();

    std::thread([job, name, source] {
        std::vector<const char*> args{ "-ftz", "2" };  // flush denormals in decaying filters
#ifdef OPENDAW_FAUST_LIBRARY_DIR
        args.push_back("-I");
        args.push_back(OPENDAW_FAUST_LIBRARY_DIR);
#endif
        std::string err;
        llvm_dsp_factory* factory = nullptr;
        {
            const std::lock_guard<std::mutex> lock(faustMutex());
            factory = createDSPFactoryFromString(name, source, int(args.size()), args.data(),
                                                 "", err, -1);
        }
        const std::lock_guard<std::mutex> lock(job->m);
        if (job->abandoned) {
            if (factory) {
                const std::lock_guard<std::mutex> faustLock(faustMutex());
                deleteDSPFactory(factory);
            }
            return;
        }
        job->factory = factory;
        job->error = err;
        job->finished = true;
        job->done.notify_all();
    }).detach();

    std::unique_lock<std::mutex> lock(job->m);
    const auto timeout = std::chrono::duration<double>(std::max(0.0, timeoutSeconds));
    if (!job->done.wait_for(lock, timeout, [&] { return job->finished; })) {
        job->abandoned = true;
        char seconds[32];
        std::snprintf(seconds, sizeof seconds, "%g", timeoutSeconds);
        error = std::string("compiling took longer than ") + seconds
                + " s; simplify the code (smaller delays, tables and loops).";
        return nullptr;
    }
    if (!job->factory) error = formatErrors(name, job->error);
    return job->factory;
}

CodeCompileResult buildDefinition(const std::string& name, const std::string& source,
                                  double timeoutSeconds)
{
    std::string error;
    llvm_dsp_factory* raw = compileWithTimeout(name, source, timeoutSeconds, error);
    if (!raw) return { nullptr, error };
    auto factory = std::make_shared<Factory>(raw);

    // A probe instance reports the ports and controls.
    int numInputs = 0, numOutputs = 0;
    ControlCollector controls;
    DescriptionReader meta;
    {
        const std::lock_guard<std::mutex> lock(faustMutex());
        std::unique_ptr<dsp> probe(raw->createDSPInstance());
        if (!probe) return { nullptr, "libfaust could not create an instance of the DSP." };
        numInputs = probe->getNumInputs();
        numOutputs = probe->getNumOutputs();
        probe->buildUserInterface(&controls);
        probe->metadata(&meta);
    }

    if (numOutputs == 0)
        return { nullptr, "'process' has no outputs, so the module would do nothing." };
    if (numInputs > kMaxCodePorts || numOutputs > kMaxCodePorts)
        return { nullptr, "'process' has " + std::to_string(numInputs) + " inputs and "
                          + std::to_string(numOutputs) + " outputs; the limit is "
                          + std::to_string(kMaxCodePorts) + " each." };
    if (int(controls.params.size()) > kMaxCodeParams)
        return { nullptr, "the code has " + std::to_string(controls.params.size())
                          + " controls; the limit is " + std::to_string(kMaxCodeParams) + "." };

    auto def = std::make_shared<ModuleDef>();
    def->type = kCodeModuleType;
    def->description = !meta.description.empty() ? meta.description
                       : !meta.name.empty()      ? "Faust code module: " + meta.name
                                                 : "Faust code module";
    def->section = Section::Both;
    for (int i = 0; i < numInputs; ++i)
        def->inputs.push_back({ portName("in", i, numInputs), PortKind::Audio, {} });
    for (int i = 0; i < numOutputs; ++i)
        def->outputs.push_back({ portName("out", i, numOutputs), PortKind::Audio, {} });
    def->params = controls.params;
    def->create = [factory, numInputs, numOutputs]() -> std::unique_ptr<Module> {
        return std::make_unique<FaustModule>(factory, numInputs, numOutputs);
    };
    return { std::move(def), {} };
}

#endif // OPENDAW_WITH_FAUST

} // namespace

bool codeModulesAvailable()
{
#if OPENDAW_WITH_FAUST
    return true;
#else
    return false;
#endif
}

CodeCompileResult compileCodeModule(const std::string& name, const std::string& source,
                                    double timeoutSeconds)
{
#if OPENDAW_WITH_FAUST
    if (auto problem = checkSource(source); !problem.empty()) return { nullptr, problem };

    // Every patch edit re-reads the whole patch, so unchanged code reuses
    // its compiled definition while any patch still holds it.
    static std::mutex cacheMutex;
    static std::map<std::string, std::weak_ptr<const ModuleDef>> cache;
    {
        const std::lock_guard<std::mutex> lock(cacheMutex);
        for (auto it = cache.begin(); it != cache.end();)
            it = it->second.expired() ? cache.erase(it) : std::next(it);
        if (auto it = cache.find(source); it != cache.end())
            if (auto def = it->second.lock()) return { def, {} };
    }
    auto result = buildDefinition(name, source, timeoutSeconds);
    if (result.def) {
        const std::lock_guard<std::mutex> lock(cacheMutex);
        cache[source] = result.def;
    }
    return result;
#else
    (void)name; (void)source; (void)timeoutSeconds;
    return { nullptr, "this build of OpenDaw has no Faust support (configure it with "
                      "-DOPENDAW_WITH_FAUST=ON and libfaust installed)." };
#endif
}

void addCodeModules(std::vector<ModuleDef>& defs)
{
    defs.push_back({ kCodeModuleType,
        "Code module: its DSP is Faust source, written with write_modular_module. Ports and "
        "parameters come from the code: one audio input is 'in', several are 'in1'..'inN' "
        "(outputs likewise 'out' or 'out1'..'outN'); each hslider, vslider, nentry, button "
        "and checkbox becomes a parameter named by its label, with its range and default.",
        Section::Both, {}, {}, {}, detail::make<SilentModule>() });
}

} // namespace OpenDaw::Modular
