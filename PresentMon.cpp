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
#include <iterator>

#include <windows.h>
#include <psapi.h>
#include <shlwapi.h>
#include <sstream>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shlwapi.lib")

enum {
    MAX_HISTORY_TIME = 2000,
    MAX_DISPLAYED_HISTORY_TIME = 2000,
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
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, thisPid);
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
    case PresentMode::IndependentFlipMPO: return "iFlip MPO";
    case PresentMode::Fullscreen_Blit: return "Fullscreen Blit";
    case PresentMode::Windowed_Blit: return "Windowed Blit";
    case PresentMode::Legacy_Windowed_Blit: return "VistaBlit";
    case PresentMode::Composition_Buffer: return "Composition Buffer";
    default: return "Other";
    }
}

const char* RuntimeToString(Runtime rt)
{
    switch (rt) {
    case Runtime::DXGI: return "DXGI";
    case Runtime::D3D9: return "D3D9";
    default: return "Other";
    }
}

void PruneDeque(std::deque<PresentEvent> &presentHistory, uint64_t perfFreq, uint32_t msTimeDiff) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    while (!presentHistory.empty() &&
           ((double)(now.QuadPart - presentHistory.front().QpcTime) / perfFreq) * 1000 > msTimeDiff) {
        presentHistory.pop_front();
    }

    std::sort(presentHistory.begin(), presentHistory.end(),
              [](PresentEvent const& a, PresentEvent const& b) { return a.QpcTime < b.QpcTime; });
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
    chain.mPresentHistory.push_back(p);

    if (p.FinalState == PresentResult::Presented) {
        PruneDeque(chain.mDisplayedPresentHistory, perfFreq, MAX_DISPLAYED_HISTORY_TIME);
        chain.mDisplayedPresentHistory.push_back(p);
    }

    chain.mLastUpdateTicks = now;
    chain.mRuntime = p.Runtime;
    chain.mLastSyncInterval = p.SyncInterval;
    chain.mLastFlags = p.PresentFlags;
    chain.mLastPresentMode = p.PresentMode;

    if (pm.mOutputFile) {
        auto len = chain.mPresentHistory.size();
        if (len > 2) {
            auto& curr = chain.mPresentHistory[len - 1];
            auto& prev = chain.mPresentHistory[len - 2];
            double deltaMilliseconds = 1000 * double(curr.QpcTime - prev.QpcTime) / perfFreq;
            double deltaReady = curr.ReadyTime == 0 ? 0.0 : (1000 * double(curr.ReadyTime - curr.QpcTime) / perfFreq);
            double deltaDisplayed = curr.ScreenTime == 0 ? 0.0 : (1000 * double(curr.ReadyTime - curr.QpcTime) / perfFreq);
            double timeTakenMilliseconds = 1000 * double(curr.TimeTaken) / perfFreq;
            fprintf(pm.mOutputFile, "%s,%d,0x%016llX,%s,%.3lf,%.3lf,%d,%d,%s,%.3lf,%.3lf,%d\n",
                proc.mModuleName.c_str(), p.ProcessId, p.SwapChainAddress, RuntimeToString(p.Runtime),
                    deltaMilliseconds, timeTakenMilliseconds, curr.SyncInterval, curr.PresentFlags, PresentModeToString(curr.PresentMode),
                    deltaReady, deltaDisplayed, curr.FinalState != PresentResult::Discarded);
        }
    }
}

static double ComputeFps(std::deque<PresentEvent> const& presentHistory, uint64_t qpcFreq)
{
    // TODO: better method
    if (presentHistory.size() < 2) {
        return 0.0;
    }
    auto start = presentHistory.front().QpcTime;
    auto end = presentHistory.back().QpcTime;
    auto count = presentHistory.size() - 1;

    double deltaT = double(end - start) / qpcFreq;
    return count / deltaT;
}

static double ComputeDisplayedFps(SwapChainData& stats, uint64_t qpcFreq)
{
    return ComputeFps(stats.mDisplayedPresentHistory, qpcFreq);
}

static double ComputeFps(SwapChainData& stats, uint64_t qpcFreq)
{
    return ComputeFps(stats.mPresentHistory, qpcFreq);
}

static double ComputeLatency(SwapChainData& stats, uint64_t qpcFreq)
{
    if (stats.mDisplayedPresentHistory.size() < 2) {
        return 0.0;
    }

    uint64_t totalLatency = std::accumulate(stats.mDisplayedPresentHistory.begin(), stats.mDisplayedPresentHistory.end() - 1, 0ull,
                                   [](uint64_t current, PresentEvent const& e) { return current + e.ScreenTime - e.QpcTime; });
    double average = ((double)(totalLatency) / qpcFreq) / (stats.mDisplayedPresentHistory.size() - 1);
    return average;
}

static double ComputeCpuFrameTime(SwapChainData& stats, uint64_t qpcFreq)
{
    if (stats.mPresentHistory.size() < 2) {
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
        fprintf(pm.mOutputFile, "module,pid,chain,runtime,delta,timeTaken,vsync,flags,mode,deltaReady,deltaDisplayed,displayed\n");
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
            PruneDeque(chain.second.mPresentHistory, perfFreq, MAX_HISTORY_TIME);
            PruneDeque(chain.second.mDisplayedPresentHistory, perfFreq, MAX_DISPLAYED_HISTORY_TIME);

            double fps = ComputeFps(chain.second, perfFreq);
            double dispFps = ComputeDisplayedFps(chain.second, perfFreq);
            double cpuTime = ComputeCpuFrameTime(chain.second, perfFreq);
            double latency = ComputeLatency(chain.second, perfFreq);
            std::ostringstream planeString;
            if (chain.second.mLastPresentMode == PresentMode::IndependentFlipMPO) {
                planeString << " Plane " << chain.second.mDisplayedPresentHistory.back().PlaneIndex;
            }
            display += FormatString("\t%016llX (%s): SyncInterval %d | Flags %d | %.2lf ms/frame (%.1lf fps, %.1lf displayed fps, %.2lf ms CPU, %.2lf ms latency) (%s%s)%s\n",
                chain.first, RuntimeToString(chain.second.mRuntime), chain.second.mLastSyncInterval, chain.second.mLastFlags, 1000.0/fps, fps, dispFps, cpuTime * 1000.0, latency * 1000.0,
                PresentModeToString(chain.second.mLastPresentMode),
                planeString.str().c_str(),
                (now - chain.second.mLastUpdateTicks) > 1000 ? " [STALE]" : "");
        }
    }

    // refresh the console
    SetConsoleText(display.c_str());
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
