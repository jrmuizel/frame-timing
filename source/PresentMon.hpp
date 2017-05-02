/*
Copyright 2017 Intel Corporation

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

#include "CommandLine.hpp"
#include "CommonIncludes.hpp"
#include "PresentMonTraceConsumer.hpp"
#include "SwapChainData.hpp"

#include <unordered_map>

struct NTProcessEvent {
    uint32_t ProcessId;
    std::string ImageFileName;  // If ImageFileName.empty(), then event is that process ending
};

struct ProcessInfo {
    uint64_t mLastRefreshTicks = 0; // GetTickCount64
    std::string mModuleName;
    std::map<uint64_t, SwapChainData> mChainMap;
    bool mTerminationProcess;
    bool mProcessExists = false;
};

struct PresentMonData {
    const CommandLineArgs *mArgs = nullptr;
    uint64_t mStartupQpcTime;
    char mOutputFilePath[MAX_PATH];
    FILE *mOutputFile = nullptr;
    std::map<uint32_t, ProcessInfo> mProcessMap;
    uint32_t mTerminationProcessCount = 0;

    // Process events
    std::mutex mNTProcessEventMutex;
    std::vector<NTProcessEvent> mNTProcessEvents;
};

void EtwConsumingThread(const CommandLineArgs& args);

void PresentMon_Init(const CommandLineArgs& args, PresentMonData& data);
void PresentMon_Update(PresentMonData& data, std::vector<std::shared_ptr<PresentEvent>>& presents, uint64_t perfFreq);
void PresentMon_Shutdown(PresentMonData& data, bool log_corrupted);

