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

#define NOMINMAX
#include <algorithm>
#include <d3d9.h>
#include <dxgi.h>

#include "MixedRealityTraceConsumer.hpp"
#include "TraceConsumer.hpp"
#include "DxgkrnlEventStructs.hpp"

LateStageReprojectionEvent::LateStageReprojectionEvent(EVENT_HEADER const& hdr)
    : QpcTime(*(uint64_t*) &hdr.TimeStamp)
	, NewSourceLatched(false)
    , ThreadWakeupToCpuRenderFrameStartInMs(0)
	, CpuRenderFrameStartToHeadPoseCallbackStartInMs(0)
	, HeadPoseCallbackStartToHeadPoseCallbackStopInMs(0)
	, HeadPoseCallbackStopToInputLatchInMs(0)
	, InputLatchToGPUSubmissionInMs(0)
	, GpuSubmissionToGpuStartInMs(0)
	, GpuStartToGpuStopInMs(0)
	, GpuStopToCopyStartInMs(0)
	, CopyStartToCopyStopInMs(0)
	, CopyStopToVsyncInMs(0)
	, LsrPredictionLatencyMs(0)
	, AppPredictionLatencyMs(0)
	, AppMispredictionMs(0)
	, WakeupErrorMs(0)
	, TimeUntilVsyncMs(0)
	, TimeUntilPhotonsMiddleMs(0)
	, EarlyLSRDueToInvalidFence(false)
	, SuspendedThreadBeforeLSR(false)
    , ProcessId(hdr.ProcessId)
    , FinalState(LateStageReprojectionResult::Unknown)
	, MissedVsyncCount(0)
    , Completed(false)
	, UserNoticedHitch(false)
{
}

#ifndef NDEBUG
static bool gMixedRealityTraceConsumer_Exiting = false;
#endif

LateStageReprojectionEvent::~LateStageReprojectionEvent()
{
    assert(Completed || gMixedRealityTraceConsumer_Exiting);
}

MRTraceConsumer::~MRTraceConsumer()
{
#ifndef NDEBUG
	gMixedRealityTraceConsumer_Exiting = true;
#endif
}

