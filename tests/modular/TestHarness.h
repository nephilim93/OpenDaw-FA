#pragma once

#include "modular/CompiledPatch.h"
#include "modular/Module.h"
#include "modular/ModuleRegistry.h"
#include "modular/Patch.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// A minimal test runner: tests register themselves by group, CHECK records a
// failure and carries on, and main() exits non-zero if any check failed.
namespace OpenDaw::Tests {

using TestFn = void (*)();

struct TestCase {
    const char* group;
    const char* name;
    TestFn fn;
};

std::vector<TestCase>& registry();
void reportFailure(const char* file, int line, const std::string& message);

struct Registrar {
    Registrar(const char* group, const char* name, TestFn fn) { registry().push_back({ group, name, fn }); }
};

inline std::string str(const QString& s) { return s.toStdString(); }
inline std::string str(const std::string& s) { return s; }
inline std::string str(const char* s) { return s; }
inline std::string str(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

} // namespace OpenDaw::Tests

#define OD_CAT2(a, b) a##b
#define OD_CAT(a, b) OD_CAT2(a, b)

#define TEST(group, name)                                                                  \
    static void OD_CAT(test_, OD_CAT(group, OD_CAT(_, name)))();                           \
    static const ::OpenDaw::Tests::Registrar OD_CAT(reg_, OD_CAT(group, OD_CAT(_, name)))( \
        #group, #name, &OD_CAT(test_, OD_CAT(group, OD_CAT(_, name))));                    \
    static void OD_CAT(test_, OD_CAT(group, OD_CAT(_, name)))()

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) ::OpenDaw::Tests::reportFailure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

// Like CHECK, with extra context (anything str() accepts) in the failure message.
#define CHECK_MSG(cond, msg)                                                           \
    do {                                                                               \
        if (!(cond))                                                                   \
            ::OpenDaw::Tests::reportFailure(__FILE__, __LINE__,                        \
                std::string("CHECK(" #cond ") ") + ::OpenDaw::Tests::str(msg));         \
    } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                                        \
    do {                                                                               \
        const double od_a_ = double(actual), od_e_ = double(expected);                 \
        if (!(std::abs(od_a_ - od_e_) <= double(tolerance)))                           \
            ::OpenDaw::Tests::reportFailure(__FILE__, __LINE__,                        \
                std::string(#actual " = ") + ::OpenDaw::Tests::str(od_a_)              \
                + ", expected " + ::OpenDaw::Tests::str(od_e_) + " +/- "              \
                + ::OpenDaw::Tests::str(double(tolerance)));                           \
    } while (0)

namespace OpenDaw::Tests {

namespace M = OpenDaw::Modular;

constexpr double kRate = 48000.0;

// Builds a patch from JSON, failing the test (and returning an empty patch)
// if it doesn't parse.
M::Patch patchFrom(const QJsonObject& json);

// Shorthands for patch JSON.
QJsonObject mod(const char* id, const char* type, const QJsonObject& params = {},
                const char* section = nullptr);
QJsonObject cable(const char* from, const char* to);
QJsonObject patchJson(const QJsonArray& modules, const QJsonArray& cables);

// Renders a compiled patch the way a host does: fixed-size blocks with the
// transport advancing between them. Returns stereo output.
struct Stereo {
    std::vector<float> left, right;
};
Stereo renderPatch(M::CompiledPatch& cp, int numSamples, M::Transport transport = {},
                   int blockSize = 512);

// Runs one module type directly, without a patch around it.
class ModuleRunner {
public:
    explicit ModuleRunner(const char* type, double sampleRate = kRate);
    void setParam(const char* name, float value);
    // Processes len samples. inputs: one vector per connected input (by name),
    // missing ones read zeros. Returns one vector per output.
    std::vector<std::vector<float>> run(int len,
                                        const std::vector<std::pair<const char*, std::vector<float>>>& inputs = {},
                                        const M::Transport& transport = {});
    M::Module& module() { return *module_; }

private:
    const M::ModuleDef* def_ = nullptr;
    std::unique_ptr<M::Module> module_;
    std::vector<float> params_;
    double sampleRate_;
};

// Sample indices where the signal crosses 0.5 upwards.
std::vector<int> risingEdges(const std::vector<float>& signal, int from = 0, int to = -1);

bool allFinite(const std::vector<float>& signal);
double rms(const std::vector<float>& signal, int from, int to);

} // namespace OpenDaw::Tests
