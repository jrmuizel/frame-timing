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

#include <cstddef>
#include <stdio.h>

#include "Events.hpp"
#include "TraceSession.hpp"

namespace {

VOID WINAPI EventRecordCallback(EVENT_RECORD* pEventRecord)
{
    auto session = (TraceSession*) pEventRecord->UserContext;
    auto const& hdr = pEventRecord->EventHeader;

    if (session->startTime_ == 0) {
        session->startTime_ = hdr.TimeStamp.QuadPart;
    }

         if (hdr.ProviderId == NT_PROCESS_EVENT_GUID) HandleNTProcessEvent(pEventRecord, session->pmData_); 
    else if (hdr.ProviderId == DXGI_PROVIDER_GUID   ) session->pmTraceConsumer_->OnDXGIEvent(pEventRecord);
    else if (hdr.ProviderId == D3D9_PROVIDER_GUID   ) session->pmTraceConsumer_->OnD3D9Event(pEventRecord);
    else if (hdr.ProviderId == DXGKRNL_PROVIDER_GUID) session->pmTraceConsumer_->OnDXGKrnlEvent(pEventRecord);
    else if (hdr.ProviderId == WIN32K_PROVIDER_GUID ) session->pmTraceConsumer_->OnWin32kEvent(pEventRecord);
    else if (hdr.ProviderId == DWM_PROVIDER_GUID    ) session->pmTraceConsumer_->OnDWMEvent(pEventRecord);
}

ULONG WINAPI BufferCallback(EVENT_TRACE_LOGFILEA* pLogFile)
{
    (void) pLogFile;

    if (EtwThreadsShouldQuit()) {
        return FALSE; // break out of ProcessTrace()
    }

    return TRUE; // continue processing events
}

}

bool TraceSession::Initialize(bool simpleMode, char const* inputEtlPath)
{
    // Initialize state
    EVENT_TRACE_LOGFILEA inputEtl = {};
    inputEtl.BufferCallback = BufferCallback;
    inputEtl.EventRecordCallback = EventRecordCallback;
    inputEtl.Context = this;

    // Real-time collection
    if (inputEtlPath == nullptr) {
        memset(&properties_, 0, sizeof(properties_));

        properties_.Wnode.BufferSize = (ULONG) offsetof(TraceSession, sessionHandle_);
      //properties_.Wnode.Guid                 // ETW will create Guid
        properties_.Wnode.ClientContext = 1;   // Clock resolution to use when logging the timestamp for each event
                                               // 1 == query performance counter
        properties_.Wnode.Flags = 0;
      //properties_.BufferSize = 0;
        properties_.MinimumBuffers = 200;
      //properties_.MaximumBuffers = 0;
      //properties_.MaximumFileSize = 0;
        properties_.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
      //properties_.FlushTimer = 0;
      //properties_.EnableFlags = 0;
        properties_.LogFileNameOffset = 0;
        properties_.LoggerNameOffset = offsetof(TraceSession, loggerName_);

        // Start trace session
        auto status = StartTraceW(&sessionHandle_, L"PresentMon", &properties_);
        if (status == ERROR_ALREADY_EXISTS) {
#ifdef _DEBUG
            fprintf(stderr, "warning: trying to start trace session that already exists.\n");
#endif
            status = ControlTraceW((TRACEHANDLE) 0, L"PresentMon", &properties_, EVENT_TRACE_CONTROL_STOP);
            if (status == ERROR_SUCCESS) {
                status = StartTraceW(&sessionHandle_, L"PresentMon", &properties_);
            }
        }

        if (status != ERROR_SUCCESS) {
            fprintf(stderr, "error: failed to start trace session (error=%u).\n", status);
            return false;
        }

        // Enable desired providers
#define ENABLE_PROVIDER(_Guid, _Level, _MatchAny, _MatchAll) \
        status = EnableTraceEx2(sessionHandle_, &_Guid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, _Level, _MatchAny, _MatchAll, 0, nullptr); \
        if (status != ERROR_SUCCESS) { \
            fprintf(stderr, "error: failed to enable " #_Guid ".\n"); \
            Finalize(); \
            return false; \
        }

        ENABLE_PROVIDER(DXGI_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 0, 0);
        ENABLE_PROVIDER(D3D9_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 0, 0);
        if (!simpleMode) {
            ENABLE_PROVIDER(DXGKRNL_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 1, 0);
            ENABLE_PROVIDER(WIN32K_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 0x1000, 0);
            ENABLE_PROVIDER(DWM_PROVIDER_GUID, TRACE_LEVEL_VERBOSE, 0, 0);
        }

        inputEtl.LoggerName = "PresentMon";
        inputEtl.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;

    // ETL file collection
    } else {
        inputEtl.LogFileName = (LPSTR) inputEtlPath;
        inputEtl.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    }

    traceHandle_ = OpenTraceA(&inputEtl);
    if (traceHandle_ == INVALID_PROCESSTRACE_HANDLE) {
        fprintf(stderr, "error: failed to open trace.\n");
        Finalize();
        return false;
    }

    frequency_ = inputEtl.LogfileHeader.PerfFreq.QuadPart;
    eventsLostCount_ = 0;
    buffersLostCount_ = 0;

    return true;
}

void TraceSession::Finalize()
{
    ULONG status = ERROR_SUCCESS;

    if (traceHandle_ != INVALID_PROCESSTRACE_HANDLE) {
        status = CloseTrace(traceHandle_);
        traceHandle_ = INVALID_PROCESSTRACE_HANDLE;
    }

    if (sessionHandle_ != 0) {
        status = ControlTraceW(sessionHandle_, nullptr, &properties_, EVENT_TRACE_CONTROL_STOP);

#define DISABLE_PROVIDER(_Guid) \
        status = EnableTraceEx2(sessionHandle_, &_Guid, EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr)

        DISABLE_PROVIDER(DXGI_PROVIDER_GUID);
        DISABLE_PROVIDER(D3D9_PROVIDER_GUID);
        DISABLE_PROVIDER(DXGKRNL_PROVIDER_GUID);
        DISABLE_PROVIDER(WIN32K_PROVIDER_GUID);
        DISABLE_PROVIDER(DWM_PROVIDER_GUID);

        sessionHandle_ = 0;
    }
}

bool TraceSession::CheckLostReports(uint32_t* eventsLost, uint32_t* buffersLost)
{
    if (sessionHandle_ == 0) {
        *eventsLost = 0;
        *buffersLost = 0;
        return false;
    }

    auto status = ControlTraceW(sessionHandle_, nullptr, &properties_, EVENT_TRACE_CONTROL_QUERY);
    if (status == ERROR_MORE_DATA) {
        *eventsLost = 0;
        *buffersLost = 0;
        return true;
    }

    if (status != ERROR_SUCCESS) {
        fprintf(stderr, "error: failed to query trace status (%u).\n", status);
        *eventsLost = 0;
        *buffersLost = 0;
        return false;
    }

    *eventsLost = properties_.EventsLost - eventsLostCount_;
    *buffersLost = properties_.RealTimeBuffersLost - buffersLostCount_;
    eventsLostCount_ = properties_.EventsLost;
    buffersLostCount_ = properties_.RealTimeBuffersLost;
    return *eventsLost + *buffersLost > 0;
}

