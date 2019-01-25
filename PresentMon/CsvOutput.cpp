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

//#include <algorithm>
//#include <shlwapi.h>

#include "PresentMon.hpp"

const char* PresentModeToString(PresentMode mode)
{
    switch (mode) {
    case PresentMode::Hardware_Legacy_Flip: return "Hardware: Legacy Flip";
    case PresentMode::Hardware_Legacy_Copy_To_Front_Buffer: return "Hardware: Legacy Copy to front buffer";
    case PresentMode::Hardware_Direct_Flip: return "Hardware: Direct Flip";
    case PresentMode::Hardware_Independent_Flip: return "Hardware: Independent Flip";
    case PresentMode::Composed_Flip: return "Composed: Flip";
    case PresentMode::Composed_Copy_GPU_GDI: return "Composed: Copy with GPU GDI";
    case PresentMode::Composed_Copy_CPU_GDI: return "Composed: Copy with CPU GDI";
    case PresentMode::Composed_Composition_Atlas: return "Composed: Composition Atlas";
    case PresentMode::Hardware_Composed_Independent_Flip: return "Hardware Composed: Independent Flip";
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

const char* FinalStateToDroppedString(PresentResult res)
{
    switch (res) {
    case PresentResult::Presented: return "0";
    case PresentResult::Error: return "Error";
    default: return "1";
    }
}

//  mOutputFilename mHotkeySupport mMultiCsv processName -> FileName
//  PATH.EXT        true           true      PROCESSNAME -> PATH-PROCESSNAME-INDEX.EXT
//  PATH.EXT        false          true      PROCESSNAME -> PATH-PROCESSNAME.EXT
//  PATH.EXT        true           false     any         -> PATH-INDEX.EXT
//  PATH.EXT        false          false     any         -> PATH.EXT
//  nullptr         any            any       nullptr     -> PresentMon-TIME.csv
//  nullptr         any            any       PROCESSNAME -> PresentMon-PROCESSNAME-TIME.csv
//
// If wmr, then append _WMR to name.
static void GenerateOutputFilename(const PresentMonData& pm, const char* processName, bool wmr, char* path)
{
    char ext[_MAX_EXT];

    if (pm.mArgs->mOutputFileName) {
        char drive[_MAX_DRIVE];
        char dir[_MAX_DIR];
        char name[_MAX_FNAME];
        _splitpath_s(pm.mArgs->mOutputFileName, drive, dir, name, ext);

        int i = _snprintf_s(path, MAX_PATH, _TRUNCATE, "%s%s%s", drive, dir, name);

        if (pm.mArgs->mMultiCsv) {
            i += _snprintf_s(path + i, MAX_PATH - i, _TRUNCATE, "-%s", processName);
        }

        if (pm.mArgs->mHotkeySupport) {
            i += _snprintf_s(path + i, MAX_PATH - i, _TRUNCATE, "-%d", pm.mArgs->mRecordingCount);
        }
    } else {
        strcpy_s(ext, ".csv");

        if (processName == nullptr) {
            _snprintf_s(path, MAX_PATH, _TRUNCATE, "PresentMon-%s", pm.mCaptureTimeStr);
        } else {
            _snprintf_s(path, MAX_PATH, _TRUNCATE, "PresentMon-%s-%s", processName, pm.mCaptureTimeStr);
        }
    }

    if (wmr) {
        strcat_s(path, MAX_PATH, "_WMR");
    }

    strcat_s(path, MAX_PATH, ext);
}

static void CreateOutputFiles(PresentMonData& pm, const char* processName, FILE** outputFile, FILE** lsrOutputFile)
{
    // Open output file and print CSV header
    char outputFilePath[MAX_PATH];
    GenerateOutputFilename(pm, processName, false, outputFilePath);
    fopen_s(outputFile, outputFilePath, "w");
    if (*outputFile) {
        fprintf(*outputFile, "Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags");
        if (pm.mArgs->mVerbosity > Verbosity::Simple)
        {
            fprintf(*outputFile, ",AllowsTearing,PresentMode");
        }
        if (pm.mArgs->mVerbosity >= Verbosity::Verbose)
        {
            fprintf(*outputFile, ",WasBatched,DwmNotified");
        }
        fprintf(*outputFile, ",Dropped,TimeInSeconds,MsBetweenPresents");
        if (pm.mArgs->mVerbosity > Verbosity::Simple)
        {
            fprintf(*outputFile, ",MsBetweenDisplayChange");
        }
        fprintf(*outputFile, ",MsInPresentAPI");
        if (pm.mArgs->mVerbosity > Verbosity::Simple)
        {
            fprintf(*outputFile, ",MsUntilRenderComplete,MsUntilDisplayed");
        }
        fprintf(*outputFile, "\n");
    }

    if (pm.mArgs->mIncludeWindowsMixedReality) {
        // Open output file and print CSV header
        GenerateOutputFilename(pm, processName, true, outputFilePath);
        fopen_s(lsrOutputFile, outputFilePath, "w");
        if (*lsrOutputFile) {
            fprintf(*lsrOutputFile, "Application,ProcessID,DwmProcessID");
            if (pm.mArgs->mVerbosity >= Verbosity::Verbose)
            {
                fprintf(*lsrOutputFile, ",HolographicFrameID");
            }
            fprintf(*lsrOutputFile, ",TimeInSeconds");
            if (pm.mArgs->mVerbosity > Verbosity::Simple)
            {
                fprintf(*lsrOutputFile, ",MsBetweenAppPresents,MsAppPresentToLsr");
            }
            fprintf(*lsrOutputFile, ",MsBetweenLsrs,AppMissed,LsrMissed");
            if (pm.mArgs->mVerbosity >= Verbosity::Verbose)
            {
                fprintf(*lsrOutputFile, ",MsSourceReleaseFromRenderingToLsrAcquire,MsAppCpuRenderFrame");
            }
            fprintf(*lsrOutputFile, ",MsAppPoseLatency");
            if (pm.mArgs->mVerbosity >= Verbosity::Verbose)
            {
                fprintf(*lsrOutputFile, ",MsAppMisprediction,MsLsrCpuRenderFrame");
            }
            fprintf(*lsrOutputFile, ",MsLsrPoseLatency,MsActualLsrPoseLatency,MsTimeUntilVsync,MsLsrThreadWakeupToGpuEnd,MsLsrThreadWakeupError");
            if (pm.mArgs->mVerbosity >= Verbosity::Verbose)
            {
                fprintf(*lsrOutputFile, ",MsLsrThreadWakeupToCpuRenderFrameStart,MsCpuRenderFrameStartToHeadPoseCallbackStart,MsGetHeadPose,MsHeadPoseCallbackStopToInputLatch,MsInputLatchToGpuSubmission");
            }
            fprintf(*lsrOutputFile, ",MsLsrPreemption,MsLsrExecution,MsCopyPreemption,MsCopyExecution,MsGpuEndToVsync");
            fprintf(*lsrOutputFile, "\n");
        }
    }
}

static void CloseFile(FILE* fp, uint32_t totalEventsLost, uint32_t totalBuffersLost)
{
    if (fp == nullptr) {
        return;
    }

    if (totalEventsLost > 0) {
        fprintf(fp, "warning: %u events were lost; collected data may be unreliable.\n", totalEventsLost);
    }
    if (totalBuffersLost > 0) {
        fprintf(fp, "warning: %u buffers were lost; collected data may be unreliable.\n", totalBuffersLost);
    }

    fclose(fp);
}

// Create output files that don't require process info:
//     - if we're not creating one per process, and
//     - we don't need to wait for the single process name specified by PID
void CreateNonProcessCSVs(PresentMonData& pm)
{
    // Generate capture date string in ISO 8601 format
    {
        struct tm tm;
        time_t time_now = time(NULL);
        localtime_s(&tm, &time_now);
        _snprintf_s(pm.mCaptureTimeStr, _TRUNCATE, "%4d-%02d-%02dT%02d%02d%02d",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    if (pm.mArgs->mOutputFile && !pm.mArgs->mMultiCsv && !(pm.mArgs->mTargetPid != 0 && pm.mArgs->mTargetProcessNames.empty())) {
        CreateOutputFiles(pm, pm.mArgs->mTargetPid == 0 && pm.mArgs->mTargetProcessNames.size() == 1 ? pm.mArgs->mTargetProcessNames[0] : nullptr, &pm.mOutputFile, &pm.mLsrOutputFile);
    }
}

// Create output files that require process info:
//     - if we're creating one per process, or
//     - if we're waiting to know the single target process name specified by PID.
void CreateProcessCSVs(PresentMonData& pm, ProcessInfo* proc, std::string const& imageFileName)
{
    if (pm.mArgs->mMultiCsv) {
        auto it = pm.mProcessOutputFiles.find(imageFileName);
        if (it == pm.mProcessOutputFiles.end()) {
            CreateOutputFiles(pm, imageFileName.c_str(), &proc->mOutputFile, &proc->mLsrOutputFile);
        } else {
            proc->mOutputFile = it->second.first;
            proc->mLsrOutputFile = it->second.second;
            pm.mProcessOutputFiles.erase(it);
        }
    } else if (pm.mArgs->mOutputFile && pm.mOutputFile == nullptr) {
        CreateOutputFiles(pm, imageFileName.c_str(), &pm.mOutputFile, &pm.mLsrOutputFile);
    }
}

void CloseCSVs(PresentMonData& pm, uint32_t totalEventsLost, uint32_t totalBuffersLost)
{
    CloseFile(pm.mOutputFile, totalEventsLost, totalBuffersLost);
    CloseFile(pm.mLsrOutputFile, totalEventsLost, totalBuffersLost);
    pm.mOutputFile = nullptr;
    pm.mLsrOutputFile = nullptr;

    for (auto& p : pm.mProcessMap) {
        auto proc = &p.second;
        CloseFile(proc->mOutputFile, totalEventsLost, totalBuffersLost);
        CloseFile(proc->mLsrOutputFile, totalEventsLost, totalBuffersLost);
        proc->mOutputFile = nullptr;
        proc->mLsrOutputFile = nullptr;
    }

    for (auto& p : pm.mProcessOutputFiles) {
        CloseFile(p.second.first, totalEventsLost, totalBuffersLost);
        CloseFile(p.second.second, totalEventsLost, totalBuffersLost);
    }

    pm.mProcessOutputFiles.clear();
}

void UpdateCSV(PresentMonData& pm, ProcessInfo* proc, SwapChainData const& chain, PresentEvent& p, uint64_t perfFreq)
{
    auto file = pm.mArgs->mMultiCsv ? proc->mOutputFile : pm.mOutputFile;
    if (file && (p.FinalState == PresentResult::Presented || !pm.mArgs->mExcludeDropped)) {
        auto len = chain.mPresentHistory.size();
        auto displayedLen = chain.mDisplayedPresentHistory.size();
        if (len > 1) {
            auto& curr = chain.mPresentHistory[len - 1];
            auto& prev = chain.mPresentHistory[len - 2];
            double deltaMilliseconds = 1000 * double(curr.QpcTime - prev.QpcTime) / perfFreq;
            double deltaReady = curr.ReadyTime == 0 ? 0.0 : (1000 * double(curr.ReadyTime - curr.QpcTime) / perfFreq);
            double deltaDisplayed = curr.FinalState == PresentResult::Presented ? (1000 * double(curr.ScreenTime - curr.QpcTime) / perfFreq) : 0.0;
            double timeTakenMilliseconds = 1000 * double(curr.TimeTaken) / perfFreq;

            double timeSincePreviousDisplayed = 0.0;
            if (curr.FinalState == PresentResult::Presented && displayedLen > 1) {
                assert(chain.mDisplayedPresentHistory[displayedLen - 1].QpcTime == curr.QpcTime);
                auto& prevDisplayed = chain.mDisplayedPresentHistory[displayedLen - 2];
                timeSincePreviousDisplayed = 1000 * double(curr.ScreenTime - prevDisplayed.ScreenTime) / perfFreq;
            }

            double timeInSeconds = (double)(int64_t)(p.QpcTime - pm.mStartupQpcTime) / perfFreq;
            fprintf(file, "%s,%d,0x%016llX,%s,%d,%d",
                    proc->mModuleName.c_str(), p.ProcessId, p.SwapChainAddress, RuntimeToString(p.Runtime), curr.SyncInterval, curr.PresentFlags);
            if (pm.mArgs->mVerbosity > Verbosity::Simple)
            {
                fprintf(file, ",%d,%s", curr.SupportsTearing, PresentModeToString(curr.PresentMode));
            }
            if (pm.mArgs->mVerbosity >= Verbosity::Verbose)
            {
                fprintf(file, ",%d,%d", curr.WasBatched, curr.DwmNotified);
            }
            fprintf(file, ",%s,%.6lf,%.3lf", FinalStateToDroppedString(curr.FinalState), timeInSeconds, deltaMilliseconds);
            if (pm.mArgs->mVerbosity > Verbosity::Simple)
            {
                fprintf(file, ",%.3lf", timeSincePreviousDisplayed);
            }
            fprintf(file, ",%.3lf", timeTakenMilliseconds);
            if (pm.mArgs->mVerbosity > Verbosity::Simple)
            {
                fprintf(file, ",%.3lf,%.3lf", deltaReady, deltaDisplayed);
            }
            fprintf(file, "\n");
        }
    }
}

