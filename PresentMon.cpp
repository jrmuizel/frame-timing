//--------------------------------------------------------------------------------------
// Copyright 2015 Intel Corporation
// All Rights Reserved
//
// Permission is granted to use, copy, distribute and prepare derivative works of this
// software for any purpose and without fee, provided, that the above copyright notice
// and this statement appear in all copies.  Intel makes no representations about the
// suitability of this software for any purpose.  THIS SOFTWARE IS PROVIDED "AS IS."
// INTEL SPECIFICALLY DISCLAIMS ALL WARRANTIES, EXPRESS OR IMPLIED, AND ALL LIABILITY,
// INCLUDING CONSEQUENTIAL AND OTHER INDIRECT DAMAGES, FOR THE USE OF THIS SOFTWARE,
// INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PROPRIETARY RIGHTS, AND INCLUDING THE
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.  Intel does not
// assume any responsibility for any errors which may appear in this software nor any
// responsibility to update it.
//--------------------------------------------------------------------------------------

#include "PresentMon.hpp"
#include "Util.hpp"
#include <algorithm>
#include <numeric>
#include <ctime>

#include <windows.h>
#include <psapi.h>
#include <shlwapi.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shlwapi.lib")

enum {
    MAX_HISTORY_LENGTH = 60,
    CHAIN_TIMEOUT_THRESHOLD_TICKS = 10000 // 10 sec
};

template <typename Map, typename F>
static void map_erase_if(Map& m, F pred)
{
    typename Map::iterator i = m.begin();
    while ((i = std::find_if(i, m.end(), pred)) != m.end())
        m.erase(i++);
}

static void UpdateProcessInfo(ProcessInfo& info, uint64_t now, uint32_t thisPid)
{
    if (now - info.mLastRefreshTicks > 1000) {
        info.mLastRefreshTicks = now;
        char path[MAX_PATH] = "<error>";
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, thisPid);
        if (h) {
            GetModuleFileNameExA(h, NULL, path, sizeof(path) - 1);
            std::string name = PathFindFileNameA(path);
            if (name != info.mModuleName) {
                info.mChainMap.clear();
                info.mModuleName = name;
            }
            CloseHandle(h);
        } else {
            info.mChainMap.clear();
        }
    }
    // remove chains without recent updates
    map_erase_if(info.mChainMap, [now](const std::pair<const uint64_t, SwapChainData>& entry) {
        return now - entry.second.mLastUpdateTicks > CHAIN_TIMEOUT_THRESHOLD_TICKS;
    });
}

const char* PresentModeToString(PresentMode mode)
{
    switch (mode) {
    case PresentMode::Fullscreen: return "Fullscreen";
    case PresentMode::Composed_Flip: return "Windowed Flip";
    case PresentMode::DirectFlip: return "DirectFlip";
    case PresentMode::IndependentFlip: return "IndependentFlip";
    case PresentMode::ImmediateIndependentFlip: return "Immediate iFlip";
    default: return "Other";
    }
}

void AddPresent(PresentMonData& pm, PresentEvent& p, uint64_t now, uint64_t perfFreq)
{
    auto& proc = pm.mProcessMap[p.ProcessId];
    if (!proc.mLastRefreshTicks) {
        UpdateProcessInfo(proc, now, p.ProcessId);
    }

    if (pm.mArgs->mTargetProcessName && strcmp(pm.mArgs->mTargetProcessName, "*") &&
        _stricmp(pm.mArgs->mTargetProcessName, proc.mModuleName.c_str())) {
        // process name does not match
        return;
    }
    if (pm.mArgs->mTargetPid && p.ProcessId != pm.mArgs->mTargetPid) {
        return;
    }

    auto& chain = proc.mChainMap[p.SwapChainAddress];
    if (chain.mPresentHistory.size() > MAX_HISTORY_LENGTH) {
        chain.mPresentHistory.pop_front();
    }
    chain.mPresentHistory.push_back(p);
    chain.mLastUpdateTicks = now;
    chain.mLastSyncInterval = p.SyncInterval;
    chain.mLastFlags = p.PresentFlags;
    chain.mLastPresentMode = p.PresentMode;

    if (pm.mOutputFile) {
        auto len = chain.mPresentHistory.size();
        if (len > 2) {
            auto& curr = chain.mPresentHistory[len - 1];
            auto& prev = chain.mPresentHistory[len - 2];
            double deltaMilliseconds = 1000 * double(curr.QpcTime - prev.QpcTime) / perfFreq;
            double timeTakenMilliseconds = 1000 * double(curr.TimeTaken) / perfFreq;
            fprintf(pm.mOutputFile, "%s,%d,0x%016llX,%.3lf,%.3lf,%d,%d,%s\n",
                proc.mModuleName.c_str(), p.ProcessId, p.SwapChainAddress, deltaMilliseconds, timeTakenMilliseconds, curr.SyncInterval, curr.PresentFlags, PresentModeToString(curr.PresentMode));
        }
    }
}

