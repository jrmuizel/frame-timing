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

#include "PresentMon.hpp"

enum {
    MAX_HISTORY_TIME = 2000,
    MAX_PRESENTS_IN_DEQUE = 60 * (MAX_HISTORY_TIME / 1000)
};

void SwapChainData::PruneDeque(std::deque<PresentEvent> &presentHistory, uint32_t msTimeDiff, uint32_t maxHistLen) {
    while (!presentHistory.empty() && (
        presentHistory.size() > maxHistLen ||
        1000.0 * QpcDeltaToSeconds(presentHistory.back().QpcTime - presentHistory.front().QpcTime) > msTimeDiff)) {
        presentHistory.pop_front();
    }
}

void SwapChainData::AddPresentToSwapChain(PresentEvent& p)
{
    if (p.FinalState == PresentResult::Presented)
    {
        mDisplayedPresentHistory.push_back(p);
    }
    if (!mPresentHistory.empty())
    {
        assert(mPresentHistory.back().QpcTime <= p.QpcTime);
    }
    mPresentHistory.push_back(p);
}

void SwapChainData::UpdateSwapChainInfo(PresentEvent&p)
{
    PruneDeque(mDisplayedPresentHistory, MAX_HISTORY_TIME, MAX_PRESENTS_IN_DEQUE);
    PruneDeque(mPresentHistory, MAX_HISTORY_TIME, MAX_PRESENTS_IN_DEQUE);

    mRuntime = p.Runtime;
    mLastSyncInterval = p.SyncInterval;
    mLastFlags = p.PresentFlags;
    if (p.FinalState == PresentResult::Presented) {
        // Prevent overwriting a valid present mode with unknown for a frame that was dropped.
        mLastPresentMode = p.PresentMode;
    }
    mLastPlane = p.PlaneIndex;
    mHasBeenBatched = p.WasBatched;
    mDwmNotified = p.DwmNotified;
}

double SwapChainData::ComputeDisplayedFps() const
{
    if (mDisplayedPresentHistory.size() < 2) {
        return 0.0;
    }
    auto start = mDisplayedPresentHistory.front().ScreenTime;
    auto end = mDisplayedPresentHistory.back().ScreenTime;
    auto count = mDisplayedPresentHistory.size() - 1;

    return count / QpcDeltaToSeconds(end - start);
}

double SwapChainData::ComputeFps() const
{
    if (mPresentHistory.size() < 2) {
        return 0.0;
    }
    auto start = mPresentHistory.front().QpcTime;
    auto end = mPresentHistory.back().QpcTime;
    auto count = mPresentHistory.size() - 1;

    return count / QpcDeltaToSeconds(end - start);
}

double SwapChainData::ComputeLatency() const
{
    if (mDisplayedPresentHistory.size() < 2) {
        return 0.0;
    }

    uint64_t totalLatency = std::accumulate(mDisplayedPresentHistory.begin(), mDisplayedPresentHistory.end() - 1, 0ull,
        [](uint64_t current, PresentEvent const& e) { return current + e.ScreenTime - e.QpcTime; });
    double average = QpcDeltaToSeconds(totalLatency) / (mDisplayedPresentHistory.size() - 1);
    return average;
}

double SwapChainData::ComputeCpuFrameTime() const
{
    if (mPresentHistory.size() < 2) {
        return 0.0;
    }

    uint64_t timeInPresent = std::accumulate(mPresentHistory.begin(), mPresentHistory.end() - 1, 0ull,
        [](uint64_t current, PresentEvent const& e) { return current + e.TimeTaken; });
    uint64_t totalTime = mPresentHistory.back().QpcTime - mPresentHistory.front().QpcTime;

    double timeNotInPresent = QpcDeltaToSeconds(totalTime - timeInPresent);
    return timeNotInPresent / (mPresentHistory.size() - 1);
}

