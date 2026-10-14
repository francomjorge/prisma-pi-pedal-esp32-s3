#include "stream_control.h"

static volatile bool s_enabled = false;

bool prisma_appliance_stream_enabled(void)
{
    return s_enabled;
}

void prisma_appliance_stream_set(bool enabled)
{
    s_enabled = enabled;
}
