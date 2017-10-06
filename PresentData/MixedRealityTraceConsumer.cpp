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
	, SourceCpuRenderTime(0)
	, SourcePresentTime(0)
	, SourcePtr(0)
	, NewSourceLatched(false)
	, SourceReleaseFromRenderingToAcquireForPresentationTime(0)
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
	, SourceProcessId(0)
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

PresentationSource::PresentationSource(uint64_t ptr)
	: Ptr(ptr)
	, AcquireForRenderingTime(0)
	, ReleaseFromRenderingTime(0)
	, AcquireForPresentationTime(0)
	, ReleaseFromPresentationTime(0)
	, HolographicFrameProcessId(0)
	, HolographicFramePresentTime(0)
	, HolographicFrameCpuRenderTime(0)
{
}

PresentationSource::~PresentationSource()
{
}

HolographicFrame::HolographicFrame(EVENT_HEADER const& hdr)
	: PresentId(0)
	, HolographicFrameId(0)
	, HolographicFrameStartTime(*(uint64_t*)&hdr.TimeStamp)
	, HolographicFrameStopTime(0)
	, ProcessId(hdr.ProcessId)
	, Completed(false)
	, FinalState(HolographicFrameResult::Unknown)
{
}

HolographicFrame::~HolographicFrame()
{
	assert(Completed || gMixedRealityTraceConsumer_Exiting);
}

MRTraceConsumer::~MRTraceConsumer()
{
#ifndef NDEBUG
	gMixedRealityTraceConsumer_Exiting = true;
#endif
}

void MRTraceConsumer::CompleteLSR(std::shared_ptr<LateStageReprojectionEvent> p)
{
	if (p->FinalState == LateStageReprojectionResult::Unknown) {
		return;
	}

	if (p->Completed) {
		p->FinalState = LateStageReprojectionResult::Error;
		return;
	}

	p->Completed = true;
	{
		auto lock = scoped_lock(mMutex);
		mCompletedLSRs.push_back(p);
	}
}

void MRTraceConsumer::CompleteHolographicFrame(std::shared_ptr<HolographicFrame> p)
{
	if (p->Completed) {
		p->FinalState = HolographicFrameResult::Error;
		return;
	}

	// Remove it from any tracking structures that it may have been inserted into.
	mHolographicFramesByPresentId.erase(p->PresentId);

	p->Completed = true;
}

void MRTraceConsumer::CompletePresentationSource(uint64_t presentationSourcePtr)
{
	// Remove it from any tracking structures that it may have been inserted into.
	mPresentationSourceByPtr.erase(presentationSourcePtr);
}

decltype(MRTraceConsumer::mPresentationSourceByPtr.begin()) MRTraceConsumer::FindOrCreatePresentationSource(uint64_t presentationSourcePtr)
{
	// See if we already have a presentation source.
	auto sourceIter = mPresentationSourceByPtr.find(presentationSourcePtr);
	if (sourceIter != mPresentationSourceByPtr.end()) {
		return sourceIter;
	}

	// Create a new presentation source.
	auto newSource = std::make_shared<PresentationSource>(presentationSourcePtr);
	sourceIter = mPresentationSourceByPtr.emplace(presentationSourcePtr, newSource).first;
	return sourceIter;
}

void MRTraceConsumer::HolographicFrameStart(HolographicFrame &frame)
{
	auto pFrame = std::make_shared<HolographicFrame>(frame);

	const auto frameIter = mHolographicFramesByFrameId.find(pFrame->HolographicFrameId);
	const bool bHolographicFrameIdExists = (frameIter != mHolographicFramesByFrameId.end());
	if (bHolographicFrameIdExists) {
		// Collision with an existing in-flight Holographic FrameId.
		// Timing information for the source may be wrong if it get's timing from the wrong Holographic Frame.
		pFrame->FinalState = HolographicFrameResult::DuplicateFrameId;
	}

	mHolographicFramesByFrameId[pFrame->HolographicFrameId] = pFrame;

	// Set the caller's local event instance to completed so the assert
	// in ~HolographicFrame() doesn't fire when it is destructed.
	frame.Completed = true;
}

