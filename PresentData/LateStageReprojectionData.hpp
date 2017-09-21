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

#include <deque>
#include <stdint.h>

#include "MixedRealityTraceConsumer.hpp"

struct LateStageReprojectionRuntimeStats {
	struct RuntimeStat {
		double mAvg = 0.0;
		double mMax = 0.0;
	};

	double fps = 0.0;
	double displayedFps = 0.0;
	RuntimeStat mGPUPreemptionInMs;
	RuntimeStat mGPUExecutionInMs;
	RuntimeStat mCopyPreemptionInMs;
	RuntimeStat mCopyExecutionInMs;
	RuntimeStat mLSRInputLatchToVsync;
	RuntimeStat mLsrPoseLatency;
	RuntimeStat mAppPoseLatency;
	size_t mAppMissedFrames = 0;
	size_t mLsrMissedFrames = 0;
	size_t mLsrConsecutiveMissedFrames = 0;
	size_t mTotalLsrFrames = 0;
	double mDurationInSec = 0.0;
};

struct LateStageReprojectionData {
	size_t mLifetimeLsrMissedFrames = 0;
	size_t mLifetimeAppMissedFrames = 0;
    uint64_t mLastUpdateTicks = 0;
    std::deque<LateStageReprojectionEvent> mLSRHistory;
    std::deque<LateStageReprojectionEvent> mDisplayedLSRHistory;
	std::deque<LateStageReprojectionEvent> mMissedLSRHistory;

    void PruneDeque(std::deque<LateStageReprojectionEvent> &lsrHistory, uint64_t perfFreq, uint32_t msTimeDiff, uint32_t maxHistLen);
    void AddLateStageReprojection(LateStageReprojectionEvent& p);
    void UpdateLateStageReprojectionInfo(uint64_t now, uint64_t perfFreq);
	double ComputeHistoryTime(uint64_t qpcFreq);
    double ComputeDisplayedFps(uint64_t qpcFreq);
    double ComputeFps(uint64_t qpcFreq);
	LateStageReprojectionRuntimeStats ComputeRuntimeStats(uint64_t qpcFreq);

    bool IsStale(uint64_t now) const;
	bool HasData() const { return !mLSRHistory.empty(); }
private:
    double ComputeFps(const std::deque<LateStageReprojectionEvent>& lsrHistory, uint64_t qpcFreq);
	double ComputeHistoryTime(const std::deque<LateStageReprojectionEvent>& lsrHistory, uint64_t qpcFreq);
};
