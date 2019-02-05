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

#include "PresentMon.hpp"

#include <algorithm>
#include <shlwapi.h>
#include <thread>

static std::thread gThread;
static bool gQuit = false;

// When we collect realtime ETW events, we don't receive the events in real
// time but rather sometime after they occur.  Since the user might be toggling
// recording based on realtime cues (e.g., watching the target application) we
// maintain a history of realtime record toggle events from the user.  When we
// consider recording an event, we can look back and see what the recording
// state was at the time the event actually occurred.
//
// gRecordingToggleHistory is a vector of QueryPerformanceCounter() values at
// times when the recording state changed, and gIsRecording is the recording
// state at the current time.
//
// CRITICAL_SECTION used as this is expected to have low contention (e.g., *no*
// contention when capturing from ETL).

static CRITICAL_SECTION gRecordingToggleCS;
static std::vector<uint64_t> gRecordingToggleHistory;
static bool gIsRecording = false;

void SetOutputRecordingState(bool record)
{
    auto const& args = GetCommandLineArgs();

    if (gIsRecording == record) {
        return;
    }

    // When capturing from an ETL file, just use the current recording state.
    // It's not clear how best to map realtime to ETL QPC time, and there
    // aren't any realtime cues in this case.
    if (args.mEtlFileName != nullptr) {
        EnterCriticalSection(&gRecordingToggleCS);
        gIsRecording = record;
        LeaveCriticalSection(&gRecordingToggleCS);
        return;
    }

    uint64_t qpc = 0;
    QueryPerformanceCounter((LARGE_INTEGER*) &qpc);

    EnterCriticalSection(&gRecordingToggleCS);
    gRecordingToggleHistory.emplace_back(qpc);
    gIsRecording = record;
    LeaveCriticalSection(&gRecordingToggleCS);
}

static bool CopyRecordingToggleHistory(std::vector<uint64_t>* recordingToggleHistory)
{
    EnterCriticalSection(&gRecordingToggleCS);
    recordingToggleHistory->assign(gRecordingToggleHistory.begin(), gRecordingToggleHistory.end());
    auto isRecording = gIsRecording;
    LeaveCriticalSection(&gRecordingToggleCS);

    auto recording = recordingToggleHistory->size() + (isRecording ? 1 : 0);
    return (recording & 1) == 1;
}

// Remove recording toggle events that we've processed.
static void UpdateRecordingToggles(size_t nextIndex)
{
    if (nextIndex > 0) {
        EnterCriticalSection(&gRecordingToggleCS);
        gRecordingToggleHistory.erase(gRecordingToggleHistory.begin(), gRecordingToggleHistory.begin() + nextIndex);
        LeaveCriticalSection(&gRecordingToggleCS);
    }
}

template <typename Map, typename F>
static void map_erase_if(Map& m, F pred)
{
    typename Map::iterator i = m.begin();
    while ((i = std::find_if(i, m.end(), pred)) != m.end()) {
        m.erase(i++);
    }
}