void MRTraceConsumer::HolographicFrameStop(std::shared_ptr<HolographicFrame> p)
{
	// Remove the frame from being tracked by FrameId.
	// Begin tracking the frame by its PresentId until LSR picks it up.
	mHolographicFramesByFrameId.erase(p->HolographicFrameId);

	assert(p->PresentId != 0);
	if (p->FinalState == HolographicFrameResult::Unknown) {
		p->FinalState = HolographicFrameResult::Presented;
	}
	mHolographicFramesByPresentId.emplace(p->PresentId, p);
}

void HandleDHDEvent(EVENT_RECORD* pEventRecord, MRTraceConsumer* mrConsumer)
{
    auto const& hdr = pEventRecord->EventHeader;
	const std::wstring taskName = GetEventTaskName(pEventRecord);

	if (taskName.compare(L"AcquireForRendering") == 0)
	{
		const uint64_t ptr = GetEventData<uint64_t>(pEventRecord, L"thisPtr");
		auto sourceIter = mrConsumer->FindOrCreatePresentationSource(ptr);
		sourceIter->second->AcquireForRenderingTime = *(uint64_t*)&hdr.TimeStamp;

		// Clear old timing data in case the Presentation Source is reused.
		sourceIter->second->ReleaseFromRenderingTime = 0;
		sourceIter->second->AcquireForPresentationTime = 0;
		sourceIter->second->ReleaseFromPresentationTime = 0;
	}
	else if (taskName.compare(L"ReleaseFromRendering") == 0)
	{
		const uint64_t ptr = GetEventData<uint64_t>(pEventRecord, L"thisPtr");
		auto sourceIter = mrConsumer->FindOrCreatePresentationSource(ptr);
		sourceIter->second->ReleaseFromRenderingTime = *(uint64_t*)&hdr.TimeStamp;
	}
	else if (taskName.compare(L"AcquireForPresentation") == 0)
	{
		const uint64_t ptr = GetEventData<uint64_t>(pEventRecord, L"thisPtr");
		auto sourceIter = mrConsumer->FindOrCreatePresentationSource(ptr);
		sourceIter->second->AcquireForPresentationTime = *(uint64_t*)&hdr.TimeStamp;
	}
	else if (taskName.compare(L"ReleaseFromPresentation") == 0)
	{
		const uint64_t ptr = GetEventData<uint64_t>(pEventRecord, L"thisPtr");
		auto sourceIter = mrConsumer->FindOrCreatePresentationSource(ptr);
		sourceIter->second->ReleaseFromPresentationTime = *(uint64_t*)&hdr.TimeStamp;
	}
	else if (taskName.compare(L"OasisPresentationSource") == 0)
	{
		std::string eventType = GetEventData<std::string>(pEventRecord, L"EventType");
		eventType.pop_back(); // Pop the null-terminator so the compare works.
		if (eventType.compare("Destruction") == 0) {
			const uint64_t ptr = GetEventData<uint64_t>(pEventRecord, L"thisPtr");
			mrConsumer->CompletePresentationSource(ptr);
		}
	}
	else if (taskName.compare(L"LsrThread_BeginLsrProcessing") == 0)
	{
		// Complete the last LSR.
		auto& pEvent = mrConsumer->mActiveLSR;
		if (pEvent) {
			if (mrConsumer->mLogUserHitches) {
				const bool bSpacePressed = (GetAsyncKeyState(VK_SPACE) & 1) == 1;
				if (bSpacePressed) {
					pEvent->UserNoticedHitch = true;
				}
			}
			
			mrConsumer->CompleteLSR(pEvent);
		}

		// Start a new LSR.
		LateStageReprojectionEvent event(hdr);
		GetEventData(pEventRecord, L"SourcePtr", &event.SourcePtr);
		GetEventData(pEventRecord, L"NewSourceLatched", &event.NewSourceLatched);
		GetEventData(pEventRecord, L"TimeUntilVblankMs", &event.TimeUntilVsyncMs);
		GetEventData(pEventRecord, L"TimeUntilPhotonsMiddleMs", &event.TimeUntilPhotonsMiddleMs);
		GetEventData(pEventRecord, L"PredictionSampleTimeToPhotonsVisibleMs", &event.AppPredictionLatencyMs);
		GetEventData(pEventRecord, L"MispredictionMs", &event.AppMispredictionMs);

		pEvent = std::make_shared<LateStageReprojectionEvent>(event);

		// Set the caller's local event instance to completed so the assert
		// in ~LateStageReprojectionEvent() doesn't fire when it is destructed.
		event.Completed = true;
	}
	else if (taskName.compare(L"LsrThread_LatchedInput") == 0)
	{
		// Update the active LSR.
		auto& pEvent = mrConsumer->mActiveLSR;
		if (pEvent) {
			// New pose latched.
			const float timeUntilPhotonsTopMs = GetEventData<float>(pEventRecord, L"TimeUntilTopPhotonsMs");
			const float timeUntilPhotonsBottomMs = GetEventData<float>(pEventRecord, L"TimeUntilBottomPhotonsMs");
			const float timeUntilPhotonsMiddleMs = (timeUntilPhotonsTopMs + timeUntilPhotonsBottomMs) / 2;
			pEvent->LsrPredictionLatencyMs = timeUntilPhotonsMiddleMs;

			// Now that we've latched, the source has been acquired for presentation.
			auto sourceIter = mrConsumer->FindOrCreatePresentationSource(pEvent->SourcePtr);
			assert(sourceIter->second->AcquireForPresentationTime != 0);

			if (!mrConsumer->mSimpleMode) {
				// Get the latest details about the Holographic Frame being used for presentation.
				// Link Holographic Frame -> Presentation Source using the presentId.
				uint32_t presentId = 0;
				GetEventData(pEventRecord, L"PresentId", &presentId);

				auto frameIter = mrConsumer->mHolographicFramesByPresentId.find(presentId);
				if (frameIter != mrConsumer->mHolographicFramesByPresentId.end()) {
					// Update the source with information about the Holographic Frame being used.
					// This data is cached as since the source is reused, but the Holographic Frame is deleted.
					sourceIter->second->HolographicFrameProcessId = frameIter->second->ProcessId;
					sourceIter->second->HolographicFrameCpuRenderTime = frameIter->second->HolographicFrameStopTime - frameIter->second->HolographicFrameStartTime;
					sourceIter->second->HolographicFramePresentTime = frameIter->second->HolographicFrameStopTime;

					// Done with this Holographic Frame. Delete it.
					mrConsumer->CompleteHolographicFrame(frameIter->second);
				}
			}

			// Update the LSR event based on the latest info in the source.
			pEvent->SourceProcessId = sourceIter->second->HolographicFrameProcessId;
			pEvent->SourceCpuRenderTime = sourceIter->second->HolographicFrameCpuRenderTime;
			pEvent->SourcePresentTime = sourceIter->second->HolographicFramePresentTime;
			pEvent->SourceReleaseFromRenderingToAcquireForPresentationTime = sourceIter->second->AcquireForPresentationTime - sourceIter->second->ReleaseFromRenderingTime;
		}
	}
	else if (taskName.compare(L"LsrThread_UnaccountedForVsyncsBetweenStatGathering") == 0)
	{
		// Update the active LSR.
		auto& pEvent = mrConsumer->mActiveLSR;
		if (pEvent) {
			// We have missed some extra Vsyncs we need to account for.
			const uint32_t unaccountedForMissedVSyncCount = GetEventData<uint32_t>(pEventRecord, L"unaccountedForVsyncsBetweenStatGathering");
			assert(unaccountedForMissedVSyncCount >= 1);
			pEvent->MissedVsyncCount += unaccountedForMissedVSyncCount;
		}
	}
	else if (taskName.compare(L"MissedPresentation") == 0)
	{
		// Update the active LSR.
		auto& pEvent = mrConsumer->mActiveLSR;
		if (pEvent) {
			// If the missed reason is for Present, increment our missed Vsync count.
			const uint32_t MissedReason = GetEventData<uint32_t>(pEventRecord, L"reason");
			if (MissedReason == 0) {
				pEvent->MissedVsyncCount++;
			}
		}
	}
	else if (taskName.compare(L"OnTimePresentationTiming") == 0 || taskName.compare(L"LatePresentationTiming") == 0)
	{
		// Update the active LSR.
		auto& pEvent = mrConsumer->mActiveLSR;
		if (pEvent) {
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

			const bool bFrameSubmittedOnSchedule = GetEventData<bool>(pEventRecord, L"frameSubmittedOnSchedule");
			if (bFrameSubmittedOnSchedule) {
				pEvent->FinalState = LateStageReprojectionResult::Presented;
			}
			else {
				pEvent->FinalState = (pEvent->MissedVsyncCount > 1) ? LateStageReprojectionResult::MissedMultiple : LateStageReprojectionResult::Missed;
			}
		}
	}
}

