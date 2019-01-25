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

#include <algorithm>

#include "PresentMon.hpp"
#include "LateStageReprojectionData.hpp"

enum {
    MAX_HISTORY_TIME = 3000,
    LSR_TIMEOUT_THRESHOLD_TICKS = 10000, // 10 sec
    MAX_LSRS_IN_DEQUE = 120 * (MAX_HISTORY_TIME / 1000)
};

void LateStageReprojectionData::PruneDeque(std::deque<LateStageReprojectionEvent> &lsrHistory, uint64_t perfFreq, uint32_t msTimeDiff, uint32_t maxHistLen) {
    while (!lsrHistory.empty() &&
        (lsrHistory.size() > maxHistLen ||
        ((double)(lsrHistory.back().QpcTime - lsrHistory.front().QpcTime) / perfFreq) * 1000 > msTimeDiff)) {
        lsrHistory.pop_front();
    }
}

void LateStageReprojectionData::AddLateStageReprojection(LateStageReprojectionEvent& p)
{
    if (LateStageReprojectionPresented(p.FinalState))
    {
        assert(p.MissedVsyncCount == 0);
        mDisplayedLSRHistory.push_back(p);
    }
    else if(LateStageReprojectionMissed(p.FinalState))
    {
        assert(p.MissedVsyncCount >= 1);
        mLifetimeLsrMissedFrames += p.MissedVsyncCount;
    }

    if (p.NewSourceLatched)
    {
        mSourceHistory.push_back(p);
    }
    else
    {
        mLifetimeAppMissedFrames++;
    }

    if (!mLSRHistory.empty())
    {
        assert(mLSRHistory.back().QpcTime <= p.QpcTime);
    }
    mLSRHistory.push_back(p);
}

void LateStageReprojectionData::UpdateLateStageReprojectionInfo(uint64_t now, uint64_t perfFreq)
{
    PruneDeque(mSourceHistory, perfFreq, MAX_HISTORY_TIME, MAX_LSRS_IN_DEQUE);
    PruneDeque(mDisplayedLSRHistory, perfFreq, MAX_HISTORY_TIME, MAX_LSRS_IN_DEQUE);
    PruneDeque(mLSRHistory, perfFreq, MAX_HISTORY_TIME, MAX_LSRS_IN_DEQUE);

    mLastUpdateTicks = now;
}

double LateStageReprojectionData::ComputeHistoryTime(const std::deque<LateStageReprojectionEvent>& lsrHistory, uint64_t qpcFreq) const
{
    if (lsrHistory.size() < 2) {
        return 0.0;
    }

    auto start = lsrHistory.front().QpcTime;
    auto end = lsrHistory.back().QpcTime;
    return double(end - start) / qpcFreq;
}

size_t LateStageReprojectionData::ComputeHistorySize() const
{
    if (mLSRHistory.size() < 2) {
        return 0;
    }

    return mLSRHistory.size();
}

double LateStageReprojectionData::ComputeHistoryTime(uint64_t qpcFreq) const
{
    return ComputeHistoryTime(mLSRHistory, qpcFreq);
}

double LateStageReprojectionData::ComputeFps(const std::deque<LateStageReprojectionEvent>& lsrHistory, uint64_t qpcFreq) const
{
    if (lsrHistory.size() < 2) {
        return 0.0;
    }
    auto start = lsrHistory.front().QpcTime;
    auto end = lsrHistory.back().QpcTime;
    auto count = lsrHistory.size() - 1;

    double deltaT = double(end - start) / qpcFreq;
    return count / deltaT;
}

double LateStageReprojectionData::ComputeSourceFps(uint64_t qpcFreq) const
{
    return ComputeFps(mSourceHistory, qpcFreq);
}

double LateStageReprojectionData::ComputeDisplayedFps(uint64_t qpcFreq) const
{
    return ComputeFps(mDisplayedLSRHistory, qpcFreq);
}

double LateStageReprojectionData::ComputeFps(uint64_t qpcFreq) const
{
    return ComputeFps(mLSRHistory, qpcFreq);
}

