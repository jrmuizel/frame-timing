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

struct __declspec(uuid("{CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}")) DXGI_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{802ec45a-1e99-4b83-9920-87c98277ba9d}")) DXGKRNL_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{8c416c79-d49b-4f01-a467-e56d3aa8234c}")) WIN32K_PROVIDER_GUID_HOLDER;
static const auto DXGI_PROVIDER_GUID = __uuidof(DXGI_PROVIDER_GUID_HOLDER);
static const auto DXGKRNL_PROVIDER_GUID = __uuidof(DXGKRNL_PROVIDER_GUID_HOLDER);
static const auto WIN32K_PROVIDER_GUID = __uuidof(WIN32K_PROVIDER_GUID_HOLDER);

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

private:
    TRACE_EVENT_INFO* pInfo;
    EVENT_RECORD* pEvent;
};

struct DxgiConsumer : ITraceConsumer
{
    CRITICAL_SECTION mMutex;
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
    void CompletePresent(std::shared_ptr<PresentEvent> const& p)
    {
        EnterCriticalSection(&mMutex);
        mCompletedPresents.push_back(p);
        LeaveCriticalSection(&mMutex);
    }

    template <typename ptr_t>
    void OnDXGIEvent(_In_ PEVENT_RECORD pEventRecord);
    template <typename ptr_t>
    void OnDXGKrnlEvent(_In_ PEVENT_RECORD pEventRecord);
    template <typename ptr_t>
    void OnWin32kEvent(_In_ PEVENT_RECORD pEventRecord);

    template <typename ptr_t>
    void OnEventRecordBitnessAware(_In_ PEVENT_RECORD pEventRecord);
};

template <typename ptr_t>
void DxgiConsumer::OnDXGIEvent(PEVENT_RECORD pEventRecord)
{
    enum {
        IDXGISwapChain_Present_Start = 178,
        IDXGISwapChain_Present_Stop
    };

    auto& hdr = pEventRecord->EventHeader;
    uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;
    if (hdr.EventDescriptor.Id == IDXGISwapChain_Present_Start) {
        PresentEvent event;
        event.ProcessId = hdr.ProcessId;
        event.QpcTime = EventTime;
        
        struct IDXGISwapChain_Present_Start_Data {
            ptr_t SwapChainAddress;
            uint32_t SyncInterval;
            uint32_t Flags;
        };
        if (pEventRecord->UserDataLength >= sizeof(IDXGISwapChain_Present_Start_Data)) {
            auto data = (IDXGISwapChain_Present_Start_Data*)pEventRecord->UserData;
            event.SwapChainAddress = data->SwapChainAddress;
            event.SyncInterval = data->SyncInterval;
            event.PresentFlags = data->Flags;
        }
        
        mPendingPresentByThread[hdr.ThreadId] = std::make_shared<PresentEvent>(event);
    }
    else if (hdr.EventDescriptor.Id == IDXGISwapChain_Present_Stop)
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
    }
}

