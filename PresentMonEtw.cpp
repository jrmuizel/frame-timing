//--------------------------------------------------------------------------------------
// Copyright 2015 Intel Corporation
// All Rights Reserved
//
// Permission is granted to use, copy, distribute and prepare derivative works of this
// software for any purpose and without fee, provided, that the above copyright notice
// and this statement appear in all copies.  Intel makes no representations about the
// suitability of this software for any purpose.  THIS SOFTWARE IS PROVIDED "AS IS."
// INTEL SPECIFICALLY DISCLAIMS ALL WARRANTIES, EXPRESS OR IMPLIED, AND ALL LIABILITY,
// INCLUDING CONSEQUENTIAL AND OTHER INDIRECT DAMAGES, FOR THE USE OF THIS SOFTWARE,
// INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PROPRIETARY RIGHTS, AND INCLUDING THE
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.  Intel does not
// assume any responsibility for any errors which may appear in this software nor any
// responsibility to update it.
//--------------------------------------------------------------------------------------

#include "PresentMon.hpp"
#include "TraceSession.hpp"

#include <cstdio>
#include <cassert>

#include <string>
#include <thread>
#include <tdh.h>
#include <dxgi.h>
#include <set>
#include <d3d9.h>
#include <algorithm>

struct __declspec(uuid("{CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}")) DXGI_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{802ec45a-1e99-4b83-9920-87c98277ba9d}")) DXGKRNL_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{8c416c79-d49b-4f01-a467-e56d3aa8234c}")) WIN32K_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{9e9bba3c-2e38-40cb-99f4-9e8281425164}")) DWM_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{783ACA0A-790E-4d7f-8451-AA850511C6B9}")) D3D9_PROVIDER_GUID_HOLDER;
static const auto DXGI_PROVIDER_GUID = __uuidof(DXGI_PROVIDER_GUID_HOLDER);
static const auto DXGKRNL_PROVIDER_GUID = __uuidof(DXGKRNL_PROVIDER_GUID_HOLDER);
static const auto WIN32K_PROVIDER_GUID = __uuidof(WIN32K_PROVIDER_GUID_HOLDER);
static const auto DWM_PROVIDER_GUID = __uuidof(DWM_PROVIDER_GUID_HOLDER);
static const auto D3D9_PROVIDER_GUID = __uuidof(D3D9_PROVIDER_GUID_HOLDER);

extern bool g_Quit;

class TraceEventInfo
{
public:
    TraceEventInfo(PEVENT_RECORD pEvent)
    : pEvent(pEvent) {
        unsigned long bufferSize = 0;
        auto result = TdhGetEventInformation(pEvent, 0, nullptr, nullptr, &bufferSize);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
            pInfo = reinterpret_cast<TRACE_EVENT_INFO*>(operator new(bufferSize));
            result = TdhGetEventInformation(pEvent, 0, nullptr, pInfo, &bufferSize);
        }
        if (result != ERROR_SUCCESS) {
            throw std::exception("Unexpected error from TdhGetEventInformation.", result);
        }
    }
    TraceEventInfo(const TraceEventInfo&) = delete;
    TraceEventInfo& operator=(const TraceEventInfo&) = delete;
    TraceEventInfo(TraceEventInfo&& o) {
        *this = std::move(o);
    }
    TraceEventInfo& operator=(TraceEventInfo&& o) {
        if (pInfo) {
            operator delete(pInfo);
        }
        pInfo = o.pInfo;
        pEvent = o.pEvent;
        o.pInfo = nullptr;
        return *this;
    }

    ~TraceEventInfo() {
        operator delete(pInfo);
        pInfo = nullptr;
    }

    void GetData(PCWSTR name, byte* outData, uint32_t dataSize) {
        PROPERTY_DATA_DESCRIPTOR descriptor;
        descriptor.ArrayIndex = 0;
        descriptor.PropertyName = reinterpret_cast<unsigned long long>(name);
        auto result = TdhGetProperty(pEvent, 0, nullptr, 1, &descriptor, dataSize, outData);
        if (result != ERROR_SUCCESS) {
            throw std::exception("Unexpected error from TdhGetProperty.", result);
        }
    }

    template <typename T>
    T GetData(PCWSTR name) {
        T local;
        GetData(name, reinterpret_cast<byte*>(&local), sizeof(local));
        return local;
    }

    uint64_t GetPtr(PCWSTR name) {
        if (pEvent->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) {
            return GetData<uint32_t>(name);
        } else if (pEvent->EventHeader.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER) {
            return GetData<uint64_t>(name);
        }
        return 0;
    }