static bool IsTargetProcess(uint32_t processId, char const* processName)
{
    auto const& args = GetCommandLineArgs();

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
    auto const& args = GetCommandLineArgs();

    if (!proc.mTargetProcess) {
        return;
    }

    // Save the output files in case the process is re-started
    if (args.mMultiCsv) {
        pm.mProcessOutputFiles.emplace(proc.mModuleName, std::make_pair(proc.mOutputFile, proc.mLsrOutputFile));
    }

    // Quit if this is the last process tracked for -terminate_on_proc_exit
    if (args.mTerminateOnProcExit) {
        pm.mTerminationProcessCount -= 1;
        if (pm.mTerminationProcessCount == 0) {
            ExitMainThread();
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
    auto const& args = GetCommandLineArgs();

    proc->mModuleName = imageFileName;
    proc->mOutputFile = nullptr;
    proc->mLsrOutputFile = nullptr;
    proc->mLastRefreshTicks = now;
    proc->mTargetProcess = IsTargetProcess(processId, imageFileName.c_str());

    if (!proc->mTargetProcess) {
        return nullptr;
    }

    // Create any CSV files that need process info to be created
    CreateProcessCSVs(pm, proc, imageFileName);

    // Include process in -terminate_on_proc_exit count
    if (args.mTerminateOnProcExit) {
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
    auto const& args = GetCommandLineArgs();

    auto it = pm.mProcessMap.find(processId);
    if (it != pm.mProcessMap.end()) {
        auto proc = &it->second;
        return proc->mTargetProcess ? proc : nullptr;
    }

    std::string imageFileName("<error>");
    if (!args.mEtlFileName) {
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

static void UpdateNTProcesses(PresentMonData& pmData, uint64_t updateTime, std::vector<NTProcessEvent> const& ntProcessEvents)
{
    for (auto ntProcessEvent : ntProcessEvents) {
        if (!ntProcessEvent.ImageFileName.empty()) { // Empty ImageFileName indicates the process terminated
            StartProcess(pmData, ntProcessEvent.ProcessId, ntProcessEvent.ImageFileName, updateTime);
        }
    }
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

static void AddPresents(PresentMonData& pm, uint64_t updateTime, std::vector<std::shared_ptr<PresentEvent>> const& presentEvents, size_t* presentEventIndex,
                        bool recording, bool checkStopQpc, uint64_t stopQpc, bool* hitStopQpc)
{
    auto i = *presentEventIndex;
    for (auto n = presentEvents.size(); i < n; ++i) {
        auto presentEvent = presentEvents[i];

        // Stop processing events if we hit the next stop time.
        if (checkStopQpc && presentEvent->QpcTime >= stopQpc) {
            *hitStopQpc = true;
            break;
        }

        auto proc = StartProcessIfNew(pm, presentEvent->ProcessId, updateTime);
        if (proc == nullptr) {
            continue; // process is not a target
        }

        auto& chain = proc->mChainMap[presentEvent->SwapChainAddress];
        chain.AddPresentToSwapChain(*presentEvent);

        if (recording) {
            UpdateCSV(pm, proc, chain, *presentEvent);
        }

        chain.UpdateSwapChainInfo(*presentEvent, updateTime);
    }

    *presentEventIndex = i;
}

static void AddPresents(PresentMonData& pm, uint64_t updateTime, LateStageReprojectionData* lsrData,
                        std::vector<std::shared_ptr<LateStageReprojectionEvent>> const& presentEvents, size_t* presentEventIndex,
                        bool recording, bool checkStopQpc, uint64_t stopQpc, bool* hitStopQpc)
{
    auto const& args = GetCommandLineArgs();

    auto i = *presentEventIndex;
    for (auto n = presentEvents.size(); i < n; ++i) {
        auto presentEvent = presentEvents[i];

        // Stop processing events if we hit the next stop time.
        if (checkStopQpc && presentEvent->QpcTime >= stopQpc) {
            *hitStopQpc = true;
            break;
        }

        const uint32_t appProcessId = presentEvent->GetAppProcessId();
        auto proc = StartProcessIfNew(pm, appProcessId, updateTime);
        if (proc == nullptr) {
            continue; // process is not a target
        }

        if ((args.mVerbosity > Verbosity::Simple) && (appProcessId == 0)) {
            continue; // Incomplete event data
        }

        lsrData->AddLateStageReprojection(*presentEvent);

        if (recording) {
            UpdateLSRCSV(pm, *lsrData, proc, *presentEvent);
        }

        lsrData->UpdateLateStageReprojectionInfo(updateTime);
    }

    *presentEventIndex = i;
}

static void ProcessEvents(
    PresentMonData& pmData,
    uint64_t updateTime,
    LateStageReprojectionData* lsrData,
    std::vector<NTProcessEvent>* ntProcessEvents,
    std::vector<std::shared_ptr<PresentEvent>>* presentEvents,
    std::vector<std::shared_ptr<LateStageReprojectionEvent>>* lsrEvents,
    std::vector<uint64_t>* recordingToggleHistory)
{
    auto const& args = GetCommandLineArgs();

    // Copy any analyzed information from ConsumerThread.
    DequeueAnalyzedInfo(ntProcessEvents, presentEvents, lsrEvents);

    // Copy the record range history form the MainThread.
    auto recording = CopyRecordingToggleHistory(recordingToggleHistory);

    // Process NTProcess events. We don't have to worry about the recording
    // toggles here because NTProcess events are only captured when parsing ETL
    // files, and we don't use recording toggle history for ETL files.
    UpdateNTProcesses(pmData, updateTime, *ntProcessEvents);

    // Next, iterate through the recording toggles (if any)...
    size_t presentEventIndex = 0;
    size_t lsrEventIndex = 0;
    size_t recordingToggleIndex = 0;
    for (;;) {
        auto checkRecordingToggle   = recordingToggleIndex < recordingToggleHistory->size();
        auto nextRecordingToggleQpc = checkRecordingToggle ? (*recordingToggleHistory)[recordingToggleIndex] : 0ull;
        auto hitNextRecordingToggle = false;

        // Process present events up until the next recording toggle.  If
        // we reached the toggle, handle it and continue.  Otherwise, we're
        // done handling all the events (and any outstanding toggles will
        // have to wait for next batch of events).
        AddPresents(pmData, updateTime, *presentEvents, &presentEventIndex, recording, checkRecordingToggle, nextRecordingToggleQpc, &hitNextRecordingToggle);
        AddPresents(pmData, updateTime, lsrData, *lsrEvents, &lsrEventIndex, recording, checkRecordingToggle, nextRecordingToggleQpc, &hitNextRecordingToggle);
        if (!hitNextRecordingToggle) {
            break;
        }

        // Toggle recording.
        recordingToggleIndex += 1;
        recording = !recording;
    }

    // Update realtime process info.
    if (!args.mEtlFileName) {
        std::vector<std::map<uint32_t, ProcessInfo>::iterator> remove;
        for (auto ii = pmData.mProcessMap.begin(), ie = pmData.mProcessMap.end(); ii != ie; ++ii) {
            if (!UpdateProcessInfo_Realtime(pmData, ii->second, updateTime, ii->first)) {
                remove.emplace_back(ii);
            }
        }
        for (auto ii : remove) {
            StopProcess(pmData, ii);
        }
    }

    // Display information to console if requested.  If debug build and simple
    // console, print a heartbeat if recording.
    //
    // gIsRecording is the real timeline recording state.  Because we're just
    // reading it without correlation to gRecordingToggleHistory, we don't need
    // the critical section.
    auto realtimeRecording = gIsRecording;
    if (!args.mSimpleConsole) {
        std::string display;
        UpdateConsole(pmData, updateTime, &display);
        UpdateConsole(pmData, *lsrData, updateTime, &display);
        SetConsoleText(display.c_str());

        if (realtimeRecording) {
            printf("** RECORDING **\n");
        }
    }
#if _DEBUG
    else if (realtimeRecording) {
        printf(".");
    }
#endif

    // Update tracking information.
    for (auto ntProcessEvent : *ntProcessEvents) {
        if (ntProcessEvent.ImageFileName.empty()) { // Empty ImageFileName indicates the process terminated
            StopProcess(pmData, ntProcessEvent.ProcessId);
        }
    }

    // Clear events processed.
    ntProcessEvents->clear();
    presentEvents->clear();
    lsrEvents->clear();

    // Finished processing all events.  Erase the recording toggles that were
    // handled.
    UpdateRecordingToggles(recordingToggleIndex);
}

void Output()
{
    auto const& args = GetCommandLineArgs();

    // Structures to track processes and statistics from recorded events.
    PresentMonData pmData;
    LateStageReprojectionData lsrData;

    // Create any CSV files that don't need process info to be created
    CreateNonProcessCSVs(pmData);

    // Enter loop to consume collected events.
    std::vector<NTProcessEvent> ntProcessEvents;
    std::vector<std::shared_ptr<PresentEvent>> presentEvents;
    std::vector<std::shared_ptr<LateStageReprojectionEvent>> lsrEvents;
    std::vector<uint64_t> recordingToggleHistory;
    ntProcessEvents.reserve(128);
    presentEvents.reserve(4096);
    lsrEvents.reserve(4096);
    recordingToggleHistory.reserve(16);
    for (;;) {
        // Read gQuit here, but then check it after processing queued events.
        // This ensures that we call DequeueAnalyzedInfo() at least once after
        // events have stopped being collected so that all events are included.
        auto quit = gQuit;

        // Copy and process all the collected events, and update the various
        // tracking and statistics data structures.
        uint64_t updateTime = GetTickCount64();
        ProcessEvents(pmData, updateTime, &lsrData, &ntProcessEvents, &presentEvents, &lsrEvents, &recordingToggleHistory);

        // Any CSV data would have been written out at this point, so if we're
        // quiting we don't need to update the rest.
        if (quit) {
            break;
        }

        // Sleep to reduce overhead.
        Sleep(100);
    }

    // Shut down output.
    uint32_t eventsLost = 0;
    uint32_t buffersLost = 0;
    CheckLostReports(&eventsLost, &buffersLost);

    CloseCSVs(pmData, eventsLost, buffersLost);

    pmData.mProcessMap.clear();

    if (args.mSimpleConsole == false) {
        SetConsoleText("");
    }
}

void StartOutputThread()
{
    InitializeCriticalSection(&gRecordingToggleCS);

    gThread = std::thread(Output);
}

void StopOutputThread()
{
    if (gThread.joinable()) {
        gQuit = true;
        gThread.join();

        DeleteCriticalSection(&gRecordingToggleCS);
    }
}