template <typename ptr_t>
void DxgiConsumer::OnDXGKrnlEvent(PEVENT_RECORD pEventRecord)
{
    enum {
        DxgKrnl_Flip = 168,
        DxgKrnl_MMIOFlip = 116,
        DxgKrnl_VSyncDPC = 17,
        DxgKrnl_QueueSubmit = 178,
        DxgKrnl_PresentHistoryDetailed = 215,
        DxgKrnl_PresentHistory = 171,
    };

    auto& hdr = pEventRecord->EventHeader;
    uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;

    auto FindPendingPresentByProcess = [EventTime](std::deque<std::shared_ptr<PresentEvent>> &eventDeque)
    {
        for (auto iter = eventDeque.begin(), end = eventDeque.end();
                iter != end;
                ++iter)
        {
            auto& event = *iter->get();
            if (event.QpcTime > EventTime) {
                return end;
            }

            return iter;
        }
        return eventDeque.end();
    };
    auto FindPendingPresent = [this, EventTime, &hdr, &FindPendingPresentByProcess](std::deque<std::shared_ptr<PresentEvent>> &eventDeque)
    {
        auto eventIter = mPendingPresentByThread.find(hdr.ThreadId);

        if (eventIter == mPendingPresentByThread.end()) {
            auto dequeIter = FindPendingPresentByProcess(eventDeque);
            if (dequeIter == eventDeque.end()) {
                return std::make_pair(std::shared_ptr<PresentEvent>(nullptr), eventDeque.end());
            }

            return std::make_pair(*dequeIter, dequeIter);
        }

        return std::make_pair(eventIter->second, eventDeque.end());
    };

    TraceEventInfo eventInfo(pEventRecord);
    switch (hdr.EventDescriptor.Id)
    {
        case DxgKrnl_Flip:
        {
            auto &eventDeque = mPendingPresentsByProcess[hdr.ProcessId];
            auto eventPair = FindPendingPresent(eventDeque);

            if (!eventPair.first) {
                return;
            }
            
            eventPair.first->PresentMode = PresentMode::Fullscreen;
            if (eventPair.second != eventDeque.end()) {
                eventDeque.erase(eventPair.second);
                mPendingPresentByThread[hdr.ThreadId] = eventPair.first;
            }
            break;
        }
        case DxgKrnl_QueueSubmit:
        {
            enum class DxgKrnl_QueueSubmit_Type {
                MMIOFlip = 3,
                Software = 7,
            };
            auto Type = eventInfo.GetData<DxgKrnl_QueueSubmit_Type>(L"PacketType");
            auto SubmitSequence = eventInfo.GetData<uint32_t>(L"SubmitSequence");

            if (Type == DxgKrnl_QueueSubmit_Type::MMIOFlip) {
                auto eventIter = mPendingPresentByThread.find(hdr.ThreadId);
                if (eventIter == mPendingPresentByThread.end()) {
                    return;
                }

                mFullscreenPresents.emplace(SubmitSequence, eventIter->second);

                if (eventIter->second->TimeTaken != 0) {
                    mPendingPresentByThread.erase(eventIter);
                }
            }
            break;
        }
        case DxgKrnl_MMIOFlip:
        {
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
                CompletePresent(eventIter->second);
                mFullscreenPresents.erase(eventIter);
            }

            break;
        }
        case DxgKrnl_VSyncDPC:
        {
            auto FlipFenceId = eventInfo.GetData<uint64_t>(L"FlipFenceId");

            uint32_t FlipSubmitSequence = (uint32_t)(FlipFenceId >> 32u);
            auto eventIter = mFullscreenPresents.find(FlipSubmitSequence);
            if (eventIter == mFullscreenPresents.end()) {
                return;
            }

            eventIter->second->ScreenTime = *(uint64_t*)&pEventRecord->EventHeader.TimeStamp;
            eventIter->second->FinalState = PresentResult::Presented;
            CompletePresent(eventIter->second);

            mFullscreenPresents.erase(eventIter);
            break;
        }
        case DxgKrnl_PresentHistoryDetailed:
        {
            auto eventIter = mPendingPresentByThread.find(hdr.ThreadId);
            if (eventIter == mPendingPresentByThread.end()) {
                return;
            }

            uint64_t TokenPtr = eventInfo.GetData<ptr_t>(L"Token");
            mPresentHistoryByTokenPtr[TokenPtr] = eventIter->second;

            if (eventIter->second->TimeTaken != 0) {
                mPendingPresentByThread.erase(eventIter);
            }
            break;
        }
        case DxgKrnl_PresentHistory:
        {
            uint64_t TokenPtr = eventInfo.GetData<ptr_t>(L"Token");
            auto eventIter = mPresentHistoryByTokenPtr.find(TokenPtr);
            if (eventIter == mPresentHistoryByTokenPtr.end()) {
                return;
            }

            eventIter->second->ReadyTime = EventTime;
            mPresentHistoryByTokenPtr.erase(eventIter);
            break;
        }
    }
}