private:
    TRACE_EVENT_INFO* pInfo;
    EVENT_RECORD* pEvent;
};

struct DxgiConsumer : ITraceConsumer
{
    CRITICAL_SECTION mMutex;
    // A set of presents that are "completed":
    // They progressed as far as they can through the pipeline before being either discarded or hitting the screen.
    // These will be handed off to the consumer thread.
    std::vector<std::shared_ptr<PresentEvent>> mCompletedPresents;

    // Presents in the process of being submitted
    // The first map contains a single present that is currently in-between a set of expected events on the same thread:
    //   (e.g. DXGI_Present_Start/DXGI_Present_Stop, or Flip/QueueSubmit)
    // The second map contains a queue of presents currently pending for a process
    //   These presents have been "batched" and will be submitted by a driver worker thread
    //   The assumption is that they will be submitted to kernel in the same order they were submitted to DXGI,
    //   but this might not hold true especially if there are multiple D3D devices in play
    std::map<uint32_t, std::shared_ptr<PresentEvent>> mPendingPresentByThread;
    std::map<uint32_t, std::deque<std::shared_ptr<PresentEvent>>> mPendingPresentsByProcess;

    // Fullscreen presents map from present sequence id
    std::map<uint32_t, std::shared_ptr<PresentEvent>> mFullscreenPresents;

    // Present history tokens
    typedef std::tuple<uint64_t, uint64_t, uint32_t> PresentHistoryTokenKey;
    std::map<PresentHistoryTokenKey, std::shared_ptr<PresentEvent>> mPresentHistoryTokens;
    std::map<uint64_t, std::shared_ptr<PresentEvent>> mPresentHistoryByTokenPtr;

    // Present by window, used for determining superceding presents
    std::map<uint64_t, std::shared_ptr<PresentEvent>> mPresentByWindow;

    // Presents that will be completed by DWM's next present
    uint32_t DwmPresentThreadId = 0;
    std::deque<std::shared_ptr<PresentEvent>> mWaitingForDWM;
    std::set<uint32_t> mWindowsBeingComposed;

    // Legacy blit presents go down a very different path
    std::map<uint64_t, std::shared_ptr<PresentEvent>> mLegacyBlitPresentMap;

    bool DequeuePresents(std::vector<std::shared_ptr<PresentEvent>>& outPresents)
    {
        if (mCompletedPresents.size())
        {
            EnterCriticalSection(&mMutex);
            outPresents.swap(mCompletedPresents);
            LeaveCriticalSection(&mMutex);
            return !outPresents.empty();
        }
        return false;
    }

    DxgiConsumer() {
        InitializeCriticalSection(&mMutex);
    }
    ~DxgiConsumer() {
        DeleteCriticalSection(&mMutex);
    }

    virtual void OnEventRecord(_In_ PEVENT_RECORD pEventRecord);
    virtual bool ContinueProcessing() { return !g_Quit; }

private:
    void CompletePresent(std::shared_ptr<PresentEvent> p)
    {
        for (auto& p2 : p->DependentPresents) {
            p2->ScreenTime = p->ScreenTime;
            CompletePresent(p2);
        }
        p->DependentPresents.clear();

        if (p->QueueSubmitSequence != 0) {
            mFullscreenPresents.erase(p->QueueSubmitSequence);
        }
        if (p->Hwnd != 0) {
            mPresentByWindow.erase(p->Hwnd);
        }

        EnterCriticalSection(&mMutex);
        mCompletedPresents.push_back(p);
        LeaveCriticalSection(&mMutex);
    }

    decltype(mPendingPresentByThread.begin()) FindOrCreatePresent(_In_ PEVENT_RECORD pEventRecord)
    {
        auto eventIter = mPendingPresentByThread.find(pEventRecord->EventHeader.ThreadId);
        if (eventIter != mPendingPresentByThread.end()) {
            return eventIter;
        }

        auto& processDeque = mPendingPresentsByProcess[pEventRecord->EventHeader.ProcessId];
        uint64_t EventTime = *(uint64_t*)&pEventRecord->EventHeader.TimeStamp;
        if (processDeque.empty()) {
            auto newEvent = std::make_shared<PresentEvent>();
            newEvent->QpcTime = EventTime;
            newEvent->ProcessId = pEventRecord->EventHeader.ProcessId;
            eventIter = mPendingPresentByThread.emplace(pEventRecord->EventHeader.ThreadId, newEvent).first;
        } else {
            assert(processDeque.front()->QpcTime < EventTime);
            eventIter = mPendingPresentByThread.emplace(pEventRecord->EventHeader.ThreadId, processDeque.front()).first;
            processDeque.pop_front();
        }

        return eventIter;
    }

