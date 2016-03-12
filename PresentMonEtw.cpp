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

static auto DXGI_PROVIDER_GUID = L"{CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}";

extern bool g_Quit;

struct DxgiConsumer : ITraceConsumer
{
    CRITICAL_SECTION mMutex;
    std::vector<PresentEvent> mPresents;

    std::map<uint64_t, PresentEvent> mPendingPresents;

    bool DequeuePresents(std::vector<PresentEvent>& outPresents)
    {
        if (mPresents.size())
        {
            EnterCriticalSection(&mMutex);
            outPresents.swap(mPresents);
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
};

void DxgiConsumer::OnEventRecord(PEVENT_RECORD pEventRecord)
{
    enum {
        Present_Start = 42,
        Present_Stop = 43,
        IDXGISwapChain_Present_Start = 178,
        IDXGISwapChain_Present_Stop // useless with multiple chains because it doesn't contain the chain ptr.
    };

    auto hdr = pEventRecord->EventHeader;

    uint64_t procAndThreadId = (static_cast<uint64_t>(hdr.ProcessId) << 32) | hdr.ThreadId;
    if (hdr.EventDescriptor.Id == IDXGISwapChain_Present_Start)
    {
        PresentEvent event = { 0 };
        event.ProcessId = hdr.ProcessId;
        event.QpcTime = *(uint64_t*)&pEventRecord->EventHeader.TimeStamp;

        if (pEventRecord->UserDataLength >= 16 &&
            !(pEventRecord->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER))
        {
            struct IDXGISwapChain_Present_Start_Data_64 {
                uint64_t SwapChainAddress;
                uint32_t SyncInterval;
                uint32_t Flags;
            };
            auto data = (IDXGISwapChain_Present_Start_Data_64*)pEventRecord->UserData;
            event.SwapChainAddress = data->SwapChainAddress;
            event.SyncInterval = data->SyncInterval;
            event.PresentFlags = data->Flags;
        }
        else if(pEventRecord->UserDataLength >= 12 &&
            (pEventRecord->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER))
        {
            struct IDXGISwapChain_Present_Start_Data_32 {
                uint32_t SwapChainAddress;
                uint32_t SyncInterval;
                uint32_t Flags;
            };
            auto data = (IDXGISwapChain_Present_Start_Data_32*)pEventRecord->UserData;
            event.SwapChainAddress = data->SwapChainAddress;
            event.SyncInterval = data->SyncInterval;
            event.PresentFlags = data->Flags;
        }

        
        mPendingPresents[procAndThreadId] = event;
    }
    else if (hdr.EventDescriptor.Id == IDXGISwapChain_Present_Stop)
    {
        auto eventIter = mPendingPresents.find(procAndThreadId);
        if (eventIter == mPendingPresents.end())
        {
            return;
        }

        auto event = eventIter->second;
        mPendingPresents.erase(eventIter);

        uint64_t EndTime = *(uint64_t*)&pEventRecord->EventHeader.TimeStamp;
        event.TimeTaken = EndTime - event.QpcTime;

        EnterCriticalSection(&mMutex);
        mPresents.push_back(event);
        LeaveCriticalSection(&mMutex);
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
    GUID dxgiProvider = GuidFromString(DXGI_PROVIDER_GUID);

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

    session.EnableProvider(dxgiProvider, TRACE_LEVEL_RESERVED6);

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
                std::vector<PresentEvent> presents;
                consumer.DequeuePresents(presents);

                PresentMon_Update(data, presents, session.PerfFreq());

                Sleep(100);
            }

            PresentMon_Shutdown(data);
        }

        etwThread.join();
    }

    session.CloseTrace();
    session.DisableProvider(dxgiProvider);
    session.Stop();
}

