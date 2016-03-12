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

#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <deque>

struct PresentEvent {
    uint64_t QpcTime;
    uint64_t SwapChainAddress;
    uint64_t TimeTaken;
    uint32_t SyncInterval;
    uint32_t PresentFlags;
    uint32_t ProcessId;
};

struct SwapChainData {
    uint64_t mLastUpdateTicks = 0;
    uint32_t mLastSyncInterval = -1;
    uint32_t mLastFlags = -1;
    std::deque<PresentEvent> mPresentHistory;
};

struct ProcessInfo {
    uint64_t mLastRefreshTicks = 0; // GetTickCount64
    std::string mModuleName;
    std::map<uint64_t, SwapChainData> mChainMap;
};

struct PresentMonArgs {
    const char *mOutputFileName = nullptr;
    const char *mTargetProcessName = nullptr;
    int mTargetPid = 0;
};

struct PresentMonData {
    const PresentMonArgs *mArgs = nullptr;
    FILE *mOutputFile = nullptr;
    std::map<uint32_t, ProcessInfo> mProcessMap;
};

void PresentMonEtw(PresentMonArgs args);

void PresentMon_Init(const PresentMonArgs& args, PresentMonData& data);
void PresentMon_Update(PresentMonData& data, std::vector<PresentEvent> presents, uint64_t perfFreq);
void PresentMon_Shutdown(PresentMonData& data);

