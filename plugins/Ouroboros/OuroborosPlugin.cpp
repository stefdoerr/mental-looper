#include "OuroborosPlugin.hpp"

START_NAMESPACE_DISTRHO

Plugin* createPlugin()
{
    return new OuroborosPlugin();
}

END_NAMESPACE_DISTRHO
