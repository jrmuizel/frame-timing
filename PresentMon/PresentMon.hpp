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

#pragma once

#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <stdint.h>
#include <stdio.h>
#include <windows.h>
#include <evntcons.h> // must be after windows.h

#include "../PresentData/PresentMonTraceConsumer.hpp"

enum class Verbosity {
    Simple,
    Normal,
    Verbose
};

struct CommandLineArgs {
    std::vector<const char*> mTargetProcessNames;
    std::vector<const char*> mExcludeProcessNames;
    const char *mOutputFileName;
    const char *mEtlFileName;
    const char *mSessionName;
    UINT mTargetPid;
    UINT mDelay;
    UINT mTimer;
    UINT mHotkeyModifiers;
    UINT mHotkeyVirtualKeyCode;
    Verbosity mVerbosity;
    bool mOutputFile;
    bool mScrollLockToggle;
    bool mScrollLockIndicator;
    bool mExcludeDropped;
    bool mSimpleConsole;
    bool mTerminateOnProcExit;
    bool mTerminateAfterTimer;
    bool mHotkeySupport;
    bool mTryToElevate;
    bool mIncludeWindowsMixedReality;
    bool mMultiCsv;
    bool mStopExistingSession;
};

bool EnableScrollLock(bool enable);

struct SwapChainData {
    Runtime mRuntime = Runtime::Other;
    uint64_t mLastUpdateTicks = 0;
    uint32_t mLastSyncInterval = UINT32_MAX;
    uint32_t mLastFlags = UINT32_MAX;
    std::deque<PresentEvent> mPresentHistory;
    std::deque<PresentEvent> mDisplayedPresentHistory;
    PresentMode mLastPresentMode = PresentMode::Unknown;
    uint32_t mLastPlane = 0;
    bool mHasBeenBatched = false;
    bool mDwmNotified = false;
    
    void PruneDeque(std::deque<PresentEvent> &presentHistory, uint32_t msTimeDiff, uint32_t maxHistLen);
    void AddPresentToSwapChain(PresentEvent& p);
    void UpdateSwapChainInfo(PresentEvent&p, uint64_t now);
    double ComputeDisplayedFps() const;
    double ComputeFps() const;
    double ComputeLatency() const;
    double ComputeCpuFrameTime() const;
    bool IsStale(uint64_t now) const;
private:
    double ComputeFps(const std::deque<PresentEvent>& presentHistory) const;
};

struct ProcessInfo {
    std::string mModuleName;
    std::map<uint64_t, SwapChainData> mChainMap;
    uint64_t mLastRefreshTicks; // GetTickCount64
    FILE *mOutputFile;          // Used if -multi_csv
    FILE *mLsrOutputFile;       // Used if -multi_csv
    bool mTargetProcess;
};

struct PresentMonData {
    char mCaptureTimeStr[18] = "";
    FILE *mOutputFile = nullptr;    // Used if not -multi_csv
    FILE *mLsrOutputFile = nullptr; // Used if not -multi_csv
    std::map<uint32_t, ProcessInfo> mProcessMap;
    std::map<std::string, std::pair<FILE*, FILE*> > mProcessOutputFiles;
    uint32_t mTerminationProcessCount = 0;
};

#include "LateStageReprojectionData.hpp"

void EtwConsumingThread();

bool EtwThreadsShouldQuit();
void PostStopRecording();
void PostQuitProcess();

// CommandLine.cpp:
bool ParseCommandLine(int argc, char** argv);
CommandLineArgs const& GetCommandLineArgs();

// Console.cpp:
void SetConsoleText(const char *text);
void UpdateConsole(PresentMonData const& pm, uint64_t updateTime, std::string* display);

// ConsumerThread.cpp:
void StartConsumerThread(TRACEHANDLE traceHandle);
void WaitForConsumerThreadToExit();

// CsvOutput.cpp:
void IncrementRecordingCount();
void CreateNonProcessCSVs(PresentMonData& pm);
void CreateProcessCSVs(PresentMonData& pm, ProcessInfo* proc, std::string const& imageFileName);
void CloseCSVs(PresentMonData& pm, uint32_t totalEventsLost, uint32_t totalBuffersLost);
void UpdateCSV(PresentMonData& pm, ProcessInfo* proc, SwapChainData const& chain, PresentEvent& p);

// CsvOutput.cpp:
const char* FinalStateToDroppedString(PresentResult res);
const char* PresentModeToString(PresentMode mode);
const char* RuntimeToString(Runtime rt);

// Privilege.cpp:
bool ElevatePrivilege(int argc, char** argv);

// TraceSession.cpp:
bool StartTraceSession(TRACEHANDLE* traceHandle);
void StopTraceSession();
void FinalizeTraceSession();
void CheckLostReports(uint32_t* eventsLost, uint32_t* buffersLost);
void DequeueAnalyzedInfo(
    std::vector<NTProcessEvent>* ntProcessEvents,
    std::vector<std::shared_ptr<PresentEvent>>* presents,
    std::vector<std::shared_ptr<LateStageReprojectionEvent>>* lsrs);
double QpcDeltaToSeconds(uint64_t qpcDelta);
double QpcToSeconds(uint64_t qpc);