void HandleDHDEvent(EVENT_RECORD* pEventRecord, MRTraceConsumer* mrConsumer)
{
    auto const& hdr = pEventRecord->EventHeader;
	const std::wstring taskName = GetEventTaskName(pEventRecord);

	if (taskName.compare(L"LsrThread_BeginLsrProcessing") == 0)
	{
		// Complete the last LSR
		auto& pEvent = mrConsumer->mActiveLSR;
		if (pEvent)
		{
			if (mrConsumer->mLogUserHitches)
			{
				const bool bSpacePressed = (GetAsyncKeyState(VK_SPACE) & 1) == 1;
				if (bSpacePressed)
				{
					pEvent->UserNoticedHitch = true;
				}
			}
			
			mrConsumer->CompleteLSR(pEvent);
		}

		// Start a new LSR
		LateStageReprojectionEvent event(hdr);
		GetEventData(pEventRecord, L"NewSourceLatched", &event.NewSourceLatched);
		GetEventData(pEventRecord, L"TimeUntilVblankMs", &event.TimeUntilVsyncMs);
		GetEventData(pEventRecord, L"TimeUntilPhotonsMiddleMs", &event.TimeUntilPhotonsMiddleMs);
		GetEventData(pEventRecord, L"PredictionSampleTimeToPhotonsVisibleMs", &event.AppPredictionLatencyMs);
		GetEventData(pEventRecord, L"MispredictionMs", &event.AppMispredictionMs);

		pEvent = std::make_shared<LateStageReprojectionEvent>(event);

		// Set the caller's local event instance to completed so the assert
		// in ~PresentEvent() doesn't fire when it is destructed.
		event.Completed = true;
	}
	else if (taskName.compare(L"LsrThread_LatchedInput") == 0)
	{
		// Update the active LSR.
		auto& pEvent = mrConsumer->mActiveLSR;
		if (pEvent)
		{
			// New pose latched.
			float TimeUntilPhotonsTopMs = 0.0f;
			float TimeUntilPhotonsBottomMs = 0.0f;
			GetEventData(pEventRecord, L"TimeUntilTopPhotonsMs", &TimeUntilPhotonsTopMs);
			GetEventData(pEventRecord, L"TimeUntilBottomPhotonsMs", &TimeUntilPhotonsBottomMs);

			const float TimeUntilPhotonsMiddleMs = (TimeUntilPhotonsTopMs + TimeUntilPhotonsBottomMs) / 2;
			pEvent->LsrPredictionLatencyMs = TimeUntilPhotonsMiddleMs;
		}
	}
	else if (taskName.compare(L"LsrThread_UnaccountedForVsyncsBetweenStatGathering") == 0)
	{
		// Update the active LSR.
		auto& pEvent = mrConsumer->mActiveLSR;
		if (pEvent)
		{
			// We have missed some extra Vsyncs we need to account for.
			uint32_t UnAccountedForMissedVSyncCount = 0;
			GetEventData(pEventRecord, L"unaccountedForVsyncsBetweenStatGathering", &UnAccountedForMissedVSyncCount);
			assert(UnAccountedForMissedVSyncCount >= 1);
			pEvent->MissedVsyncCount += UnAccountedForMissedVSyncCount;
		}
	}
	else if (taskName.compare(L"MissedPresentation") == 0)
	{
		// Update the active LSR.
		auto& pEvent = mrConsumer->mActiveLSR;
		if (pEvent)
		{
			// If the missed reason is for Present, increment our missed Vsync count.
			uint32_t MissedReason = static_cast<uint32_t>(-1);
			GetEventData(pEventRecord, L"reason", &MissedReason);
			if (MissedReason == 0)
			{
				pEvent->MissedVsyncCount++;
			}
		}
	}
	else if (taskName.compare(L"OnTimePresentationTiming") == 0 || taskName.compare(L"LatePresentationTiming") == 0)
	{
		// Update the active LSR.
		auto& pEvent = mrConsumer->mActiveLSR;
		if (pEvent)
		{
			GetEventData(pEventRecord, L"threadWakeupToCpuRenderFrameStartInMs", &pEvent->ThreadWakeupToCpuRenderFrameStartInMs);
			GetEventData(pEventRecord, L"cpuRenderFrameStartToHeadPoseCallbackStartInMs", &pEvent->CpuRenderFrameStartToHeadPoseCallbackStartInMs);
			GetEventData(pEventRecord, L"headPoseCallbackDurationInMs", &pEvent->HeadPoseCallbackStartToHeadPoseCallbackStopInMs);
			GetEventData(pEventRecord, L"headPoseCallbackEndToInputLatchInMs", &pEvent->HeadPoseCallbackStopToInputLatchInMs);
			GetEventData(pEventRecord, L"inputLatchToGpuSubmissionInMs", &pEvent->InputLatchToGPUSubmissionInMs);
			GetEventData(pEventRecord, L"gpuSubmissionToGpuStartInMs", &pEvent->GpuSubmissionToGpuStartInMs);
			GetEventData(pEventRecord, L"gpuStartToGpuStopInMs", &pEvent->GpuStartToGpuStopInMs);
			GetEventData(pEventRecord, L"gpuStopToCopyStartInMs", &pEvent->GpuStopToCopyStartInMs);
			GetEventData(pEventRecord, L"copyStartToCopyStopInMs", &pEvent->CopyStartToCopyStopInMs);
			GetEventData(pEventRecord, L"copyStopToVsyncInMs", &pEvent->CopyStopToVsyncInMs);

			GetEventData(pEventRecord, L"wakeupErrorInMs", &pEvent->WakeupErrorMs);
			GetEventData(pEventRecord, L"earlyLSRDueToInvalidFence", &pEvent->EarlyLSRDueToInvalidFence);
			GetEventData(pEventRecord, L"suspendedThreadBeforeLSR", &pEvent->SuspendedThreadBeforeLSR);

			bool bFrameSubmittedOnSchedule = false;
			GetEventData(pEventRecord, L"frameSubmittedOnSchedule", &bFrameSubmittedOnSchedule);
			if (bFrameSubmittedOnSchedule)
			{
				pEvent->FinalState = LateStageReprojectionResult::Presented;
			}
			else
			{
				pEvent->FinalState = (pEvent->MissedVsyncCount > 1) ? LateStageReprojectionResult::MissedMultiple : LateStageReprojectionResult::Missed;
			}
		}
	}
}

void MRTraceConsumer::CompleteLSR(std::shared_ptr<LateStageReprojectionEvent> p)
{
	if (p->FinalState == LateStageReprojectionResult::Unknown)
	{
		return;
	}

	if (p->Completed)
	{
		p->FinalState = LateStageReprojectionResult::Error;
		return;
	}

	p->Completed = true;
	{
		auto lock = scoped_lock(mMutex);
		mCompletedLSRs.push_back(p);
	}
}