#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p)
{
    pluginInstance = p;

    p->addModel(modelSnes);
    p->addModel(modelSampler);
    p->addModel(modelApu);
    p->addModel(modelBender);
}
