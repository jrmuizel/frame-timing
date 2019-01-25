/*
Copyright 2017-2019 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <algorithm>
#include <shlwapi.h>

#include "PresentMon.hpp"
#include "LateStageReprojectionData.hpp"

template <typename Map, typename F>
static void map_erase_if(Map& m, F pred)
{
    typename Map::iterator i = m.begin();
    while ((i = std::find_if(i, m.end(), pred)) != m.end()) {
        m.erase(i++);
    }
}

static bool IsTargetProcess(CommandLineArgs const& args, uint32_t processId, char const* processName)
{
    // -exclude
    for (auto excludeProcessName : args.mExcludeProcessNames) {
        if (_stricmp(excludeProcessName, processName) == 0) {
            return false;
        }
    }

    // -capture_all
    if (args.mTargetPid == 0 && args.mTargetProcessNames.empty()) {
        return true;
    }

    // -process_id
    if (args.mTargetPid != 0 && args.mTargetPid == processId) {
        return true;
    }

    // -process_name
    for (auto targetProcessName : args.mTargetProcessNames) {
        if (_stricmp(targetProcessName, processName) == 0) {
            return true;
        }
    }

    return false;
}

static void TerminateProcess(PresentMonData& pm, ProcessInfo const& proc)
{
    if (!proc.mTargetProcess) {
        return;
    }

    // Save the output files in case the process is re-started
    if (pm.mArgs->mMultiCsv) {
        pm.mProcessOutputFiles.emplace(proc.mModuleName, std::make_pair(proc.mOutputFile, proc.mLsrOutputFile));
    }

    // Quit if this is the last process tracked for -terminate_on_proc_exit
    if (pm.mArgs->mTerminateOnProcExit) {
        pm.mTerminationProcessCount -= 1;
        if (pm.mTerminationProcessCount == 0) {
            PostStopRecording();
            PostQuitProcess();
        }
    }
}

static void StopProcess(PresentMonData& pm, std::map<uint32_t, ProcessInfo>::iterator it)
{
    TerminateProcess(pm, it->second);
    pm.mProcessMap.erase(it);
}

static void StopProcess(PresentMonData& pm, uint32_t processId)
{
    auto it = pm.mProcessMap.find(processId);
    if (it != pm.mProcessMap.end()) {
        StopProcess(pm, it);
    }
}

static ProcessInfo* StartNewProcess(PresentMonData& pm, ProcessInfo* proc, uint32_t processId, std::string const& imageFileName, uint64_t now)
{
    proc->mModuleName = imageFileName;
    proc->mOutputFile = nullptr;
    proc->mLsrOutputFile = nullptr;
    proc->mLastRefreshTicks = now;
    proc->mTargetProcess = IsTargetProcess(*pm.mArgs, processId, imageFileName.c_str());

    if (!proc->mTargetProcess) {
        return nullptr;
    }

    // Create any CSV files that need process info to be created
    CreateProcessCSVs(pm, proc, imageFileName);

    // Include process in -terminate_on_proc_exit count
    if (pm.mArgs->mTerminateOnProcExit) {
        pm.mTerminationProcessCount += 1;
    }

    return proc;
}

static ProcessInfo* StartProcess(PresentMonData& pm, uint32_t processId, std::string const& imageFileName, uint64_t now)
{
    auto it = pm.mProcessMap.find(processId);
    if (it != pm.mProcessMap.end()) {
        StopProcess(pm, it);
    }

    auto proc = &pm.mProcessMap.emplace(processId, ProcessInfo()).first->second;
    return StartNewProcess(pm, proc, processId, imageFileName, now);
}

static ProcessInfo* StartProcessIfNew(PresentMonData& pm, uint32_t processId, uint64_t now)
{
    auto it = pm.mProcessMap.find(processId);
    if (it != pm.mProcessMap.end()) {
        auto proc = &it->second;
        return proc->mTargetProcess ? proc : nullptr;
    }

    std::string imageFileName("<error>");
    if (!pm.mArgs->mEtlFileName) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (h) {
            char path[MAX_PATH] = "<error>";
            char* name = path;
            DWORD numChars = sizeof(path);
            if (QueryFullProcessImageNameA(h, 0, path, &numChars) == TRUE) {
                name = PathFindFileNameA(path);
            }
            imageFileName = name;
            CloseHandle(h);
        }
    }

    auto proc = &pm.mProcessMap.emplace(processId, ProcessInfo()).first->second;
    return StartNewProcess(pm, proc, processId, imageFileName, now);
}

static bool UpdateProcessInfo_Realtime(PresentMonData& pm, ProcessInfo& info, uint64_t now, uint32_t thisPid)
{
    // Check periodically if the process has exited
    if (now - info.mLastRefreshTicks > 1000) {
        info.mLastRefreshTicks = now;

        auto running = false;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, thisPid);
        if (h) {
            char path[MAX_PATH] = "<error>";
            char* name = path;
            DWORD numChars = sizeof(path);
            if (QueryFullProcessImageNameA(h, 0, path, &numChars) == TRUE) {
                name = PathFindFileNameA(path);
            }
            if (info.mModuleName.compare(name) != 0) {
                // Image name changed, which means that our process exited and another
                // one started with the same PID.
                TerminateProcess(pm, info);
                StartNewProcess(pm, &info, thisPid, name, now);
            }

            DWORD dwExitCode = 0;
            if (GetExitCodeProcess(h, &dwExitCode) == TRUE && dwExitCode == STILL_ACTIVE) {
                running = true;
            }
            CloseHandle(h);
        }

        if (!running) {
            return false;
        }
    }

    // remove chains without recent updates
    map_erase_if(info.mChainMap, [now](const std::pair<const uint64_t, SwapChainData>& entry) {
        return entry.second.IsStale(now);
    });

    return true;
}

void AddPresent(PresentMonData& pm, PresentEvent& p, uint64_t now, uint64_t perfFreq)
{
    auto proc = StartProcessIfNew(pm, p.ProcessId, now);
    if (proc == nullptr) {
        return; // process is not a target
    }

    auto& chain = proc->mChainMap[p.SwapChainAddress];
    chain.AddPresentToSwapChain(p);

    UpdateCSV(pm, proc, chain, p, perfFreq);

    chain.UpdateSwapChainInfo(p, now, perfFreq);
}

void PresentMon_Init(const CommandLineArgs& args, PresentMonData& pm)
{
    pm.mArgs = &args;

    if (!args.mEtlFileName)
    {
        QueryPerformanceCounter((PLARGE_INTEGER)&pm.mStartupQpcTime);
    }
    else
    {
        // Reading events from ETL file so current QPC value is irrelevant. Update this 
        // later from first event in the file.
        pm.mStartupQpcTime = 0;
    }

    // Create any CSV files that don't need process info to be created
    CreateNonProcessCSVs(pm);
}

void AddLateStageReprojection(PresentMonData& pm, LateStageReprojectionData& lsr, LateStageReprojectionEvent& p, uint64_t now, uint64_t perfFreq)
{
    const uint32_t appProcessId = p.GetAppProcessId();
    auto proc = StartProcessIfNew(pm, appProcessId, now);
    if (proc == nullptr) {
        return; // process is not a target
    }

    if ((pm.mArgs->mVerbosity > Verbosity::Simple) && (appProcessId == 0)) {
        return; // Incomplete event data
    }

    lsr.AddLateStageReprojection(p);

    UpdateLSRCSV(pm, lsr, proc, p, perfFreq);

    lsr.UpdateLateStageReprojectionInfo(now, perfFreq);
}

void PresentMon_Update(PresentMonData& pm, LateStageReprojectionData& lsr, std::vector<std::shared_ptr<PresentEvent>>& presents, std::vector<std::shared_ptr<LateStageReprojectionEvent>>& lsrs, uint64_t now, uint64_t perfFreq)
{
    // store the new presents into processes
    for (auto& p : presents)
    {
        AddPresent(pm, *p, now, perfFreq);
    }

    // store the new lsrs
    for (auto& p : lsrs)
    {
        AddLateStageReprojection(pm, lsr, *p, now, perfFreq);
    }

    // Update realtime process info
    if (!pm.mArgs->mEtlFileName) {
        std::vector<std::map<uint32_t, ProcessInfo>::iterator> remove;
        for (auto ii = pm.mProcessMap.begin(), ie = pm.mProcessMap.end(); ii != ie; ++ii) {
            if (!UpdateProcessInfo_Realtime(pm, ii->second, now, ii->first)) {
                remove.emplace_back(ii);
            }
        }
        for (auto ii : remove) {
            StopProcess(pm, ii);
        }
    }

    // Display information to console
    if (!pm.mArgs->mSimpleConsole) {
        std::string display;
        UpdateConsole(pm, now, perfFreq, &display);
        UpdateConsole(pm, lsr, now, perfFreq, &display);
        SetConsoleText(display.c_str());
    }
}

void PresentMon_Shutdown(PresentMonData& pm, uint32_t totalEventsLost, uint32_t totalBuffersLost)
{
    CloseCSVs(pm, totalEventsLost, totalBuffersLost);

    pm.mProcessMap.clear();

    if (pm.mArgs->mSimpleConsole == false) {
        SetConsoleText("");
    }
}

void EtwConsumingThread(const CommandLineArgs& args)
{
    Sleep(args.mDelay * 1000);
    if (EtwThreadsShouldQuit()) {
        return;
    }

    PresentMonData pmData;
    LateStageReprojectionData lsrData;
    PMTraceConsumer pmConsumer(args.mVerbosity == Verbosity::Simple);
    MRTraceConsumer mrConsumer(args.mVerbosity == Verbosity::Simple);

    TraceSession session;

    if (args.mIncludeWindowsMixedReality) {
        session.AddProviderAndHandler(DHD_PROVIDER_GUID, TRACE_LEVEL_VERBOSE, 0x1C00000, 0, (EventHandlerFn)&HandleDHDEvent, &mrConsumer);
        if (args.mVerbosity != Verbosity::Simple) {
            session.AddProviderAndHandler(SPECTRUMCONTINUOUS_PROVIDER_GUID, TRACE_LEVEL_VERBOSE, 0x800000, 0, (EventHandlerFn)&HandleSpectrumContinuousEvent, &mrConsumer);
        }
    }

    session.AddProviderAndHandler(DXGI_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 0, 0, (EventHandlerFn) &HandleDXGIEvent, &pmConsumer);
    session.AddProviderAndHandler(D3D9_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 0, 0, (EventHandlerFn) &HandleD3D9Event, &pmConsumer);
    if (args.mVerbosity != Verbosity::Simple) {
        session.AddProviderAndHandler(DXGKRNL_PROVIDER_GUID,   TRACE_LEVEL_INFORMATION, 1,      0, (EventHandlerFn) &HandleDXGKEvent,   &pmConsumer);
        session.AddProviderAndHandler(WIN32K_PROVIDER_GUID,    TRACE_LEVEL_INFORMATION, 0x1000, 0, (EventHandlerFn) &HandleWin32kEvent, &pmConsumer);
        session.AddProviderAndHandler(DWM_PROVIDER_GUID,       TRACE_LEVEL_VERBOSE,     0,      0, (EventHandlerFn) &HandleDWMEvent,    &pmConsumer);
        session.AddProviderAndHandler(Win7::DWM_PROVIDER_GUID, TRACE_LEVEL_VERBOSE, 0, 0, (EventHandlerFn) &HandleDWMEvent, &pmConsumer);
        session.AddProvider(Win7::DXGKRNL_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 1, 0);
    }
    session.AddHandler(EventMetadataGuid,             (EventHandlerFn) &HandleMetadataEvent,            &pmConsumer);
    session.AddHandler(NT_PROCESS_EVENT_GUID,         (EventHandlerFn) &HandleNTProcessEvent,           &pmConsumer);
    session.AddHandler(Win7::DXGKBLT_GUID,            (EventHandlerFn) &Win7::HandleDxgkBlt,            &pmConsumer);
    session.AddHandler(Win7::DXGKFLIP_GUID,           (EventHandlerFn) &Win7::HandleDxgkFlip,           &pmConsumer);
    session.AddHandler(Win7::DXGKPRESENTHISTORY_GUID, (EventHandlerFn) &Win7::HandleDxgkPresentHistory, &pmConsumer);
    session.AddHandler(Win7::DXGKQUEUEPACKET_GUID,    (EventHandlerFn) &Win7::HandleDxgkQueuePacket,    &pmConsumer);
    session.AddHandler(Win7::DXGKVSYNCDPC_GUID,       (EventHandlerFn) &Win7::HandleDxgkVSyncDPC,       &pmConsumer);
    session.AddHandler(Win7::DXGKMMIOFLIP_GUID,       (EventHandlerFn) &Win7::HandleDxgkMMIOFlip,       &pmConsumer);

    if (!(args.mEtlFileName == nullptr
        ? session.InitializeRealtime(args.mSessionName, args.mStopExistingSession, &EtwThreadsShouldQuit)
        : session.InitializeEtlFile(args.mEtlFileName, &EtwThreadsShouldQuit))) {
        PostStopRecording();
        PostQuitProcess();
        return;
    }

    if (args.mSimpleConsole) {
        printf("Started recording.\n");
    }
    if (args.mScrollLockIndicator) {
        EnableScrollLock(true);
    }

    {
        // Launch the ETW producer thread
        StartConsumerThread(session.traceHandle_);

        // Consume / Update based on the ETW output
        {
            PresentMon_Init(args, pmData);
            auto timerRunning = args.mTimer > 0;
            auto timerEnd = GetTickCount64() + args.mTimer * 1000;

            std::vector<std::shared_ptr<PresentEvent>> presents;
            std::vector<std::shared_ptr<LateStageReprojectionEvent>> lsrs;
            std::vector<NTProcessEvent> ntProcessEvents;

            uint32_t totalEventsLost = 0;
            uint32_t totalBuffersLost = 0;
            for (;;) {
#if _DEBUG
                if (args.mSimpleConsole) {
                    printf(".");
                }
#endif

                presents.clear();
                lsrs.clear();
                ntProcessEvents.clear();

                // If we are reading events from ETL file set start time to match time stamp of first event
                if (pmData.mArgs->mEtlFileName && pmData.mStartupQpcTime == 0)
                {
                    pmData.mStartupQpcTime = session.startTime_;
                }

                uint64_t now = GetTickCount64();

                // Dequeue any captured NTProcess events; if ImageFileName is
                // empty then the process stopped, otherwise it started.
                pmConsumer.DequeueProcessEvents(ntProcessEvents);
                for (auto ntProcessEvent : ntProcessEvents) {
                    if (!ntProcessEvent.ImageFileName.empty()) {
                        StartProcess(pmData, ntProcessEvent.ProcessId, ntProcessEvent.ImageFileName, now);
                    }
                }

                pmConsumer.DequeuePresents(presents);
                mrConsumer.DequeueLSRs(lsrs);
                if (args.mScrollLockToggle && (GetKeyState(VK_SCROLL) & 1) == 0) {
                    presents.clear();
                    lsrs.clear();
                }

                auto doneProcessingEvents = EtwThreadsShouldQuit();
                PresentMon_Update(pmData, lsrData, presents, lsrs, now, session.frequency_);

                for (auto ntProcessEvent : ntProcessEvents) {
                    if (ntProcessEvent.ImageFileName.empty()) {
                        StopProcess(pmData, ntProcessEvent.ProcessId);
                    }
                }

                uint32_t eventsLost = 0;
                uint32_t buffersLost = 0;
                if (session.CheckLostReports(&eventsLost, &buffersLost)) {
                    printf("Lost %u events, %u buffers.", eventsLost, buffersLost);

                    totalEventsLost += eventsLost;
                    totalBuffersLost += buffersLost;

                    // FIXME: How do we set a threshold here?
                    if (eventsLost > 100) {
                        PostStopRecording();
                        PostQuitProcess();
                    }
                }

                if (timerRunning) {
                    if (GetTickCount64() >= timerEnd) {
                        PostStopRecording();
                        if (args.mTerminateAfterTimer) {
                            PostQuitProcess();
                        }
                        timerRunning = false;
                    }
                }

                if (doneProcessingEvents) {
                    break;
                }

                Sleep(100);
            }

            PresentMon_Shutdown(pmData, totalEventsLost, totalBuffersLost);
        }

        if (IsConsumerThreadRunning()) {
            session.Stop();
        }

        WaitForConsumerThreadToExit();
    }

    session.Finalize();

    if (args.mScrollLockIndicator) {
        EnableScrollLock(false);
    }
    if (args.mSimpleConsole) {
        printf("Stopping recording.\n");
    }
}

