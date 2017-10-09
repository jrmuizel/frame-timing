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

#include <assert.h>
#include <deque>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <vector>
#include <windows.h>
#include <evntcons.h> // must include after windows.h

#include "PresentMonTraceConsumer.hpp"

struct __declspec(uuid("{356e1338-04ad-420e-8b8a-a2eb678541cf}")) SPECTRUMCONTINUOUS_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{19d9d739-da0a-41a0-b97f-24ed27abc9fb}")) DHD_PROVIDER_GUID_HOLDER;
static const auto SPECTRUMCONTINUOUS_PROVIDER_GUID = __uuidof(SPECTRUMCONTINUOUS_PROVIDER_GUID_HOLDER);
static const auto DHD_PROVIDER_GUID = __uuidof(DHD_PROVIDER_GUID_HOLDER);

enum class LateStageReprojectionResult
{
    Unknown, Presented, Missed, MissedMultiple, Error
};

enum class HolographicFrameResult
{
    Unknown, Presented, DuplicateFrameId, Error
};

inline bool LateStageReprojectionPresented(LateStageReprojectionResult result)
{
    return (result == LateStageReprojectionResult::Presented) ? true : false;
}

inline bool LateStageReprojectionMissed(LateStageReprojectionResult result)
{
    switch (result)
    {
    case LateStageReprojectionResult::Missed:
    case LateStageReprojectionResult::MissedMultiple:
        return true;
    }

    return false;
}

struct LateStageReprojectionEvent {
    uint64_t QpcTime;
    uint32_t SourceHolographicFrameId;
    uint64_t SourceCpuRenderTime;
    uint64_t SourcePresentTime;
    uint64_t SourcePtr;
    
    bool NewSourceLatched;
    uint64_t SourceReleaseFromRenderingToAcquireForPresentationTime;

    float ThreadWakeupToCpuRenderFrameStartInMs;
    float CpuRenderFrameStartToHeadPoseCallbackStartInMs;
    float HeadPoseCallbackStartToHeadPoseCallbackStopInMs;
    float HeadPoseCallbackStopToInputLatchInMs;
    float InputLatchToGPUSubmissionInMs;
    float GpuSubmissionToGpuStartInMs;
    float GpuStartToGpuStopInMs;
    float GpuStopToCopyStartInMs;
    float CopyStartToCopyStopInMs;
    float CopyStopToVsyncInMs;

    float LsrPredictionLatencyMs;
    float AppPredictionLatencyMs;
    float AppMispredictionMs;
    float WakeupErrorMs;
    float TimeUntilVsyncMs;
    float TimeUntilPhotonsMiddleMs;

    bool EarlyLSRDueToInvalidFence;
    bool SuspendedThreadBeforeLSR;

    uint32_t ProcessId;
    uint32_t SourceProcessId;
    LateStageReprojectionResult FinalState;
    uint32_t MissedVsyncCount;

    // Additional transient state
    bool Completed;

    LateStageReprojectionEvent(EVENT_HEADER const& hdr);
    ~LateStageReprojectionEvent();

    inline float GetLsrCpuRenderMs() const
    {
        return CpuRenderFrameStartToHeadPoseCallbackStartInMs +
            HeadPoseCallbackStartToHeadPoseCallbackStopInMs +
            HeadPoseCallbackStopToInputLatchInMs +
            InputLatchToGPUSubmissionInMs;
    }

    inline float GetThreadWakeupToGpuEndMs() const
    {
        return ThreadWakeupToCpuRenderFrameStartInMs +
            CpuRenderFrameStartToHeadPoseCallbackStartInMs +
            HeadPoseCallbackStartToHeadPoseCallbackStopInMs +
            HeadPoseCallbackStopToInputLatchInMs +
            InputLatchToGPUSubmissionInMs +
            GpuSubmissionToGpuStartInMs +
            GpuStartToGpuStopInMs +
            GpuStopToCopyStartInMs +
            CopyStartToCopyStopInMs;
    }
    
    inline float GetActualLsrLatencyMs() const
    {
        return InputLatchToGPUSubmissionInMs +
            GpuSubmissionToGpuStartInMs +
            GpuStartToGpuStopInMs +
            GpuStopToCopyStartInMs +
            CopyStartToCopyStopInMs +
            CopyStopToVsyncInMs +
            (TimeUntilPhotonsMiddleMs - TimeUntilVsyncMs);
    }
};

struct PresentationSource {
    uint64_t Ptr;
    uint64_t AcquireForRenderingTime;
    uint64_t ReleaseFromRenderingTime;
    uint64_t AcquireForPresentationTime;
    uint64_t ReleaseFromPresentationTime;

    uint32_t HolographicFrameId;
    uint32_t HolographicFrameProcessId;
    uint64_t HolographicFramePresentTime;
    uint64_t HolographicFrameCpuRenderTime;

    PresentationSource(uint64_t ptr);
    ~PresentationSource();
};

struct HolographicFrame {
    uint32_t PresentId;	// Unique globally
    uint32_t HolographicFrameId;	// Unique per-process

    uint64_t HolographicFrameStartTime;
    uint64_t HolographicFrameStopTime;

    uint32_t ProcessId;
    bool Completed;
    HolographicFrameResult FinalState;

    HolographicFrame(EVENT_HEADER const& hdr);
    ~HolographicFrame();
};

struct MRTraceConsumer
{
    MRTraceConsumer(bool simple) 
        : mSimpleMode(simple)
    {}
    ~MRTraceConsumer();

    const bool mSimpleMode;

    std::mutex mMutex;
    // A set of LSRs that are "completed":
    // They progressed as far as they can through the pipeline before being either discarded or hitting the screen.
    // These will be handed off to the consumer thread.
    std::vector<std::shared_ptr<LateStageReprojectionEvent>> mCompletedLSRs;

    // Presentation sources in the process of being rendered by the app.
    std::map<uint64_t, std::shared_ptr<PresentationSource>> mPresentationSourceByPtr;

    // Stores each Holographic Frame started by it's HolographicFrameId.
    std::map<uint32_t, std::shared_ptr<HolographicFrame>> mHolographicFramesByFrameId;

    // Stores each Holographic Frame started by it's PresentId.
    std::map<uint32_t, std::shared_ptr<HolographicFrame>> mHolographicFramesByPresentId;

    std::shared_ptr<LateStageReprojectionEvent> mActiveLSR;
    bool DequeueLSRs(std::vector<std::shared_ptr<LateStageReprojectionEvent>>& outLSRs)
    {
        if (mCompletedLSRs.size()) {
            auto lock = scoped_lock(mMutex);
            outLSRs.swap(mCompletedLSRs);
            return !outLSRs.empty();
        }
        return false;
    }

    void CompleteLSR(std::shared_ptr<LateStageReprojectionEvent> p);
    void CompleteHolographicFrame(std::shared_ptr<HolographicFrame> p);
    void CompletePresentationSource(uint64_t presentationSourcePtr);

    decltype(mPresentationSourceByPtr.begin()) FindOrCreatePresentationSource(uint64_t presentationSourcePtr);
    
    void HolographicFrameStart(HolographicFrame &frame);
    void HolographicFrameStop(std::shared_ptr<HolographicFrame> p);
};

void HandleDHDEvent(EVENT_RECORD* pEventRecord, MRTraceConsumer* mrConsumer);
void HandleSpectrumContinuousEvent(EVENT_RECORD* pEventRecord, MRTraceConsumer* mrConsumer);
