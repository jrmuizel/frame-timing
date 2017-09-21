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

#include "LateStageReprojectionData.hpp"

#include <algorithm>

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
    if (p.FinalState == LateStageReprojectionResult::Presented)
    {
		mDisplayedLSRHistory.push_back(p);
    }
	else if(p.FinalState == LateStageReprojectionResult::Missed)
	{
		mLifetimeLsrMissedFrames += p.MissedVsyncCount;
	}

	if (!p.NewSourceLatched)
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
    PruneDeque(mDisplayedLSRHistory, perfFreq, MAX_HISTORY_TIME, MAX_LSRS_IN_DEQUE);
    PruneDeque(mLSRHistory, perfFreq, MAX_HISTORY_TIME, MAX_LSRS_IN_DEQUE);

    mLastUpdateTicks = now;
}

double LateStageReprojectionData::ComputeHistoryTime(const std::deque<LateStageReprojectionEvent>& lsrHistory, uint64_t qpcFreq)
{
	if (lsrHistory.size() < 2) {
		return 0.0;
	}

	auto start = lsrHistory.front().QpcTime;
	auto end = lsrHistory.back().QpcTime;
	return double(end - start) / qpcFreq;
}

double LateStageReprojectionData::ComputeHistoryTime(uint64_t qpcFreq)
{
	return ComputeHistoryTime(mLSRHistory, qpcFreq);
}

double LateStageReprojectionData::ComputeFps(const std::deque<LateStageReprojectionEvent>& lsrHistory, uint64_t qpcFreq)
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

double LateStageReprojectionData::ComputeDisplayedFps(uint64_t qpcFreq)
{
    return ComputeFps(mDisplayedLSRHistory, qpcFreq);
}

double LateStageReprojectionData::ComputeFps(uint64_t qpcFreq)
{
    return ComputeFps(mLSRHistory, qpcFreq);
}

LateStageReprojectionRuntimeStats LateStageReprojectionData::ComputeRuntimeStats(uint64_t qpcFreq)
{
	LateStageReprojectionRuntimeStats stats = {};
	if (mLSRHistory.size() < 2) {
		stats;
	}

	stats.fps = ComputeFps(qpcFreq);
	stats.displayedFps = ComputeDisplayedFps(qpcFreq);
	stats.mDurationInSec = ComputeHistoryTime(qpcFreq);

	auto count = mLSRHistory.size() - 1;
	stats.mTotalLsrFrames = mLSRHistory.size();
	for (size_t i = 0; i <= count; i++)
	{
		LateStageReprojectionEvent& current = mLSRHistory[i];

		LateStageReprojectionRuntimeStats::RuntimeStat* pRuntimeStat = &stats.mGPUPreemptionInMs;
		pRuntimeStat->mAvg += current.GpuSubmissionToGpuStartInMs;
		pRuntimeStat->mMax = std::max<double>(pRuntimeStat->mMax, current.GpuSubmissionToGpuStartInMs);

		pRuntimeStat = &stats.mGPUExecutionInMs;
		pRuntimeStat->mAvg += current.GpuStartToGpuStopInMs;
		pRuntimeStat->mMax = std::max<double>(pRuntimeStat->mMax, current.GpuStartToGpuStopInMs);

		pRuntimeStat = &stats.mCopyPreemptionInMs;
		pRuntimeStat->mAvg += current.GpuStopToCopyStartInMs;
		pRuntimeStat->mMax = std::max<double>(pRuntimeStat->mMax, current.GpuStopToCopyStartInMs);

		pRuntimeStat = &stats.mCopyExecutionInMs;
		pRuntimeStat->mAvg += current.CopyStartToCopyStopInMs;
		pRuntimeStat->mMax = std::max<double>(pRuntimeStat->mMax, current.CopyStartToCopyStopInMs);

		pRuntimeStat = &stats.mLSRInputLatchToVsync;
		const double lsrInputLatchToVsync =
			current.InputLatchToGPUSubmissionInMs +
			current.GpuSubmissionToGpuStartInMs +
			current.GpuStartToGpuStopInMs +
			current.GpuStopToCopyStartInMs +
			current.CopyStartToCopyStopInMs +
			current.CopyStopToVsyncInMs;
		pRuntimeStat->mAvg += lsrInputLatchToVsync;
		pRuntimeStat->mMax = std::max<double>(pRuntimeStat->mMax, lsrInputLatchToVsync);

		pRuntimeStat = &stats.mLsrPoseLatency;
		pRuntimeStat->mAvg += current.LsrPredictionLatencyMs;
		pRuntimeStat->mMax = std::max<double>(pRuntimeStat->mMax, current.LsrPredictionLatencyMs);

		pRuntimeStat = &stats.mAppPoseLatency;
		pRuntimeStat->mAvg += current.AppPredictionLatencyMs;
		pRuntimeStat->mMax = std::max<double>(pRuntimeStat->mMax, current.AppPredictionLatencyMs);

		if (!current.NewSourceLatched)
		{
			stats.mAppMissedFrames++;
		}

		if (current.FinalState == LateStageReprojectionResult::Missed)
		{
			assert(current.MissedVsyncCount >= 1);
			stats.mLsrMissedFrames += current.MissedVsyncCount;
			if (current.MissedVsyncCount > 1)
			{
				// We always expect a count of at least 1, but if we missed multiple vsyncs during a single LSR period we need to account for that.
				stats.mLsrConsecutiveMissedFrames += (current.MissedVsyncCount - 1);
			}
			if (i > 0 && (mLSRHistory[i - 1].FinalState == LateStageReprojectionResult::Missed))
			{
				stats.mLsrConsecutiveMissedFrames++;
			}
		}
	}

	stats.mGPUPreemptionInMs.mAvg /= count;
	stats.mGPUExecutionInMs.mAvg /= count;
	stats.mCopyPreemptionInMs.mAvg /= count;
	stats.mCopyExecutionInMs.mAvg /= count;
	stats.mLSRInputLatchToVsync.mAvg /= count;
	stats.mLsrPoseLatency.mAvg /= count;
	stats.mAppPoseLatency.mAvg /= count;

	return stats;
}

bool LateStageReprojectionData::IsStale(uint64_t now) const
{
    return now - mLastUpdateTicks > LSR_TIMEOUT_THRESHOLD_TICKS;
}
