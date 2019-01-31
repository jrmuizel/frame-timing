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

/*
ETW Architecture:

    Controller -----> Trace Session <----- Providers (e.g., DXGI, D3D9, DXGK, DWM, Win32K)
                           |
                           \-------------> Consumers (e.g., ../PresentData/PresentMonTraceConsumer)

PresentMon Architecture:

    MainThread: starts and stops the trace session and coordinates user
    interaction.

    ConsumerThread: is controlled by the trace session, and collects and
    analyzes ETW events.

    OutputThread: is controlled by the trace session, and outputs analyzed
    events to the CSV and/or console.

The trace session and ETW analysis is always running, but whether or not
collected data is written to the CSV file(s) is controlled by a recording state
which is controlled from MainThread based on user input or timer.
*/

#include "../PresentData/MixedRealityTraceConsumer.hpp"
#include "../PresentData/PresentMonTraceConsumer.hpp"

#include <unordered_map>

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

struct SwapChainData {
    Runtime mRuntime = Runtime::Other;
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
    void UpdateSwapChainInfo(PresentEvent&p);
    double ComputeDisplayedFps() const;
    double ComputeFps() const;
    double ComputeLatency() const;
    double ComputeCpuFrameTime() const;
private:
    double ComputeFps(const std::deque<PresentEvent>& presentHistory) const;
};

struct ProcessInfo {
    std::string mModuleName;
    std::map<uint64_t, SwapChainData> mChainMap;
    HANDLE mHandle;
    FILE *mOutputFile;          // Used if -multi_csv
    FILE *mLsrOutputFile;       // Used if -multi_csv
    bool mTargetProcess;
};

struct PresentMonData {
    char mCaptureTimeStr[18] = "";
    FILE *mOutputFile = nullptr;    // Used if not -multi_csv
    FILE *mLsrOutputFile = nullptr; // Used if not -multi_csv
    std::map<std::string, std::pair<FILE*, FILE*> > mProcessOutputFiles;
};

#include "LateStageReprojectionData.hpp"

// CommandLine.cpp:
bool ParseCommandLine(int argc, char** argv);
CommandLineArgs const& GetCommandLineArgs();

// Console.cpp:
void SetConsoleText(const char *text);
void UpdateConsole(uint32_t processId, ProcessInfo const& processInfo, std::string* display);

// ConsumerThread.cpp:
void StartConsumerThread(TRACEHANDLE traceHandle);
void WaitForConsumerThreadToExit();

// CsvOutput.cpp:
void IncrementRecordingCount();
void CreateNonProcessCSVs(PresentMonData& pm);
void CreateProcessCSVs(PresentMonData& pm, ProcessInfo* proc, std::string const& imageFileName);
void CloseCSVs(PresentMonData& pm, std::unordered_map<uint32_t, ProcessInfo>* activeProcesses, uint32_t totalEventsLost, uint32_t totalBuffersLost);
void UpdateCSV(PresentMonData& pm, ProcessInfo const& processInfo, SwapChainData const& chain, PresentEvent& p);
const char* FinalStateToDroppedString(PresentResult res);
const char* PresentModeToString(PresentMode mode);
const char* RuntimeToString(Runtime rt);

// MainThread.cpp:
void ExitMainThread();

// OutputThread.cpp:
void StartOutputThread();
void StopOutputThread();
void SetOutputRecordingState(bool record);

// Privilege.cpp:
bool ElevatePrivilege(int argc, char** argv);

// TraceSession.cpp:
bool StartTraceSession();
void StopTraceSession();
void CheckLostReports(uint32_t* eventsLost, uint32_t* buffersLost);
void DequeueAnalyzedInfo(
    std::vector<NTProcessEvent>* ntProcessEvents,
    std::vector<std::shared_ptr<PresentEvent>>* presents,
    std::vector<std::shared_ptr<LateStageReprojectionEvent>>* lsrs);
double QpcDeltaToSeconds(uint64_t qpcDelta);
double QpcToSeconds(uint64_t qpc);

