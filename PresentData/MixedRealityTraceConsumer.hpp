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

//struct __declspec(uuid("{356e1338-04ad-420e-8b8a-a2eb678541cf}")) SPECTRUM_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{19d9d739-da0a-41a0-b97f-24ed27abc9fb}")) DHD_PROVIDER_GUID_HOLDER;
//static const auto SPECTRUM_PROVIDER_GUID = __uuidof(SPECTRUM_PROVIDER_GUID_HOLDER);
static const auto DHD_PROVIDER_GUID = __uuidof(DHD_PROVIDER_GUID_HOLDER);

// Forward-declare structs that will be used by both modern and legacy dxgkrnl events.
struct DHDBeginLSRProcessingArgs;
struct DHDPresentationTimingArgs;

enum class LateStageReprojectionResult
{
	Unknown, Presented, Missed, MissedMultiple, Error
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
    // Available from DHD
	uint64_t QpcTime;
	//uint64_t TargetVBlankQPC;
	
	bool NewSourceLatched;
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
	LateStageReprojectionResult FinalState;
	uint32_t MissedVsyncCount;

    // Additional transient state
    bool Completed;
	bool UserNoticedHitch;

	LateStageReprojectionEvent(EVENT_HEADER const& hdr);
    ~LateStageReprojectionEvent();
};

struct MRTraceConsumer
{
	MRTraceConsumer(bool simple, bool logUserHitches) 
		: mSimpleMode(simple)
		, mLogUserHitches(logUserHitches)
	{ }
    ~MRTraceConsumer();

    bool mSimpleMode;
	bool mLogUserHitches;

    std::mutex mMutex;
    // A set of LSRs that are "completed":
    // They progressed as far as they can through the pipeline before being either discarded or hitting the screen.
    // These will be handed off to the consumer thread.
    std::vector<std::shared_ptr<LateStageReprojectionEvent>> mCompletedLSRs;

    // Presents in the process of being submitted
    // The first map contains a single present that is currently in-between a set of expected events on the same thread:
    //   (e.g. DXGI_Present_Start/DXGI_Present_Stop, or Flip/QueueSubmit)
    // Used for mapping from runtime events to future events, and thread map used extensively for correlating kernel events
    //std::map<uint32_t, std::shared_ptr<LateStageReprojectionEvent>> mLSRByThreadId;
	std::shared_ptr<LateStageReprojectionEvent> mActiveLSR;
    bool DequeueLSRs(std::vector<std::shared_ptr<LateStageReprojectionEvent>>& outLSRs)
    {
        if (mCompletedLSRs.size())
        {
            auto lock = scoped_lock(mMutex);
			outLSRs.swap(mCompletedLSRs);
            return !outLSRs.empty();
        }
        return false;
    }

	//void HandleDHDBeginLSRProcessing(DHDBeginLSRProcessingArgs& args);
	//void HandleDHDPresentationTiming(DHDPresentationTimingArgs& args);

    void CompleteLSR(std::shared_ptr<LateStageReprojectionEvent> p);
    //decltype(mLSRByThreadId.begin()) FindOrCreateLSR(EVENT_HEADER const& hdr);
};

void HandleDHDEvent(EVENT_RECORD* pEventRecord, MRTraceConsumer* mrConsumer);
