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
#include <memory>
#include <cassert>

extern bool g_Quit;

enum class PresentMode
{
    Unknown,
    Fullscreen,
    Composed_Flip,
    DirectFlip,
    IndependentFlip,
    ImmediateIndependentFlip,
    IndependentFlipMPO,
    Windowed_Blit,
    Fullscreen_Blit,
    Legacy_Windowed_Blit,
    Composition_Buffer,
};
enum class PresentResult
{
    Unknown, Presented, Discarded
};
enum class Runtime
{
    DXGI, D3D9, Other
};
struct PresentEvent {
    // Available from DXGI Present
    uint64_t QpcTime = 0;
    uint64_t SwapChainAddress = 0;
    int32_t SyncInterval = -1;
    uint32_t PresentFlags = 0;
    uint32_t ProcessId = 0;

    PresentMode PresentMode = PresentMode::Unknown;
    Runtime Runtime = Runtime::Other;

    // Time spent in DXGI Present call
    uint64_t TimeTaken = 0;

    // Timestamp of "ready" state (GPU work completed)
    uint64_t ReadyTime = 0;

    // Timestamp of "complete" state (data on screen or discarded)
    uint64_t ScreenTime = 0;
    PresentResult FinalState = PresentResult::Unknown;
    uint32_t PlaneIndex = 0;

    // Additional transient state
    uint32_t QueueSubmitSequence = 0;
    uint64_t Hwnd = 0;
    std::deque<std::shared_ptr<PresentEvent>> DependentPresents;
    bool MMIO = false;
#if _DEBUG
    bool Completed = false;
    ~PresentEvent() { assert(Completed || g_Quit); }
#endif
};

struct SwapChainData {
    Runtime mRuntime = Runtime::Other;
    uint64_t mLastUpdateTicks = 0;
    uint32_t mLastSyncInterval = -1;
    uint32_t mLastFlags = -1;
    std::deque<PresentEvent> mPresentHistory;
    std::deque<PresentEvent> mDisplayedPresentHistory;
    PresentMode mLastPresentMode = PresentMode::Unknown;
};

struct ProcessInfo {
    uint64_t mLastRefreshTicks = 0; // GetTickCount64
    std::string mModuleName;
    std::map<uint64_t, SwapChainData> mChainMap;
};

struct PresentMonArgs {
    const char *mOutputFileName = nullptr;
    const char *mTargetProcessName = nullptr;
    const char *mEtlFileName = nullptr;
    int mTargetPid = 0;
    int mDelay = 0;
    int mTimer = 0;
    bool mScrollLockToggle = false;
};

struct PresentMonData {
    const PresentMonArgs *mArgs = nullptr;
    FILE *mOutputFile = nullptr;
    std::map<uint32_t, ProcessInfo> mProcessMap;
};

void PresentMonEtw(PresentMonArgs args);

void PresentMon_Init(const PresentMonArgs& args, PresentMonData& data);
void PresentMon_UpdateNewProcesses(PresentMonData& data, std::map<uint32_t, ProcessInfo>& processes);
void PresentMon_Update(PresentMonData& data, std::vector<std::shared_ptr<PresentEvent>> presents, uint64_t perfFreq);
void PresentMon_UpdateDeadProcesses(PresentMonData& data, std::vector<uint32_t>& processIds);
void PresentMon_Shutdown(PresentMonData& data);