template <typename ptr_t>
void DxgiConsumer::OnWin32kEvent(PEVENT_RECORD pEventRecord)
{
    enum {
        Win32K_TokenCompositionSurfaceObject = 201,
        Win32K_TokenStateChanged = 301,
    };

    auto& hdr = pEventRecord->EventHeader;
    uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;

    auto FindPendingPresentByProcess = [EventTime](std::deque<std::shared_ptr<PresentEvent>> &eventDeque)
    {
        for (auto iter = eventDeque.begin(), end = eventDeque.end();
                iter != end;
                ++iter)
        {
            auto& event = *iter->get();
            if (event.QpcTime > EventTime) {
                return end;
            }

            return iter;
        }
        return eventDeque.end();
    };
    auto FindPendingPresent = [this, EventTime, &hdr, &FindPendingPresentByProcess](std::deque<std::shared_ptr<PresentEvent>> &eventDeque)
    {
        auto eventIter = mPendingPresentByThread.find(hdr.ThreadId);

        if (eventIter == mPendingPresentByThread.end()) {
            auto dequeIter = FindPendingPresentByProcess(eventDeque);
            if (dequeIter == eventDeque.end()) {
                return std::make_pair(std::shared_ptr<PresentEvent>(nullptr), eventDeque.end());
            }

            return std::make_pair(*dequeIter, dequeIter);
        }
        
        return std::make_pair(eventIter->second, eventDeque.end());
    };

    TraceEventInfo eventInfo(pEventRecord);

    switch (hdr.EventDescriptor.Id)
    {
        case Win32K_TokenCompositionSurfaceObject:
        {
            auto &eventDeque = mPendingPresentsByProcess[hdr.ProcessId];
            auto eventPair = FindPendingPresent(eventDeque);

            if (!eventPair.first) {
                return;
            }
            
            eventPair.first->PresentMode = PresentMode::Composed_Flip;

            if (eventPair.second != eventDeque.end()) {
                eventDeque.erase(eventPair.second);
                mPendingPresentByThread[hdr.ThreadId] = eventPair.first;
            }

            PresentHistoryTokenKey key(eventInfo.GetData<ptr_t>(L"pCompositionSurfaceObject"),
                                       eventInfo.GetData<uint64_t>(L"PresentCount"),
                                       eventInfo.GetData<uint32_t>(L"SwapChainIndex"));
            mPresentHistoryTokens[key] = eventPair.first;
            break;
        }
        case Win32K_TokenStateChanged:
        {
            PresentHistoryTokenKey key(eventInfo.GetData<ptr_t>(L"pCompositionSurfaceObject"),
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
                case TokenState::Confirmed:
                {
                    event.FinalState = PresentResult::Presented;

                    bool iFlip = eventInfo.GetData<BOOL>(L"IndependentFlip") != 0;
                    if (iFlip) {
                        event.PresentMode = PresentMode::IndependentFlip;
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

                    if (event.PresentMode == PresentMode::IndependentFlip) {
                        event.ScreenTime = EventTime;
                    }

                    CompletePresent(sharedPtr);

                    break;
                }
            }
            break;
        }
    }
}

template <typename ptr_t>
void DxgiConsumer::OnEventRecordBitnessAware(_In_ PEVENT_RECORD pEventRecord)
{
    auto& hdr = pEventRecord->EventHeader;

    if (hdr.ProviderId == DXGI_PROVIDER_GUID)
    {
        OnDXGIEvent<ptr_t>(pEventRecord);
    }
    else if (hdr.ProviderId == DXGKRNL_PROVIDER_GUID)
    {
        OnDXGKrnlEvent<ptr_t>(pEventRecord);
    }
    else if (hdr.ProviderId == WIN32K_PROVIDER_GUID)
    {
        OnWin32kEvent<ptr_t>(pEventRecord);
    }
}


void DxgiConsumer::OnEventRecord(PEVENT_RECORD pEventRecord)
{
    auto& hdr = pEventRecord->EventHeader;
    if (hdr.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER)
    {
        OnEventRecordBitnessAware<uint32_t>(pEventRecord);
    }
    else if (hdr.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER)
    {
        OnEventRecordBitnessAware<uint64_t>(pEventRecord);
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
    session.EnableProvider(DXGKRNL_PROVIDER_GUID, TRACE_LEVEL_INFORMATION);
    session.EnableProvider(WIN32K_PROVIDER_GUID, TRACE_LEVEL_INFORMATION);

    session.OpenTrace(&consumer);

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
    session.Stop();
}