    void OnDXGIEvent(_In_ PEVENT_RECORD pEventRecord);
    void OnDXGKrnlEvent(_In_ PEVENT_RECORD pEventRecord);
    void OnWin32kEvent(_In_ PEVENT_RECORD pEventRecord);
    void OnDWMEvent(_In_ PEVENT_RECORD pEventRecord);
    void OnD3D9Event(_In_ PEVENT_RECORD pEventRecord);
};

void DxgiConsumer::OnDXGIEvent(PEVENT_RECORD pEventRecord)
{
    enum {
        DXGIPresent_Start = 42,
        DXGIPresent_Stop,
    };

    auto& hdr = pEventRecord->EventHeader;
    uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;
    switch (hdr.EventDescriptor.Id)
    {
        case DXGIPresent_Start:
        {
            PresentEvent event;
            event.ProcessId = hdr.ProcessId;
            event.QpcTime = EventTime;
        
            TraceEventInfo eventInfo(pEventRecord);
            event.SwapChainAddress = eventInfo.GetPtr(L"pIDXGISwapChain");
            event.SyncInterval = eventInfo.GetData<uint32_t>(L"SyncInterval");
            event.PresentFlags = eventInfo.GetData<uint32_t>(L"Flags");
            event.Runtime = Runtime::DXGI;
        
            // Ignore PRESENT_TEST: it's just to check if you're still fullscreen, doesn't actually do anything
            if ((event.PresentFlags & DXGI_PRESENT_TEST) == 0) {
                mPendingPresentByThread[hdr.ThreadId] = std::make_shared<PresentEvent>(event);
            }
            break;
        }
        case DXGIPresent_Stop:
        {
            auto eventIter = mPendingPresentByThread.find(hdr.ThreadId);
            if (eventIter == mPendingPresentByThread.end()) {
                return;
            }
            auto &event = *eventIter->second;

            uint64_t EndTime = EventTime;
            assert(event.QpcTime < EndTime);
            event.TimeTaken = EndTime - event.QpcTime;

            if (event.PresentMode == PresentMode::Unknown) {
                auto& eventDeque = mPendingPresentsByProcess[hdr.ProcessId];
                eventDeque.push_back(eventIter->second);

#if _DEBUG
                auto& container = mPendingPresentsByProcess[hdr.ProcessId];
                for (UINT i = 0; i < container.size() - 1; ++i)
                {
                    assert(container[i]->QpcTime <= container[i + 1]->QpcTime);
                }
#endif

                const uint32_t cMaxPendingPresents = 20;
                if (eventDeque.size() > cMaxPendingPresents)
                {
                    CompletePresent(eventDeque.front());
                    eventDeque.pop_front();
                }
            }
            break;
        }
    }
}