LateStageReprojectionRuntimeStats LateStageReprojectionData::ComputeRuntimeStats(uint64_t qpcFreq) const
{
    LateStageReprojectionRuntimeStats stats = {};
    if (mLSRHistory.size() < 2) {
        return stats;
    }

    uint64_t totalAppSourceReleaseToLsrAcquireTime = 0;
    uint64_t totalAppSourceCpuRenderTime = 0;
    const size_t count = mLSRHistory.size();
    for (size_t i = 0; i < count; i++)
    {
        LateStageReprojectionEvent const& current = mLSRHistory[i];

        stats.mGpuPreemptionInMs.AddValue(current.GpuSubmissionToGpuStartInMs);
        stats.mGpuExecutionInMs.AddValue(current.GpuStartToGpuStopInMs);
        stats.mCopyPreemptionInMs.AddValue(current.GpuStopToCopyStartInMs);
        stats.mCopyExecutionInMs.AddValue(current.CopyStartToCopyStopInMs);

        const double lsrInputLatchToVsyncInMs =
            current.InputLatchToGpuSubmissionInMs +
            current.GpuSubmissionToGpuStartInMs +
            current.GpuStartToGpuStopInMs +
            current.GpuStopToCopyStartInMs +
            current.CopyStartToCopyStopInMs +
            current.CopyStopToVsyncInMs;
        stats.mLsrInputLatchToVsyncInMs.AddValue(lsrInputLatchToVsyncInMs);

        // Stats just with averages
        totalAppSourceReleaseToLsrAcquireTime += current.Source.GetReleaseFromRenderingToAcquireForPresentationTime();
        totalAppSourceCpuRenderTime += current.GetAppCpuRenderFrameTime();
        stats.mLsrCpuRenderTimeInMs +=
            current.CpuRenderFrameStartToHeadPoseCallbackStartInMs +
            current.HeadPoseCallbackStartToHeadPoseCallbackStopInMs +
            current.HeadPoseCallbackStopToInputLatchInMs +
            current.InputLatchToGpuSubmissionInMs;

        stats.mGpuEndToVsyncInMs += current.CopyStopToVsyncInMs;
        stats.mVsyncToPhotonsMiddleInMs += (current.TimeUntilPhotonsMiddleMs - current.TimeUntilVsyncMs);
        stats.mLsrPoseLatencyInMs += current.LsrPredictionLatencyMs;
        stats.mAppPoseLatencyInMs += current.AppPredictionLatencyMs;

        if (!current.NewSourceLatched) {
            stats.mAppMissedFrames++;
        }

        if (LateStageReprojectionMissed(current.FinalState)) {
            stats.mLsrMissedFrames += current.MissedVsyncCount;
            if (current.MissedVsyncCount > 1) {
                // We always expect a count of at least 1, but if we missed multiple vsyncs during a single LSR period we need to account for that.
                stats.mLsrConsecutiveMissedFrames += (current.MissedVsyncCount - 1);
            }
            if (i > 0 && LateStageReprojectionMissed((mLSRHistory[i - 1].FinalState))) {
                stats.mLsrConsecutiveMissedFrames++;
            }
        }
    }

    stats.mAppProcessId = mLSRHistory[count - 1].GetAppProcessId();
    stats.mLsrProcessId = mLSRHistory[count - 1].ProcessId;

    stats.mAppSourceCpuRenderTimeInMs = 1000 * double(totalAppSourceCpuRenderTime) / qpcFreq;
    stats.mAppSourceReleaseToLsrAcquireInMs = 1000 * double(totalAppSourceReleaseToLsrAcquireTime) / qpcFreq;

    stats.mAppSourceReleaseToLsrAcquireInMs /= count;
    stats.mAppSourceCpuRenderTimeInMs /= count;
    stats.mLsrCpuRenderTimeInMs /= count;
    stats.mGpuEndToVsyncInMs /= count;
    stats.mVsyncToPhotonsMiddleInMs /= count;
    stats.mLsrPoseLatencyInMs /= count;
    stats.mAppPoseLatencyInMs /= count;

    return stats;
}

bool LateStageReprojectionData::IsStale(uint64_t now) const
{
    return now - mLastUpdateTicks > LSR_TIMEOUT_THRESHOLD_TICKS;
}