void HandleSpectrumContinuousEvent(EVENT_RECORD* pEventRecord, MRTraceConsumer* mrConsumer)
{
	auto const& hdr = pEventRecord->EventHeader;
	const std::wstring taskName = GetEventTaskName(pEventRecord);

	if (taskName.compare(L"HolographicFrame") == 0)
	{
		// Ignore rehydrated frames.
		const bool bIsRehydration = GetEventData<bool>(pEventRecord, L"isRehydration");
		if (!bIsRehydration) {
			switch (pEventRecord->EventHeader.EventDescriptor.Opcode)
			{
			case EVENT_TRACE_TYPE_START:
			{
				// CreateNextFrame() was called by the App.
				HolographicFrame frame(hdr);
				GetEventData(pEventRecord, L"holographicFrameID", &frame.HolographicFrameId);

				mrConsumer->HolographicFrameStart(frame);
				break;
			}
			case EVENT_TRACE_TYPE_STOP:
			{
				// PresentUsingCurrentPrediction() was called by the App.
				uint32_t holographicFrameId = 0;
				GetEventData(pEventRecord, L"holographicFrameID", &holographicFrameId);

				auto frameIter = mrConsumer->mHolographicFramesByFrameId.find(holographicFrameId);
				if (frameIter == mrConsumer->mHolographicFramesByFrameId.end()) {
					return;
				}

				const uint64_t timeStamp = *(uint64_t*)&hdr.TimeStamp;
				assert(frameIter->second->HolographicFrameStartTime <= timeStamp);
				frameIter->second->HolographicFrameStopTime = timeStamp;

				// Only stop the frame once we've seen all the events for it.
				if (frameIter->second->PresentId != 0) {
					mrConsumer->HolographicFrameStop(frameIter->second);
				}
				break;
			}
			}
		}
	}
	else if (taskName.compare(L"HolographicFrameMetadata_GetNewPoseForReprojection") == 0)
	{
		// Link holographicFrameId -> presentId.
		uint32_t holographicFrameId = 0;
		GetEventData(pEventRecord, L"holographicFrameId", &holographicFrameId);

		auto frameIter = mrConsumer->mHolographicFramesByFrameId.find(holographicFrameId);
		if (frameIter == mrConsumer->mHolographicFramesByFrameId.end()) {
			return;
		}

		GetEventData(pEventRecord, L"presentId", &frameIter->second->PresentId);

		// Only complete the frame once we've seen all the events for it.
		if (frameIter->second->HolographicFrameStopTime != 0) {
			mrConsumer->HolographicFrameStop(frameIter->second);
		}
	}
}