void DxgiConsumer::OnDXGKrnlEvent(PEVENT_RECORD pEventRecord)
{
    enum {
        DxgKrnl_Flip = 168,
        DxgKrnl_FlipMPO = 252,
        DxgKrnl_QueueSubmit = 178,
        DxgKrnl_QueueComplete = 180,
        DxgKrnl_MMIOFlip = 116,
        DxgKrnl_MMIOFlipMPO = 259,
        DxgKrnl_VSyncDPC = 17,
        DxgKrnl_Present = 184,
        DxgKrnl_PresentHistoryDetailed = 215,
        DxgKrnl_SubmitPresentHistory = 171,
        DxgKrnl_PropagatePresentHistory = 172,
        DxgKrnl_Blit = 166,
    };

    auto& hdr = pEventRecord->EventHeader;
    switch (hdr.EventDescriptor.Id) 
    {
    case DxgKrnl_Flip:
    case DxgKrnl_FlipMPO:
    case DxgKrnl_QueueSubmit:
    case DxgKrnl_QueueComplete:
    case DxgKrnl_MMIOFlip:
    case DxgKrnl_MMIOFlipMPO:
    case DxgKrnl_VSyncDPC:
    case DxgKrnl_Present:
    case DxgKrnl_PresentHistoryDetailed:
    case DxgKrnl_SubmitPresentHistory:
    case DxgKrnl_PropagatePresentHistory:
    case DxgKrnl_Blit:
        break;
    default:
        return;
    }

    uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;

    TraceEventInfo eventInfo(pEventRecord);
    switch (hdr.EventDescriptor.Id)
    {
        case DxgKrnl_Flip:
        case DxgKrnl_FlipMPO:
        {
            // A flip event is emitted during fullscreen present submission.
            // Afterwards, expect an MMIOFlip packet on the same thread, used
            // to trace the flip to screen.
            auto eventIter = FindOrCreatePresent(pEventRecord);

            if (eventIter->second->PresentMode != PresentMode::Unknown) {
                // For MPO, N events may be issued, but we only care about the first
                return;
            }
            
            eventIter->second->PresentMode = PresentMode::Fullscreen;
            if (eventIter->second->Runtime == Runtime::D3D9) {
                // To avoid a rundown, we'll poll the sync interval for each packet from some other component
                eventIter->second->SyncInterval = eventInfo.GetData<uint32_t>(L"FlipInterval");
            }

            if (hdr.ThreadId == DwmPresentThreadId) {
                std::swap(eventIter->second->DependentPresents, mWaitingForDWM);
                DwmPresentThreadId = 0;
            }
            break;
        }
        case DxgKrnl_QueueSubmit:
        {
            // A QueueSubmit can be many types, but these are interesting for present.
            // This event is emitted after a flip/blt/PHT event, and may be the only way
            // to trace completion of the present.
            enum class DxgKrnl_QueueSubmit_Type {
                MMIOFlip = 3,
                Software = 7,
            };
            auto Type = eventInfo.GetData<DxgKrnl_QueueSubmit_Type>(L"PacketType");
            auto SubmitSequence = eventInfo.GetData<uint32_t>(L"SubmitSequence");
            bool Present = eventInfo.GetData<BOOL>(L"bPresent") != 0;

            if (Type == DxgKrnl_QueueSubmit_Type::MMIOFlip ||
                Type == DxgKrnl_QueueSubmit_Type::Software ||
                Present) {
                auto eventIter = mPendingPresentByThread.find(hdr.ThreadId);
                if (eventIter == mPendingPresentByThread.end()) {
                    return;
                }

                eventIter->second->QueueSubmitSequence = SubmitSequence;
                mFullscreenPresents.emplace(SubmitSequence, eventIter->second);
            }
            break;
        }
        case DxgKrnl_QueueComplete:
        {
            auto SubmitSequence = eventInfo.GetData<uint32_t>(L"SubmitSequence");
            auto eventIter = mFullscreenPresents.find(SubmitSequence);
            if (eventIter == mFullscreenPresents.end()) {
                return;
            }

            if (eventIter->second->PresentMode == PresentMode::Fullscreen_Blit) {
                eventIter->second->ScreenTime = EventTime;
                eventIter->second->FinalState = PresentResult::Presented;
                CompletePresent(eventIter->second);
            }
            break;
        }
        case DxgKrnl_MMIOFlip:
        {
            // An MMIOFlip event is emitted when an MMIOFlip packet is dequeued.
            // This corresponds to all GPU work prior to the flip being completed
            // (i.e. present "ready")
            // It also is emitted when an independent flip PHT is dequed,
            // and will tell us whether the present is immediate or vsync.
            enum DxgKrnl_MMIOFlip_Flags {
                FlipImmediate = 0x2,
                FlipOnNextVSync = 0x4
            };

            auto FlipSubmitSequence = eventInfo.GetData<uint32_t>(L"FlipSubmitSequence");
            auto Flags = eventInfo.GetData<DxgKrnl_MMIOFlip_Flags>(L"Flags");

            auto eventIter = mFullscreenPresents.find(FlipSubmitSequence);
            if (eventIter == mFullscreenPresents.end()) {
                return;
            }

            eventIter->second->ReadyTime = EventTime;

            if (Flags & DxgKrnl_MMIOFlip_Flags::FlipImmediate) {
                eventIter->second->FinalState = PresentResult::Presented;
                eventIter->second->ScreenTime = *(uint64_t*)&pEventRecord->EventHeader.TimeStamp;
                if (eventIter->second->PresentMode == PresentMode::Fullscreen) {
                    CompletePresent(eventIter->second);
                } else {
                    eventIter->second->PresentMode = PresentMode::ImmediateIndependentFlip;
                }
            }

            break;
        }
        case DxgKrnl_MMIOFlipMPO:
        {
            // See above for more info about this packet.
            auto FlipFenceId = eventInfo.GetData<uint64_t>(L"FlipSubmitSequence");
            uint32_t FlipSubmitSequence = (uint32_t)(FlipFenceId >> 32u);

            auto eventIter = mFullscreenPresents.find(FlipSubmitSequence);
            if (eventIter == mFullscreenPresents.end()) {
                return;
            }

            // Avoid double-marking a single present packet coming from the MPO API
            if (eventIter->second->ReadyTime == 0) {
                eventIter->second->ReadyTime = EventTime;
                eventIter->second->PlaneIndex = eventInfo.GetData<uint32_t>(L"LayerIndex");
            }

            if (eventIter->second->PresentMode == PresentMode::IndependentFlip ||
                eventIter->second->PresentMode == PresentMode::Composed_Flip) {
                eventIter->second->PresentMode = PresentMode::IndependentFlipMPO;
            }

            break;
        }
        case DxgKrnl_VSyncDPC:
        {
            // The VSyncDPC contains a field telling us what flipped to screen.
            // This is the way to track completion of a fullscreen present.
            auto FlipFenceId = eventInfo.GetData<uint64_t>(L"FlipFenceId");

            uint32_t FlipSubmitSequence = (uint32_t)(FlipFenceId >> 32u);
            auto eventIter = mFullscreenPresents.find(FlipSubmitSequence);
            if (eventIter == mFullscreenPresents.end()) {
                return;
            }
            
            eventIter->second->ScreenTime = *(uint64_t*)&pEventRecord->EventHeader.TimeStamp;
            eventIter->second->FinalState = PresentResult::Presented;
            if (eventIter->second->PresentMode == PresentMode::Fullscreen) {
                CompletePresent(eventIter->second);
            }
            break;
        }
        case DxgKrnl_Present:
        {
            // This event is emitted at the end of the kernel present, before returning.
            // All other events have already been logged, but this one contains one
            // extra piece of useful information: the hWnd that a present targeted,
            // used to determine when presents are discarded instead of composed.
            auto eventIter = mPendingPresentByThread.find(hdr.ThreadId);
            if (eventIter == mPendingPresentByThread.end()) {
                return;
            }

            auto hWnd = eventInfo.GetPtr(L"hWindow");

            if (eventIter->second->PresentMode == PresentMode::Windowed_Blit) {
                auto hWndIter = mPresentByWindow.find(hWnd);
                if (hWndIter == mPresentByWindow.end()) {
                    mPresentByWindow.emplace(hWnd, eventIter->second);
                } else if (hWndIter->second != eventIter->second) {
                    auto eventPtr = hWndIter->second;
                    hWndIter->second = eventIter->second;

                    eventPtr->FinalState = PresentResult::Discarded;
                    if (eventPtr->ReadyTime != 0) {
                        // This won't make it to screen, go ahead and complete it now
                        CompletePresent(eventPtr);
                    }
                }
            } else {
                eventIter->second->Hwnd = hWnd;
            }

            if (eventIter->second->TimeTaken != 0 || eventIter->second->Runtime == Runtime::Other) {
                mPendingPresentByThread.erase(eventIter);
            }
            break;
        }
        case DxgKrnl_PresentHistoryDetailed:
        {
            // This event is emitted during submission of a windowed present.
            // In the case of flip model, it is used to find a key to watch for the
            // event which triggers the "ready" state.
            // In the case of blit model, it is used to distinguish between fs/windowed.
            auto eventIter = mPendingPresentByThread.find(hdr.ThreadId);
            if (eventIter == mPendingPresentByThread.end()) {
                return;
            }

            if (eventIter->second->PresentMode == PresentMode::Fullscreen_Blit) {
                eventIter->second->PresentMode = PresentMode::Windowed_Blit;
            }
            uint64_t TokenPtr = eventInfo.GetPtr(L"Token");
            mPresentHistoryByTokenPtr[TokenPtr] = eventIter->second;
            break;
        }
        case DxgKrnl_SubmitPresentHistory:
        {
            auto eventIter = FindOrCreatePresent(pEventRecord);

            if (eventIter->second->PresentMode == PresentMode::Fullscreen_Blit) {
                auto TokenData = eventInfo.GetData<uint64_t>(L"TokenData");
                mLegacyBlitPresentMap[TokenData] = eventIter->second;

                eventIter->second->ReadyTime = EventTime;
                eventIter->second->PresentMode = PresentMode::Legacy_Windowed_Blit;
            } else if (eventIter->second->PresentMode == PresentMode::Unknown) {
                enum class TokenModel {
                    Composition = 7,
                };

                auto Model = eventInfo.GetData<TokenModel>(L"Model");
                if (Model == TokenModel::Composition) {
                    eventIter->second->PresentMode = PresentMode::Composition_Buffer;
                    uint64_t TokenPtr = eventInfo.GetPtr(L"Token");
                    mPresentHistoryByTokenPtr[TokenPtr] = eventIter->second;
                }
            }

            if (eventIter->second->Runtime == Runtime::Other ||
                eventIter->second->PresentMode == PresentMode::Composition_Buffer)
            {
                // We're not expecting any other events from this thread (no DxgKrnl Present or EndPresent runtime event)
                mPendingPresentByThread.erase(eventIter);
            }
            break;
        }
        case DxgKrnl_PropagatePresentHistory:
        {
            uint64_t TokenPtr = eventInfo.GetPtr(L"Token");
            auto eventIter = mPresentHistoryByTokenPtr.find(TokenPtr);
            if (eventIter == mPresentHistoryByTokenPtr.end()) {
                return;
            }
            eventIter->second->ReadyTime = EventTime;

            if (eventIter->second->FinalState == PresentResult::Discarded &&
                eventIter->second->PresentMode == PresentMode::Windowed_Blit) {
                // This won't make it to screen, go ahead and complete it now
                CompletePresent(eventIter->second);
            } else if (eventIter->second->PresentMode == PresentMode::Composition_Buffer) {
                mWaitingForDWM.emplace_back(eventIter->second);
            }

            mPresentHistoryByTokenPtr.erase(eventIter);
            break;
        }
        case DxgKrnl_Blit:
        {
            auto eventIter = FindOrCreatePresent(pEventRecord);

            eventIter->second->PresentMode = PresentMode::Fullscreen_Blit;
            break;
        }
    }
}

