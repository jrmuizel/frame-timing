#pragma once
#include "CommonIncludes.hpp"
#include "PresentMonTraceConsumer.hpp"

struct SwapChainData {
    Runtime mRuntime = Runtime::Other;
    uint64_t mLastUpdateTicks = 0;
    uint32_t mLastSyncInterval = -1;
    uint32_t mLastFlags = -1;
    std::deque<PresentEvent> mPresentHistory;
    std::deque<PresentEvent> mDisplayedPresentHistory;
    PresentMode mLastPresentMode = PresentMode::Unknown;
    uint32_t mLastPlane = 0;
    
    void PruneDeque(std::deque<PresentEvent> &presentHistory, uint64_t perfFreq, uint32_t msTimeDiff, uint32_t maxHistLen);
    void AddPresentToSwapChain(PresentEvent& p);
    void UpdateSwapChainInfo(PresentEvent&p, uint64_t now, uint64_t perfFreq);
    double ComputeDisplayedFps(uint64_t qpcFreq);
    double ComputeFps(uint64_t qpcFreq);
    double ComputeLatency(uint64_t qpcFreq);
    double ComputeCpuFrameTime(uint64_t qpcFreq);
    bool IsStale(uint64_t now) const;
private:
    double ComputeFps(const std::deque<PresentEvent>& presentHistory, uint64_t qpcFreq);
};