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

struct __declspec(uuid("{CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}")) DXGI_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{802ec45a-1e99-4b83-9920-87c98277ba9d}")) DXGKRNL_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{8c416c79-d49b-4f01-a467-e56d3aa8234c}")) WIN32K_PROVIDER_GUID_HOLDER;
static const auto DXGI_PROVIDER_GUID = __uuidof(DXGI_PROVIDER_GUID_HOLDER);
static const auto DXGKRNL_PROVIDER_GUID = __uuidof(DXGKRNL_PROVIDER_GUID_HOLDER);
static const auto WIN32K_PROVIDER_GUID = __uuidof(WIN32K_PROVIDER_GUID_HOLDER);

extern bool g_Quit;

struct DxgiConsumer : ITraceConsumer
{
    CRITICAL_SECTION mMutex;
    std::vector<std::shared_ptr<PresentEvent>> mCompletedPresents;

    // Presents in the process of being submitted
    std::map<uint32_t, std::shared_ptr<PresentEvent>> mPendingPresents;

    // Fullscreen presents map from present sequence id
    std::map<uint32_t, std::shared_ptr<PresentEvent>> mFullscreenPresents;

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
    if (hdr.EventDescriptor.Id == IDXGISwapChain_Present_Start) {
        PresentEvent event;
        event.ProcessId = hdr.ProcessId;
        event.QpcTime = *(uint64_t*)&pEventRecord->EventHeader.TimeStamp;
        
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
        
        mPendingPresents[hdr.ThreadId] = std::make_shared<PresentEvent>(event);
    }
    else if (hdr.EventDescriptor.Id == IDXGISwapChain_Present_Stop)
    {
        auto eventIter = mPendingPresents.find(hdr.ThreadId);
        if (eventIter == mPendingPresents.end()) {
            OutputDebugString(L"Didn't find outstanding present\n");
            return;
        }

        auto eventShared = eventIter->second;
        auto &event = *eventShared;
        mPendingPresents.erase(eventIter);

        uint64_t EndTime = *(uint64_t*)&pEventRecord->EventHeader.TimeStamp;
        event.TimeTaken = EndTime - event.QpcTime;

        if (event.PresentMode == PresentMode::Unknown) {
            CompletePresent(eventShared);
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
    switch (hdr.EventDescriptor.Id)
    {
        case DxgKrnl_Flip:
        {
            auto eventIter = mPendingPresents.find(hdr.ThreadId);
            if (eventIter == mPendingPresents.end()) {
                return;
            }

            OutputDebugString(L"Found outstanding present\n");
            auto &event = *eventIter->second;
            event.PresentMode = PresentMode::Fullscreen;
            break;
        }
        case DxgKrnl_QueueSubmit:
        {
            auto eventIter = mPendingPresents.find(hdr.ThreadId);
            if (eventIter == mPendingPresents.end()) {
                return;
            }

            struct DxgKrnl_QueueSubmit_Data {
                ptr_t Context;
                uint32_t Type;
                uint32_t SubmitSequence;
            };
            if (pEventRecord->UserDataLength >= sizeof(DxgKrnl_QueueSubmit_Data)) {
                auto data = (DxgKrnl_QueueSubmit_Data*)pEventRecord->UserData;
                mFullscreenPresents.emplace(data->SubmitSequence, eventIter->second);
            }
            break;
        }
        case DxgKrnl_MMIOFlip:
        {
            struct DxgKrnl_MMIOFlip_Data {
                ptr_t Adapter;
                uint32_t VidPnSourceId;
                uint32_t FlipSubmitSequence;
            };
            if (pEventRecord->UserDataLength < sizeof(DxgKrnl_MMIOFlip_Data)) {
                return;
            }

            auto data = (DxgKrnl_MMIOFlip_Data*)pEventRecord->UserData;

            auto eventIter = mFullscreenPresents.find(data->FlipSubmitSequence);
            if (eventIter == mFullscreenPresents.end()) {
                return;
            }

            eventIter->second->ReadyTime = *(uint64_t*)&pEventRecord->EventHeader.TimeStamp;
            break;
        }
        case DxgKrnl_VSyncDPC:
        {
            struct DxgKrnl_VSyncDPC_Data {
                ptr_t Adapter;
                uint32_t VidPnTargetId;
                uint64_t ScannedPhysical;
                uint32_t VidPnSourceId;
                uint32_t FrameNumber;
                int64_t FrameQpcTime;
                ptr_t FlipDevice;
                uint32_t FlipType;
                uint64_t FlipFenceId;
            };

            if (pEventRecord->UserDataLength < sizeof(DxgKrnl_VSyncDPC_Data)) {
                return;
            }

            auto data = (DxgKrnl_VSyncDPC_Data*)pEventRecord->UserData;

            uint32_t FlipSubmitSequence = (uint32_t)(data->FlipFenceId >> 32u);
            auto eventIter = mFullscreenPresents.find(FlipSubmitSequence);
            if (eventIter == mFullscreenPresents.end()) {
                return;
            }

            eventIter->second->ScreenTime = *(uint64_t*)&pEventRecord->EventHeader.TimeStamp;
            eventIter->second->FinalState = PresentResult::Presented;
            CompletePresent(eventIter->second);

            mFullscreenPresents.erase(eventIter);
        }
    }
}

template <typename ptr_t>
void DxgiConsumer::OnWin32kEvent(PEVENT_RECORD pEventRecord)
{
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

