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

static uint32_t gRecordingCount = 0;

void IncrementRecordingCount()
{
    gRecordingCount += 1;
}

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

    auto const& args = GetCommandLineArgs();
    if (args.mOutputFileName) {
        char drive[_MAX_DRIVE];
        char dir[_MAX_DIR];
        char name[_MAX_FNAME];
        _splitpath_s(args.mOutputFileName, drive, dir, name, ext);

        int i = _snprintf_s(path, MAX_PATH, _TRUNCATE, "%s%s%s", drive, dir, name);

        if (args.mMultiCsv) {
            i += _snprintf_s(path + i, MAX_PATH - i, _TRUNCATE, "-%s", processName);
        }

        if (args.mHotkeySupport) {
            i += _snprintf_s(path + i, MAX_PATH - i, _TRUNCATE, "-%d", gRecordingCount);
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
    auto const& args = GetCommandLineArgs();

    // Open output file and print CSV header
    char outputFilePath[MAX_PATH];
    GenerateOutputFilename(pm, processName, false, outputFilePath);
    fopen_s(outputFile, outputFilePath, "w");
    if (*outputFile) {
        fprintf(*outputFile, "Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags");
        if (args.mVerbosity > Verbosity::Simple)
        {
            fprintf(*outputFile, ",AllowsTearing,PresentMode");
        }
        if (args.mVerbosity >= Verbosity::Verbose)
        {
            fprintf(*outputFile, ",WasBatched,DwmNotified");
        }
        fprintf(*outputFile, ",Dropped,TimeInSeconds,MsBetweenPresents");
        if (args.mVerbosity > Verbosity::Simple)
        {
            fprintf(*outputFile, ",MsBetweenDisplayChange");
        }
        fprintf(*outputFile, ",MsInPresentAPI");
        if (args.mVerbosity > Verbosity::Simple)
        {
            fprintf(*outputFile, ",MsUntilRenderComplete,MsUntilDisplayed");
        }
        fprintf(*outputFile, "\n");
    }

    if (args.mIncludeWindowsMixedReality) {
        // Open output file and print CSV header
        GenerateOutputFilename(pm, processName, true, outputFilePath);
        fopen_s(lsrOutputFile, outputFilePath, "w");
        if (*lsrOutputFile) {
            fprintf(*lsrOutputFile, "Application,ProcessID,DwmProcessID");
            if (args.mVerbosity >= Verbosity::Verbose)
            {
                fprintf(*lsrOutputFile, ",HolographicFrameID");
            }
            fprintf(*lsrOutputFile, ",TimeInSeconds");
            if (args.mVerbosity > Verbosity::Simple)
            {
                fprintf(*lsrOutputFile, ",MsBetweenAppPresents,MsAppPresentToLsr");
            }
            fprintf(*lsrOutputFile, ",MsBetweenLsrs,AppMissed,LsrMissed");
            if (args.mVerbosity >= Verbosity::Verbose)
            {
                fprintf(*lsrOutputFile, ",MsSourceReleaseFromRenderingToLsrAcquire,MsAppCpuRenderFrame");
            }
            fprintf(*lsrOutputFile, ",MsAppPoseLatency");
            if (args.mVerbosity >= Verbosity::Verbose)
            {
                fprintf(*lsrOutputFile, ",MsAppMisprediction,MsLsrCpuRenderFrame");
            }
            fprintf(*lsrOutputFile, ",MsLsrPoseLatency,MsActualLsrPoseLatency,MsTimeUntilVsync,MsLsrThreadWakeupToGpuEnd,MsLsrThreadWakeupError");
            if (args.mVerbosity >= Verbosity::Verbose)
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
    auto const& args = GetCommandLineArgs();

    // Generate capture date string in ISO 8601 format
    {
        struct tm tm;
        time_t time_now = time(NULL);
        localtime_s(&tm, &time_now);
        _snprintf_s(pm.mCaptureTimeStr, _TRUNCATE, "%4d-%02d-%02dT%02d%02d%02d",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    if (args.mOutputFile && !args.mMultiCsv && !(args.mTargetPid != 0 && args.mTargetProcessNames.empty())) {
        CreateOutputFiles(pm, args.mTargetPid == 0 && args.mTargetProcessNames.size() == 1 ? args.mTargetProcessNames[0] : nullptr, &pm.mOutputFile, &pm.mLsrOutputFile);
    }
}

// Create output files that require process info:
//     - if we're creating one per process, or
//     - if we're waiting to know the single target process name specified by PID.
void CreateProcessCSVs(PresentMonData& pm, ProcessInfo* proc, std::string const& imageFileName)
{
    auto const& args = GetCommandLineArgs();

    if (args.mMultiCsv) {
        auto it = pm.mProcessOutputFiles.find(imageFileName);
        if (it == pm.mProcessOutputFiles.end()) {
            CreateOutputFiles(pm, imageFileName.c_str(), &proc->mOutputFile, &proc->mLsrOutputFile);
        } else {
            proc->mOutputFile = it->second.first;
            proc->mLsrOutputFile = it->second.second;
            pm.mProcessOutputFiles.erase(it);
        }
    } else if (args.mOutputFile && pm.mOutputFile == nullptr) {
        CreateOutputFiles(pm, imageFileName.c_str(), &pm.mOutputFile, &pm.mLsrOutputFile);
    }
}

void CloseCSVs(PresentMonData& pm, std::unordered_map<uint32_t, ProcessInfo>* activeProcesses, uint32_t totalEventsLost, uint32_t totalBuffersLost)
{
    CloseFile(pm.mOutputFile, totalEventsLost, totalBuffersLost);
    CloseFile(pm.mLsrOutputFile, totalEventsLost, totalBuffersLost);
    pm.mOutputFile = nullptr;
    pm.mLsrOutputFile = nullptr;

    for (auto& p : *activeProcesses) {
        auto processInfo = &p.second;
        CloseFile(processInfo->mOutputFile, totalEventsLost, totalBuffersLost);
        CloseFile(processInfo->mLsrOutputFile, totalEventsLost, totalBuffersLost);
        processInfo->mOutputFile = nullptr;
        processInfo->mLsrOutputFile = nullptr;
    }

    for (auto& p : pm.mProcessOutputFiles) {
        CloseFile(p.second.first, totalEventsLost, totalBuffersLost);
        CloseFile(p.second.second, totalEventsLost, totalBuffersLost);
    }

    pm.mProcessOutputFiles.clear();
}

void UpdateCSV(PresentMonData& pm, ProcessInfo const& processInfo, SwapChainData const& chain, PresentEvent& p)
{
    auto const& args = GetCommandLineArgs();

    auto file = args.mMultiCsv ? processInfo.mOutputFile : pm.mOutputFile;
    if (file && (p.FinalState == PresentResult::Presented || !args.mExcludeDropped)) {
        auto len = chain.mPresentHistory.size();
        auto displayedLen = chain.mDisplayedPresentHistory.size();
        if (len > 1) {
            auto& curr = chain.mPresentHistory[len - 1];
            auto& prev = chain.mPresentHistory[len - 2];
            double deltaMilliseconds = 1000.0 * QpcDeltaToSeconds(curr.QpcTime - prev.QpcTime);
            double deltaReady = curr.ReadyTime == 0 ? 0.0 : (1000.0 * QpcDeltaToSeconds(curr.ReadyTime - curr.QpcTime));
            double deltaDisplayed = curr.FinalState == PresentResult::Presented ? (1000.0 * QpcDeltaToSeconds(curr.ScreenTime - curr.QpcTime)) : 0.0;
            double timeTakenMilliseconds = 1000.0 * QpcDeltaToSeconds(curr.TimeTaken);

            double timeSincePreviousDisplayed = 0.0;
            if (curr.FinalState == PresentResult::Presented && displayedLen > 1) {
                assert(chain.mDisplayedPresentHistory[displayedLen - 1].QpcTime == curr.QpcTime);
                auto& prevDisplayed = chain.mDisplayedPresentHistory[displayedLen - 2];
                timeSincePreviousDisplayed = 1000.0 * QpcDeltaToSeconds(curr.ScreenTime - prevDisplayed.ScreenTime);
            }

            double timeInSeconds = QpcToSeconds(p.QpcTime);
            fprintf(file, "%s,%d,0x%016llX,%s,%d,%d",
                    processInfo.mModuleName.c_str(), p.ProcessId, p.SwapChainAddress, RuntimeToString(p.Runtime), curr.SyncInterval, curr.PresentFlags);
            if (args.mVerbosity > Verbosity::Simple)
            {
                fprintf(file, ",%d,%s", curr.SupportsTearing, PresentModeToString(curr.PresentMode));
            }
            if (args.mVerbosity >= Verbosity::Verbose)
            {
                fprintf(file, ",%d,%d", curr.WasBatched, curr.DwmNotified);
            }
            fprintf(file, ",%s,%.6lf,%.3lf", FinalStateToDroppedString(curr.FinalState), timeInSeconds, deltaMilliseconds);
            if (args.mVerbosity > Verbosity::Simple)
            {
                fprintf(file, ",%.3lf", timeSincePreviousDisplayed);
            }
            fprintf(file, ",%.3lf", timeTakenMilliseconds);
            if (args.mVerbosity > Verbosity::Simple)
            {
                fprintf(file, ",%.3lf,%.3lf", deltaReady, deltaDisplayed);
            }
            fprintf(file, "\n");
        }
    }
}