void DxgiConsumer::OnWin32kEvent(PEVENT_RECORD pEventRecord)
{
    enum {
        Win32K_TokenCompositionSurfaceObject = 201,
        Win32K_TokenStateChanged = 301,
    };

    auto& hdr = pEventRecord->EventHeader;
    switch (hdr.EventDescriptor.Id) 
    {
    case Win32K_TokenCompositionSurfaceObject:
    case Win32K_TokenStateChanged:
        break;
    default:
        return;
    }

    uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;
    TraceEventInfo eventInfo(pEventRecord);

    switch (hdr.EventDescriptor.Id)
    {
        case Win32K_TokenCompositionSurfaceObject:
        {
            auto eventIter = FindOrCreatePresent(pEventRecord);
            
            eventIter->second->PresentMode = PresentMode::Composed_Flip;

            PresentHistoryTokenKey key(eventInfo.GetPtr(L"pCompositionSurfaceObject"),
                                       eventInfo.GetData<uint64_t>(L"PresentCount"),
                                       eventInfo.GetData<uint32_t>(L"SwapChainIndex"));
            mPresentHistoryTokens[key] = eventIter->second;
            break;
        }
        case Win32K_TokenStateChanged:
        {
            PresentHistoryTokenKey key(eventInfo.GetPtr(L"pCompositionSurfaceObject"),
                                       eventInfo.GetData<uint32_t>(L"PresentCount"),
                                       eventInfo.GetData<uint32_t>(L"SwapChainIndex"));
            auto eventIter = mPresentHistoryTokens.find(key);
            if (eventIter == mPresentHistoryTokens.end()) {
                return;
            }

            enum class TokenState {
                InFrame = 3,
                Confirmed = 4,
                Retired = 5,
                Discarded = 6,
            };
            
            auto &event = *eventIter->second;
            auto state = eventInfo.GetData<TokenState>(L"NewState");
            switch (state)
            {
                case TokenState::InFrame:
                {
                    if (event.Hwnd) {
                        auto hWndIter = mPresentByWindow.find(event.Hwnd);
                        if (hWndIter == mPresentByWindow.end()) {
                            mPresentByWindow.emplace(event.Hwnd, eventIter->second);
                        } else if (hWndIter->second != eventIter->second) {
                            hWndIter->second->FinalState = PresentResult::Discarded;
                            hWndIter->second = eventIter->second;
                        }
                    }

                    bool iFlip = eventInfo.GetData<BOOL>(L"IndependentFlip") != 0;
                    if (iFlip && event.PresentMode == PresentMode::Composed_Flip) {
                        event.PresentMode = PresentMode::IndependentFlip;
                    }

                    break;
                }
                case TokenState::Confirmed:
                {
                    if (event.FinalState == PresentResult::Unknown) {
                        if (event.PresentFlags & DXGI_PRESENT_DO_NOT_SEQUENCE) {
                            // DO_NOT_SEQUENCE presents may get marked as confirmed,
                            // if a frame was composed when this token was completed
                            event.FinalState = PresentResult::Discarded;
                        } else {
                            event.FinalState = PresentResult::Presented;
                        }
                    }
                    break;
                }
                case TokenState::Retired:
                {
                    event.ScreenTime = EventTime;
                    break;
                }
                case TokenState::Discarded:
                {
                    auto sharedPtr = eventIter->second;
                    mPresentHistoryTokens.erase(eventIter);

                    if (event.FinalState == PresentResult::Unknown) {
                        event.FinalState = PresentResult::Discarded;
                    }

                    CompletePresent(sharedPtr);

                    break;
                }
            }
            break;
        }
    }
}

