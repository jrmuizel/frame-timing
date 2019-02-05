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
    
    void PruneDeque(std::deque<PresentEvent> &presentHistory, uint64_t perfFreq, uint32_t msTimeDiff, uint32_t maxHistLen);
    void AddPresentToSwapChain(PresentEvent& p);
    void UpdateSwapChainInfo(PresentEvent&p, uint64_t now, uint64_t perfFreq);
    double ComputeDisplayedFps(uint64_t qpcFreq) const;
    double ComputeFps(uint64_t qpcFreq) const;
    double ComputeLatency(uint64_t qpcFreq) const;
    double ComputeCpuFrameTime(uint64_t qpcFreq) const;
    bool IsStale(uint64_t now) const;
private:
    double ComputeFps(const std::deque<PresentEvent>& presentHistory, uint64_t qpcFreq) const;
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
    uint64_t mStartupQpcTime = 0;
    FILE *mOutputFile = nullptr;    // Used if not -multi_csv
    FILE *mLsrOutputFile = nullptr; // Used if not -multi_csv
    std::map<uint32_t, ProcessInfo> mProcessMap;
    std::map<std::string, std::pair<FILE*, FILE*> > mProcessOutputFiles;
    uint32_t mTerminationProcessCount = 0;
};

typedef void (*EventHandlerFn)(EVENT_RECORD* pEventRecord, void* pContext);
typedef bool (*ShouldStopProcessingEventsFn)();

struct TraceSession {
    // BEGIN trace property block, must be beginning of TraceSession
    EVENT_TRACE_PROPERTIES properties_;
    wchar_t loggerName_[MAX_PATH];
    // END Trace property block

    TRACEHANDLE sessionHandle_;    // Must be first member after trace property block
    TRACEHANDLE traceHandle_;
    ShouldStopProcessingEventsFn shouldStopProcessingEventsFn_;
    uint64_t startTime_;
    uint64_t frequency_;
    uint32_t eventsLostCount_;
    uint32_t buffersLostCount_;

    // Structure to hold the mapping from provider ID to event handler function
    struct GUIDHash { size_t operator()(GUID const& g) const; };
    struct GUIDEqual { bool operator()(GUID const& lhs, GUID const& rhs) const; };
    struct Provider {
        ULONGLONG matchAny_;
        ULONGLONG matchAll_;
        UCHAR level_;
    };
    struct Handler {
        EventHandlerFn fn_;
        void* ctxt_;
    };
    std::unordered_map<GUID, Provider, GUIDHash, GUIDEqual> eventProvider_;
    std::unordered_map<GUID, Handler, GUIDHash, GUIDEqual> eventHandler_;

    TraceSession()
        : sessionHandle_(0)
        , traceHandle_(INVALID_PROCESSTRACE_HANDLE)
        , startTime_(0)
        , frequency_(0)
        , shouldStopProcessingEventsFn_(nullptr)
    {
    }

    // Usage:
    //
    // 1) use TraceSession::AddProvider() to add the IDs for all the providers
    // you want to trace. Use TraceSession::AddHandler() to add the handler
    // functions for the providers/events you want to trace.
    //
    // 2) call TraceSession::InitializeRealtime() or
    // TraceSession::InitializeEtlFile(), to start tracing events from
    // real-time collection or from a previously-captured .etl file. At this
    // point, events start to be traced.
    //
    // 3) call ::ProcessTrace() to start collecting the events; provider
    // handler functions will be called as those provider events are collected.
    // ProcessTrace() will exit when shouldStopProcessingEventsFn_ returns
    // true, or when the .etl file is fully consumed.
    //
    // 4) Finalize() to clean up.

    // AddProvider/Handler() returns false if the providerId already has a handler.
    // RemoveProvider/Handler() returns false if the providerId don't have a handler.
    bool AddProvider(GUID providerId, UCHAR level, ULONGLONG matchAnyKeyword, ULONGLONG matchAllKeyword);
    bool AddHandler(GUID handlerId, EventHandlerFn handlerFn, void* handlerContext);
    bool AddProviderAndHandler(GUID providerId, UCHAR level, ULONGLONG matchAnyKeyword, ULONGLONG matchAllKeyword,
                               EventHandlerFn handlerFn, void* handlerContext);
    bool RemoveProvider(GUID providerId);
    bool RemoveHandler(GUID handlerId);
    bool RemoveProviderAndHandler(GUID providerId);

    // InitializeRealtime() and InitializeEtlFile() return false if the session
    // could not be created.
    bool InitializeEtlFile(char const* etlPath, ShouldStopProcessingEventsFn shouldStopProcessingEventsFn);
    bool InitializeRealtime(char const* traceSessionName, bool stopExistingSession, ShouldStopProcessingEventsFn shouldStopProcessingEventsFn);
    void Finalize();

    // Call CheckLostReports() at any time the session is initialized to query
    // how many events and buffers have been lost while tracing.
    bool CheckLostReports(uint32_t* eventsLost, uint32_t* buffersLost);

    void Stop();
};

void EtwConsumingThread();

bool EtwThreadsShouldQuit();
void PostStopRecording();
void PostQuitProcess();

// CommandLine.cpp:
bool ParseCommandLine(int argc, char** argv);
CommandLineArgs const& GetCommandLineArgs();

// Console.cpp:
void SetConsoleText(const char *text);
void UpdateConsole(PresentMonData const& pm, uint64_t updateTime, uint64_t qpcFrequency, std::string* display);

// ConsumerThread.cpp:
bool IsConsumerThreadRunning();
void StartConsumerThread(TRACEHANDLE traceHandle);
void WaitForConsumerThreadToExit();

// CsvOutput.cpp:
void IncrementRecordingCount();
void CreateNonProcessCSVs(PresentMonData& pm);
void CreateProcessCSVs(PresentMonData& pm, ProcessInfo* proc, std::string const& imageFileName);
void CloseCSVs(PresentMonData& pm, uint32_t totalEventsLost, uint32_t totalBuffersLost);
void UpdateCSV(PresentMonData& pm, ProcessInfo* proc, SwapChainData const& chain, PresentEvent& p, uint64_t perfFreq);

// CsvOutput.cpp:
const char* FinalStateToDroppedString(PresentResult res);
const char* PresentModeToString(PresentMode mode);
const char* RuntimeToString(Runtime rt);

// Privilege.cpp:
bool ElevatePrivilege(int argc, char** argv);

