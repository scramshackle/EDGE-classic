#pragma once

void ProfilerStartup(void);
void ProfilerShutdown(void);

#ifdef EDGE_PROFILING

#include "Remotery.h"

class ProfilerZone
{
  public:
    ProfilerZone(const char *name, uint32_t *hash_cache, bool active) : active_(active)
    {
        if (active_)
            _rmt_BeginCPUSample(name, RMTSF_None, hash_cache);
    }

    ~ProfilerZone()
    {
        if (active_)
            _rmt_EndCPUSample();
    }

    ProfilerZone(const ProfilerZone &)            = delete;
    ProfilerZone &operator=(const ProfilerZone &) = delete;

  private:
    bool active_;
};

void ProfilerFrameMark(void);

#define EDGE_ZoneNamedN(varname, name, active)                                                                         \
    static uint32_t varname##_hash_cache = 0;                                                                          \
    ProfilerZone    varname(name, &varname##_hash_cache, active)

#define EDGE_ZoneScopedN(name) EDGE_ZoneNamedN(edge_profiler_zone, name, true)
#define EDGE_ZoneScoped        EDGE_ZoneNamedN(edge_profiler_zone, __func__, true)
#define EDGE_FrameMark         ProfilerFrameMark()

#else

#define EDGE_ZoneNamedN(varname, name, active)
#define EDGE_ZoneScopedN(name)
#define EDGE_ZoneScoped
#define EDGE_FrameMark

#endif
