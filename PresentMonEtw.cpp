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

#include "CommonIncludes.hpp"
#include "PresentMon.hpp"
#include "TraceSession.hpp"
#include <thread>

struct ProcessTraceConsumer : public ITraceConsumer
{
    std::mutex mProcessMutex;
    std::map<uint32_t, ProcessInfo> mNewProcessesFromETW;
    std::vector<uint32_t> mDeadProcessIds;

    void GetProcessEvents(decltype(mNewProcessesFromETW)& outNewProcesses, decltype(mDeadProcessIds)& outDeadProcesses)
    {
        auto lock = scoped_lock(mProcessMutex);
        outNewProcesses.swap(mNewProcessesFromETW);
        outDeadProcesses.swap(mDeadProcessIds);
    }

    virtual void OnEventRecord(_In_ PEVENT_RECORD pEventRecord);
    virtual bool ContinueProcessing() { return !g_Quit; }

private:
    void OnNTProcessEvent(PEVENT_RECORD pEventRecord);
};

void ProcessTraceConsumer::OnEventRecord(PEVENT_RECORD pEventRecord)
{
    if (mTraceStartTime == 0)
    {
        mTraceStartTime = pEventRecord->EventHeader.TimeStamp.QuadPart;
    }

    auto& hdr = pEventRecord->EventHeader;

    if (hdr.ProviderId == NT_PROCESS_EVENT_GUID)
    {
        OnNTProcessEvent(pEventRecord);
    }
}

void ProcessTraceConsumer::OnNTProcessEvent(PEVENT_RECORD pEventRecord)
{
    TraceEventInfo eventInfo(pEventRecord);
    auto pid = eventInfo.GetData<uint32_t>(L"ProcessId");
    auto lock = scoped_lock(mProcessMutex);
    switch (pEventRecord->EventHeader.EventDescriptor.Opcode)
    {
    case EVENT_TRACE_TYPE_START:
    case EVENT_TRACE_TYPE_DC_START:
    {
        ProcessInfo process;
        auto nameSize = eventInfo.GetDataSize(L"ImageFileName");
        process.mModuleName.resize(nameSize, '\0');
        eventInfo.GetData(L"ImageFileName", (byte*)process.mModuleName.data(), nameSize);

        mNewProcessesFromETW.insert_or_assign(pid, std::move(process));
        break;
    }
    case EVENT_TRACE_TYPE_END:
    case EVENT_TRACE_TYPE_DC_END:
    {
        mDeadProcessIds.emplace_back(pid);
        break;
    }
    }
}

static bool g_FileComplete = false;
static void EtwProcessingThread(TraceSession *session)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    session->Process();

    // Guarantees that the PM thread does one more loop to pick up any last events before setting g_Quit
    g_FileComplete = true;
}

void PresentMonEtw(const PresentMonArgs& args)
{
    Sleep(args.mDelay * 1000);
    if (g_Quit) {
        return;
    }

    g_FileComplete = false;
    std::wstring fileName(args.mEtlFileName, args.mEtlFileName +
        (args.mEtlFileName ? strlen(args.mEtlFileName) : 0));
    std::wstring sessionName(L"PresentMon");
    TraceSession session(sessionName.c_str(), !fileName.empty() ? fileName.c_str() : nullptr);
    PMTraceConsumer pmConsumer(args.mSimple);
    ProcessTraceConsumer procConsumer = {};
    MultiTraceConsumer mtConsumer = {};

    mtConsumer.AddTraceConsumer(&pmConsumer);

    if (!args.mEtlFileName && !session.Start()) {
        if (session.Status() == ERROR_ALREADY_EXISTS) {
            if (!session.Stop() || !session.Start()) {
                printf("ETW session error. Quitting.\n");
                exit(0);
            }
        }
    }

    session.EnableProvider(DXGI_PROVIDER_GUID, TRACE_LEVEL_INFORMATION);
    session.EnableProvider(D3D9_PROVIDER_GUID, TRACE_LEVEL_INFORMATION);
    if (!args.mSimple)
    {
        session.EnableProvider(DXGKRNL_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 1);
        session.EnableProvider(WIN32K_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 0x1000);
        session.EnableProvider(DWM_PROVIDER_GUID, TRACE_LEVEL_VERBOSE);
    }

    session.OpenTrace(&mtConsumer);
    uint32_t eventsLost, buffersLost;

    {
        // Launch the ETW producer thread
        std::thread etwThread(EtwProcessingThread, &session);

        // Consume / Update based on the ETW output
        {
            PresentMonData data;

            PresentMon_Init(args, data);
            uint64_t start_time = GetTickCount64();

            std::vector<std::shared_ptr<PresentEvent>> presents;
            std::map<uint32_t, ProcessInfo> newProcesses;
            std::vector<uint32_t> deadProcesses;

            bool log_corrupted = false;

            while (!g_Quit)
            {
                presents.clear();
                newProcesses.clear();
                deadProcesses.clear();

                // If we are reading events from ETL file set start time to match time stamp of first event
                if (data.mArgs->mEtlFileName && data.mStartupQpcTime == 0)
                {
                    data.mStartupQpcTime = pmConsumer.mTraceStartTime;
                }

                if (args.mEtlFileName) {
                    pmConsumer.GetProcessEvents(newProcesses, deadProcesses);
                    PresentMon_UpdateNewProcesses(data, newProcesses);
                }

                pmConsumer.DequeuePresents(presents);
                if (args.mScrollLockToggle && (GetKeyState(VK_SCROLL) & 1) == 0) {
                    presents.clear();
                }

                PresentMon_Update(data, presents, session.PerfFreq());
                if (session.AnythingLost(eventsLost, buffersLost)) {
                    printf("Lost %u events, %u buffers.", eventsLost, buffersLost);
                    // FIXME: How do we set a threshold here?
                    if (eventsLost > 100) {
                        log_corrupted = true;
                        g_FileComplete = true;
                    }
                }

                if (args.mEtlFileName) {
                    PresentMon_UpdateDeadProcesses(data, deadProcesses);
                }

                if (g_FileComplete || (args.mTimer > 0 && GetTickCount64() - start_time > args.mTimer * 1000)) {
                    g_Quit = true;
                }

                Sleep(100);
            }

            PresentMon_Shutdown(data, log_corrupted);
        }

        etwThread.join();
    }

    session.CloseTrace();
    session.DisableProvider(DXGI_PROVIDER_GUID);
    session.DisableProvider(D3D9_PROVIDER_GUID);
    if (!args.mSimple)
    {
        session.DisableProvider(DXGKRNL_PROVIDER_GUID);
        session.DisableProvider(WIN32K_PROVIDER_GUID);
        session.DisableProvider(DWM_PROVIDER_GUID);
    }
    session.Stop();
}