void UpdateLSRCSV(PresentMonData& pm, LateStageReprojectionData& lsr, ProcessInfo* proc, LateStageReprojectionEvent& p, uint64_t perfFreq)
{
    auto file = pm.mArgs->mMultiCsv ? proc->mLsrOutputFile : pm.mLsrOutputFile;
    if (file && (p.FinalState == LateStageReprojectionResult::Presented || !pm.mArgs->mExcludeDropped)) {
        auto len = lsr.mLSRHistory.size();
        if (len > 1) {
            auto& curr = lsr.mLSRHistory[len - 1];
            auto& prev = lsr.mLSRHistory[len - 2];
            const double deltaMilliseconds = 1000 * double(curr.QpcTime - prev.QpcTime) / perfFreq;
            const double timeInSeconds = (double)(int64_t)(p.QpcTime - pm.mStartupQpcTime) / perfFreq;

            fprintf(file, "%s,%d,%d", proc->mModuleName.c_str(), curr.GetAppProcessId(), curr.ProcessId);
            if (pm.mArgs->mVerbosity >= Verbosity::Verbose)
            {
                fprintf(file, ",%d", curr.GetAppFrameId());
            }
            fprintf(file, ",%.6lf", timeInSeconds);
            if (pm.mArgs->mVerbosity > Verbosity::Simple)
            {
                double appPresentDeltaMilliseconds = 0.0;
                double appPresentToLsrMilliseconds = 0.0;
                if (curr.IsValidAppFrame())
                {
                    const uint64_t currAppPresentTime = curr.GetAppPresentTime();
                    appPresentToLsrMilliseconds = 1000 * double(curr.QpcTime - currAppPresentTime) / perfFreq;

                    if (prev.IsValidAppFrame() && (curr.GetAppProcessId() == prev.GetAppProcessId()))
                    {
                        const uint64_t prevAppPresentTime = prev.GetAppPresentTime();
                        appPresentDeltaMilliseconds = 1000 * double(currAppPresentTime - prevAppPresentTime) / perfFreq;
                    }
                }
                fprintf(file, ",%.6lf,%.6lf", appPresentDeltaMilliseconds, appPresentToLsrMilliseconds);
            }
            fprintf(file, ",%.6lf,%d,%d", deltaMilliseconds, !curr.NewSourceLatched, curr.MissedVsyncCount);
            if (pm.mArgs->mVerbosity >= Verbosity::Verbose)
            {
                fprintf(file, ",%.6lf,%.6lf", 1000 * double(curr.Source.GetReleaseFromRenderingToAcquireForPresentationTime()) / perfFreq, 1000 * double(curr.GetAppCpuRenderFrameTime()) / perfFreq);
            }
            fprintf(file, ",%.6lf", curr.AppPredictionLatencyMs);
            if (pm.mArgs->mVerbosity >= Verbosity::Verbose)
            {
                fprintf(file, ",%.6lf,%.6lf", curr.AppMispredictionMs, curr.GetLsrCpuRenderFrameMs());
            }
            fprintf(file, ",%.6lf,%.6lf,%.6lf,%.6lf,%.6lf",
                curr.LsrPredictionLatencyMs,
                curr.GetLsrMotionToPhotonLatencyMs(),
                curr.TimeUntilVsyncMs,
                curr.GetLsrThreadWakeupStartLatchToGpuEndMs(),
                curr.TotalWakeupErrorMs);
            if (pm.mArgs->mVerbosity >= Verbosity::Verbose)
            {
                fprintf(file, ",%.6lf,%.6lf,%.6lf,%.6lf,%.6lf",
                    curr.ThreadWakeupStartLatchToCpuRenderFrameStartInMs,
                    curr.CpuRenderFrameStartToHeadPoseCallbackStartInMs,
                    curr.HeadPoseCallbackStartToHeadPoseCallbackStopInMs,
                    curr.HeadPoseCallbackStopToInputLatchInMs,
                    curr.InputLatchToGpuSubmissionInMs);
            }
            fprintf(file, ",%.6lf,%.6lf,%.6lf,%.6lf,%.6lf",
                curr.GpuSubmissionToGpuStartInMs,
                curr.GpuStartToGpuStopInMs,
                curr.GpuStopToCopyStartInMs,
                curr.CopyStartToCopyStopInMs,
                curr.CopyStopToVsyncInMs);
            fprintf(file, "\n");
        }
    }
}

