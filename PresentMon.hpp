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

#pragma once

#include "CommonIncludes.hpp"
#include "PresentMonTraceConsumer.hpp"

struct PresentMonArgs {
    const char *mOutputFileName = nullptr;
    const char *mTargetProcessName = nullptr;
    const char *mEtlFileName = nullptr;
    int mTargetPid = 0;
    int mDelay = 0;
    int mTimer = 0;
    int mRestartCount = 0;
    bool mScrollLockToggle = false;
    bool mExcludeDropped = false;
    bool mSimple = false;
    bool mTerminateOnProcExit = false;
    bool mHotkeySupport = false;
};

struct PresentMonData {
    const PresentMonArgs *mArgs = nullptr;
    uint64_t mStartupQpcTime;
    std::string mOutputFilePath;
    FILE *mOutputFile = nullptr;
    std::map<uint32_t, ProcessInfo> mProcessMap;
    uint32_t mTerminationProcessCount = 0;
};

void PresentMonEtw(const PresentMonArgs& args);

void PresentMon_Init(const PresentMonArgs& args, PresentMonData& data);
void PresentMon_UpdateNewProcesses(PresentMonData& data, std::map<uint32_t, ProcessInfo>& processes);
void PresentMon_Update(PresentMonData& data, std::vector<std::shared_ptr<PresentEvent>>& presents, uint64_t perfFreq);
void PresentMon_UpdateDeadProcesses(PresentMonData& data, std::vector<uint32_t>& processIds);
void PresentMon_Shutdown(PresentMonData& data, bool log_corrupted);