void DxgiConsumer::OnDWMEvent(PEVENT_RECORD pEventRecord)
{
    enum {
        DWM_DwmUpdateWindow = 46,
        DWM_Schedule_Present_Start = 15,
        DWM_FlipChain_Pending = 69,
        DWM_FlipChain_Complete = 70,
        DWM_FlipChain_Dirty = 101,
    };

    auto& hdr = pEventRecord->EventHeader;
    switch (hdr.EventDescriptor.Id) 
    {
    case DWM_DwmUpdateWindow:
    case DWM_Schedule_Present_Start:
    case DWM_FlipChain_Pending:
    case DWM_FlipChain_Complete:
    case DWM_FlipChain_Dirty:
        break;
    default:
        return;
    }

    uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;

    switch (hdr.EventDescriptor.Id)
    {
        case DWM_DwmUpdateWindow:
        {
            TraceEventInfo eventInfo(pEventRecord);
            auto hWnd = (uint32_t)eventInfo.GetData<uint64_t>(L"hWnd");

            // Piggyback on the next DWM present
            mWindowsBeingComposed.insert(hWnd);
            break;
        }
        case DWM_Schedule_Present_Start:
        {
            DwmPresentThreadId = hdr.ThreadId;
            for (auto hWnd : mWindowsBeingComposed)
            {
                auto hWndIter = mPresentByWindow.find(hWnd);
                if (hWndIter != mPresentByWindow.end()) {
                    if (hWndIter->second->PresentMode != PresentMode::Windowed_Blit &&
                        hWndIter->second->PresentMode != PresentMode::Legacy_Windowed_Blit) {
                        continue;
                    }
                    mWaitingForDWM.emplace_back(hWndIter->second);
                    hWndIter->second->FinalState = PresentResult::Presented;
                    mPresentByWindow.erase(hWndIter);
                }
            }
            mWindowsBeingComposed.clear();
            break;
        }
        case DWM_FlipChain_Pending:
        case DWM_FlipChain_Complete:
        case DWM_FlipChain_Dirty:
        {
            TraceEventInfo eventInfo(pEventRecord);
            uint32_t flipChainId = (uint32_t)eventInfo.GetData<uint64_t>(L"ulFlipChain");
            uint32_t serialNumber = (uint32_t)eventInfo.GetData<uint64_t>(L"ulSerialNumber");
            uint64_t token = ((uint64_t)flipChainId << 32ull) | serialNumber;
            auto flipIter = mLegacyBlitPresentMap.find(token);
            if (flipIter == mLegacyBlitPresentMap.end()) {
                return;
            }

            auto hWnd = (uint32_t)eventInfo.GetData<uint64_t>(L"hwnd");
            auto hWndIter = mPresentByWindow.find(hWnd);
            if (hWndIter == mPresentByWindow.end()) {
                mPresentByWindow.emplace(hWnd, flipIter->second);
            } else if (hWndIter->second != flipIter->second) {
                auto eventPtr = hWndIter->second;
                hWndIter->second = flipIter->second;

                eventPtr->FinalState = PresentResult::Discarded;
                CompletePresent(eventPtr);
            }

            mLegacyBlitPresentMap.erase(flipIter);
            mWindowsBeingComposed.insert(hWnd);
            break;
        }
    }
}