static double ComputeFps(SwapChainData& stats, uint64_t qpcFreq)
{
    // TODO: better method
    auto start = stats.mPresentHistory.front().QpcTime;
    auto end = stats.mPresentHistory.back().QpcTime;
    auto count = stats.mPresentHistory.size() - 1;

    double deltaT = double(end - start) / qpcFreq;
    return count / deltaT;
}

static double ComputeCpuFrameTime(SwapChainData& stats, uint64_t qpcFreq)
{
    if (stats.mPresentHistory.empty())
    {
        return 0.0;
    }

    uint64_t timeInPresent = std::accumulate(stats.mPresentHistory.begin(), stats.mPresentHistory.end() - 1, 0ull,
                                   [](uint64_t current, PresentEvent const& e) { return current + e.TimeTaken; });
    uint64_t totalTime = stats.mPresentHistory.back().QpcTime - stats.mPresentHistory.front().QpcTime;
    
    double timeNotInPresent = double(totalTime - timeInPresent) / qpcFreq;
    return timeNotInPresent / (stats.mPresentHistory.size() - 1);
}

void PresentMon_Init(const PresentMonArgs& args, PresentMonData& pm)
{
    pm.mArgs = &args;

    if (args.mOutputFileName) {
        fopen_s(&pm.mOutputFile, args.mOutputFileName, "w");
    } else if (args.mTargetProcessName) {
        struct tm tm;
        time_t time_now = time(NULL);
        localtime_s(&tm, &time_now);
        std::string date = FormatString("%4d-%02d-%02d-%02d-%02d",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
        std::string path;
        if (strchr(args.mTargetProcessName, '*')) {
            path = FormatString("PresentMon-%s.csv", date.c_str());
        } else {
            path = FormatString("PresentMon-%s-%s.csv", args.mTargetProcessName, date.c_str());
        }
        fopen_s(&pm.mOutputFile, path.c_str(), "w");
    }

    if (pm.mOutputFile) {
        fprintf(pm.mOutputFile, "module,pid,chain,delta,timeTaken,vsync,flags,mode\n");
    }
}

void PresentMon_Update(PresentMonData& pm, std::vector<std::shared_ptr<PresentEvent>> presents, uint64_t perfFreq)
{
    std::string display;
    uint64_t now = GetTickCount64();

    // store the new presents into processes
    for (auto& p : presents)
    {
        AddPresent(pm, *p, now, perfFreq);
    }

    // update all processes
    for (auto& proc : pm.mProcessMap)
    {
        UpdateProcessInfo(proc.second, now, proc.first);
        if (proc.second.mModuleName.empty() ||
            proc.second.mChainMap.empty())
        {
            // don't display empty processes
            continue;
        }
        display += FormatString("%s[%d]:\n", proc.second.mModuleName.c_str(),proc.first);
        for (auto& chain : proc.second.mChainMap)
        {
            double fps = ComputeFps(chain.second, perfFreq);
            double cpuTime = ComputeCpuFrameTime(chain.second, perfFreq);
            display += FormatString("\t%016llX: SyncInterval %d | Flags %d | %.2lf ms/frame (%.1lf fps, %.2lf ms CPU) (%s)%s\n",
                chain.first, chain.second.mLastSyncInterval, chain.second.mLastFlags, 1000.0/fps, fps, cpuTime * 1000.0,
                PresentModeToString(chain.second.mLastPresentMode),
                (now - chain.second.mLastUpdateTicks) > 1000 ? " [STALE]" : "");
        }
    }

    // refresh the console
    ClearConsole();
    printf(display.c_str());
}

void PresentMon_Shutdown(PresentMonData& pm)
{
    if (pm.mOutputFile)
    {
        fclose(pm.mOutputFile);
        pm.mOutputFile = nullptr;
    }
    pm.mProcessMap.clear();
}