void UpdateConsole(PresentMonData const& pm, LateStageReprojectionData& lsr, uint64_t now, uint64_t perfFreq, std::string* display)
{
    // LSR info
    if (lsr.HasData()) {
        char str[256] = {};
        _snprintf_s(str, _TRUNCATE, "\nWindows Mixed Reality:%s\n",
            lsr.IsStale(now) ? " [STALE]" : "");
        *display += str;

        const LateStageReprojectionRuntimeStats runtimeStats = lsr.ComputeRuntimeStats(perfFreq);
        const double historyTime = lsr.ComputeHistoryTime(perfFreq);

        {
            // App
            const double fps = lsr.ComputeSourceFps(perfFreq);
            const size_t historySize = lsr.ComputeHistorySize();

            if (pm.mArgs->mVerbosity > Verbosity::Simple) {
                auto const& appProcess = pm.mProcessMap.find(runtimeStats.mAppProcessId)->second;
                _snprintf_s(str, _TRUNCATE, "\tApp - %s[%d]:\n\t\t%.2lf ms/frame (%.1lf fps, %.2lf ms CPU",
                    appProcess.mModuleName.c_str(),
                    runtimeStats.mAppProcessId,
                    1000.0 / fps,
                    fps,
                    runtimeStats.mAppSourceCpuRenderTimeInMs);
                *display += str;
            }
            else
            {
                _snprintf_s(str, _TRUNCATE, "\tApp:\n\t\t%.2lf ms/frame (%.1lf fps",
                    1000.0 / fps,
                    fps);
                *display += str;
            }

            _snprintf_s(str, _TRUNCATE, ", %.1lf%% of Compositor frame rate)\n", double(historySize - runtimeStats.mAppMissedFrames) / (historySize) * 100.0f);
            *display += str;

            _snprintf_s(str, _TRUNCATE, "\t\tMissed Present: %Iu total in last %.1lf seconds (%Iu total observed)\n",
                runtimeStats.mAppMissedFrames,
                historyTime,
                lsr.mLifetimeAppMissedFrames);
            *display += str;

            _snprintf_s(str, _TRUNCATE, "\t\tPost-Present to Compositor CPU: %.2lf ms\n",
                runtimeStats.mAppSourceReleaseToLsrAcquireInMs);
            *display += str;
        }

        {
            // LSR
            const double fps = lsr.ComputeFps(perfFreq);
            auto const& lsrProcess = pm.mProcessMap.find(runtimeStats.mLsrProcessId)->second;

            _snprintf_s(str, _TRUNCATE, "\tCompositor - %s[%d]:\n\t\t%.2lf ms/frame (%.1lf fps, %.1lf displayed fps, %.2lf ms CPU)\n",
                lsrProcess.mModuleName.c_str(),
                runtimeStats.mLsrProcessId,
                1000.0 / fps,
                fps,
                lsr.ComputeDisplayedFps(perfFreq),
                runtimeStats.mLsrCpuRenderTimeInMs);
            *display += str;

            _snprintf_s(str, _TRUNCATE, "\t\tMissed V-Sync: %Iu consecutive, %Iu total in last %.1lf seconds (%Iu total observed)\n",
                runtimeStats.mLsrConsecutiveMissedFrames,
                runtimeStats.mLsrMissedFrames,
                historyTime,
                lsr.mLifetimeLsrMissedFrames);
            *display += str;

            _snprintf_s(str, _TRUNCATE, "\t\tReprojection: %.2lf ms gpu preemption (%.2lf ms max) | %.2lf ms gpu execution (%.2lf ms max)\n",
                runtimeStats.mGpuPreemptionInMs.GetAverage(),
                runtimeStats.mGpuPreemptionInMs.GetMax(),
                runtimeStats.mGpuExecutionInMs.GetAverage(),
                runtimeStats.mGpuExecutionInMs.GetMax());
            *display += str;

            if (runtimeStats.mCopyExecutionInMs.GetAverage() > 0.0) {
                _snprintf_s(str, _TRUNCATE, "\t\tHybrid Copy: %.2lf ms gpu preemption (%.2lf ms max) | %.2lf ms gpu execution (%.2lf ms max)\n",
                    runtimeStats.mCopyPreemptionInMs.GetAverage(),
                    runtimeStats.mCopyPreemptionInMs.GetMax(),
                    runtimeStats.mCopyExecutionInMs.GetAverage(),
                    runtimeStats.mCopyExecutionInMs.GetMax());
                *display += str;
            }

            _snprintf_s(str, _TRUNCATE, "\t\tGpu-End to V-Sync: %.2lf ms\n",
                runtimeStats.mGpuEndToVsyncInMs);
            *display += str;
        }

        {
            // Latency
            _snprintf_s(str, _TRUNCATE, "\tPose Latency:\n\t\tApp Motion-to-Mid-Photon: %.2lf ms\n",
                runtimeStats.mAppPoseLatencyInMs);
            *display += str;

            _snprintf_s(str, _TRUNCATE, "\t\tCompositor Motion-to-Mid-Photon: %.2lf ms (%.2lf ms to V-Sync)\n",
                runtimeStats.mLsrPoseLatencyInMs,
                runtimeStats.mLsrInputLatchToVsyncInMs.GetAverage());
            *display += str;

            _snprintf_s(str, _TRUNCATE, "\t\tV-Sync to Mid-Photon: %.2lf ms\n",
                runtimeStats.mVsyncToPhotonsMiddleInMs);
            *display += str;
        }

        _snprintf_s(str, _TRUNCATE, "\n");
        *display += str;
    }
}