void DxgiConsumer::OnD3D9Event(PEVENT_RECORD pEventRecord)
{
    enum {
        D3D9PresentStart = 1,
        D3D9PresentStop,
    };

    auto& hdr = pEventRecord->EventHeader;
    uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;
    switch (hdr.EventDescriptor.Id)
    {
        case D3D9PresentStart:
        {
            PresentEvent event;
            event.ProcessId = hdr.ProcessId;
            event.QpcTime = EventTime;
        
            TraceEventInfo eventInfo(pEventRecord);
            event.SwapChainAddress = eventInfo.GetPtr(L"pSwapchain");
            uint32_t D3D9Flags = eventInfo.GetData<uint32_t>(L"Flags");
            event.PresentFlags =
                ((D3D9Flags & D3DPRESENT_DONOTFLIP) ? DXGI_PRESENT_DO_NOT_SEQUENCE : 0) |
                ((D3D9Flags & D3DPRESENT_DONOTWAIT) ? DXGI_PRESENT_DO_NOT_WAIT : 0) |
                ((D3D9Flags & D3DPRESENT_FLIPRESTART) ? DXGI_PRESENT_RESTART : 0);
            event.Runtime = Runtime::D3D9;
        
            mPendingPresentByThread[hdr.ThreadId] = std::make_shared<PresentEvent>(event);
            break;
        }
        case D3D9PresentStop:
        {
            auto eventIter = mPendingPresentByThread.find(hdr.ThreadId);
            if (eventIter == mPendingPresentByThread.end()) {
                return;
            }
            auto &event = *eventIter->second;

            uint64_t EndTime = EventTime;
            assert(event.QpcTime < EndTime);
            event.TimeTaken = EndTime - event.QpcTime;

            if (event.PresentMode == PresentMode::Unknown) {
                auto& eventDeque = mPendingPresentsByProcess[hdr.ProcessId];
                eventDeque.push_back(eventIter->second);

#if _DEBUG
                auto& container = mPendingPresentsByProcess[hdr.ProcessId];
                for (UINT i = 0; i < container.size() - 1; ++i)
                {
                    assert(container[i]->QpcTime <= container[i + 1]->QpcTime);
                }
#endif

                const uint32_t cMaxPendingPresents = 20;
                if (eventDeque.size() > cMaxPendingPresents)
                {
                    CompletePresent(eventDeque.front());
                    eventDeque.pop_front();
                }
            }
            break;
        }
    }
}

