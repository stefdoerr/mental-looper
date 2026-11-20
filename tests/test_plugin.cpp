#include "OuroborosPlugin.hpp"
#include "src/DistrhoPluginInternal.hpp"
#include "signals.hpp"
#include "test_util.hpp"

USE_NAMESPACE_DISTRHO

static bool requestStub(void*, uint32_t, float) { return true; }

static std::vector<float> gOut;

// Process vec in 128-frame blocks into gOut.
static void run(PluginExporter& plugin, const std::vector<float>& vec)
{
    gOut.assign(vec.size(), 0.0f);
    for (size_t i = 0; i < vec.size(); i += 128)
    {
        const uint32_t len = static_cast<uint32_t>(std::min<size_t>(128, vec.size() - i));
        const float* inputs[1]  = { vec.data() + i };
        float*       outputs[1] = { gOut.data() + i };
        plugin.run(inputs, outputs, len);
    }
}

static void skeletonIsMonoAndSilent(PluginExporter& plugin)
{
    CHECK(DISTRHO_PLUGIN_NUM_INPUTS == 1 && DISTRHO_PLUGIN_NUM_OUTPUTS == 1);
    // 0.1 s of sine in, nothing recorded -> silence out (loops only)
    plugin.activate();
    run(plugin, sine(220, 0.1f, 4800));
    CHECK(peak(gOut) == 0.0f);
}

int main()
{
    d_nextBufferSize = 128;
    d_nextSampleRate = 48000.0;
    d_nextCanRequestParameterValueChanges = true;
    PluginExporter plugin(nullptr, nullptr, requestStub, nullptr);

    skeletonIsMonoAndSilent(plugin);
    REPORT();
}
