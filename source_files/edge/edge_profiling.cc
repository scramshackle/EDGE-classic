#include "edge_profiling.h"

#include <string>

#include "epi.h"
#include "epi_filesystem.h"
#include "i_system.h"
#include "m_argv.h"

#ifdef EDGE_PROFILING

static Remotery   *profiler_instance   = nullptr;
static uint32_t    profiler_frame_hash = 0;
static bool        profiler_frame_open = false;
static std::string profiler_log_path;

void ProfilerStartup(void)
{
    if (profiler_instance != nullptr)
        return;

    rmtSettings *settings = rmt_Settings();

    settings->reuse_open_port                = RMT_TRUE;
    settings->limit_connections_to_localhost = RMT_TRUE;
#ifdef _WIN32
    settings->enableThreadSampler = RMT_FALSE;
#endif

    profiler_log_path = ArgumentValue("profile_log");

    if (!profiler_log_path.empty())
    {
        if (!epi::IsDirectory(profiler_log_path) && !epi::MakeDirectory(profiler_log_path))
        {
            LogWarning("Profiler: cannot create trace directory %s\n", profiler_log_path.c_str());
            profiler_log_path.clear();
        }
        else
            settings->logPath = profiler_log_path.c_str();
    }

    rmtError error = rmt_CreateGlobalInstance(&profiler_instance);

    if (error != RMT_ERROR_NONE)
    {
        LogWarning("Profiler: Remotery failed to start (%s)\n", rmt_GetLastErrorMessage());
        profiler_instance = nullptr;
        return;
    }

    rmt_SetCurrentThreadName("Main");

    LogPrint("Profiler: Remotery listening on localhost:%d\n", (int)settings->port);

    if (!profiler_log_path.empty())
        LogPrint("Profiler: writing trace files to %s\n", profiler_log_path.c_str());
}

void ProfilerFrameMark(void)
{
    if (profiler_instance == nullptr)
        return;

    if (profiler_frame_open)
        _rmt_EndCPUSample();

    _rmt_BeginCPUSample("Frame", RMTSF_Root, &profiler_frame_hash);

    profiler_frame_open = true;
}

void ProfilerShutdown(void)
{
    if (profiler_instance == nullptr)
        return;

    if (profiler_frame_open)
    {
        _rmt_EndCPUSample();
        profiler_frame_open = false;
    }

    rmt_DestroyGlobalInstance(profiler_instance);

    profiler_instance = nullptr;
}

#else

void ProfilerStartup(void)
{
    if (FindArgument("profile_log") > 0)
        LogWarning("Profiler: -profile_log ignored, built without EDGE_PROFILING\n");
}

void ProfilerShutdown(void)
{
}

#endif