void DxgiConsumer::OnEventRecord(PEVENT_RECORD pEventRecord)
{
    auto& hdr = pEventRecord->EventHeader;

    if (hdr.ProviderId == DXGI_PROVIDER_GUID)
    {
        OnDXGIEvent(pEventRecord);
    }
    else if (hdr.ProviderId == DXGKRNL_PROVIDER_GUID)
    {
        OnDXGKrnlEvent(pEventRecord);
    }
    else if (hdr.ProviderId == WIN32K_PROVIDER_GUID)
    {
        OnWin32kEvent(pEventRecord);
    }
    else if (hdr.ProviderId == DWM_PROVIDER_GUID)
    {
        OnDWMEvent(pEventRecord);
    }
    else if (hdr.ProviderId == D3D9_PROVIDER_GUID)
    {
        OnD3D9Event(pEventRecord);
    }
}

static void EtwProcessingThread(TraceSession *session)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    session->Process();
}

static GUID GuidFromString(const wchar_t *guidString)
{
    GUID g;
    auto hr = CLSIDFromString(guidString, &g);
    assert(SUCCEEDED(hr));
    return g;
}

void PresentMonEtw(PresentMonArgs args)
{
    TraceSession session(L"PresentMon");
    DxgiConsumer consumer;

    if (!session.Start()) {
        if (session.Status() == ERROR_ALREADY_EXISTS) {
            if (!session.Stop() || !session.Start()) {
                printf("ETW session error. Quitting.\n");
                exit(0);
            }
        }
    }

    session.EnableProvider(DXGI_PROVIDER_GUID, TRACE_LEVEL_INFORMATION);
    session.EnableProvider(DXGKRNL_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 1);
    session.EnableProvider(WIN32K_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 0x1000);
    session.EnableProvider(DWM_PROVIDER_GUID, TRACE_LEVEL_RESERVED6);
    session.EnableProvider(D3D9_PROVIDER_GUID, TRACE_LEVEL_INFORMATION);

    session.OpenTrace(&consumer);
    uint32_t eventsLost, buffersLost;

    {
        // Launch the ETW producer thread
        std::thread etwThread(EtwProcessingThread, &session);

        // Consume / Update based on the ETW output
        {
            PresentMonData data;

            PresentMon_Init(args, data);

            while (!g_Quit)
            {
                std::vector<std::shared_ptr<PresentEvent>> presents;
                consumer.DequeuePresents(presents);

                PresentMon_Update(data, presents, session.PerfFreq());
                if (session.AnythingLost(eventsLost, buffersLost)) {
                    printf("Lost %u events, %u buffers.", eventsLost, buffersLost);
                }

                Sleep(100);
            }

            PresentMon_Shutdown(data);
        }

        etwThread.join();
    }

    session.CloseTrace();
    session.DisableProvider(DXGI_PROVIDER_GUID);
    session.DisableProvider(DXGKRNL_PROVIDER_GUID);
    session.DisableProvider(WIN32K_PROVIDER_GUID);
    session.DisableProvider(DWM_PROVIDER_GUID);
    session.DisableProvider(D3D9_PROVIDER_GUID);
    session.Stop();
}

