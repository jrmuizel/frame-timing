/*
Copyright 2017-2019 Intel Corporation

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

#include "PresentMon.hpp"

struct TraceProperties : public EVENT_TRACE_PROPERTIES {
    wchar_t mSessionName[MAX_PATH];
};

static TRACEHANDLE gTraceHandle = INVALID_PROCESSTRACE_HANDLE; // invalid trace handles are INVALID_PROCESSTRACE_HANDLE
static TRACEHANDLE gSessionHandle = 0;                         // invalid session handles are 0
static PMTraceConsumer* gPMConsumer = nullptr;
static MRTraceConsumer* gMRConsumer = nullptr;
static uint64_t gQpcTraceStart = 0;
static uint64_t gQpcFrequency = 0;
static ULONG gContinueProcessingBuffers = TRUE;

static bool EnableProviders()
{
#define EnableProvider(Provider, Level, Any) do { \
    auto status = EnableTraceEx2(gSessionHandle, &Provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_##Level, Any, 0, 0, nullptr); \
    if (status != ERROR_SUCCESS) { \
        fprintf(stderr, "error: failed to enable trace provider \"%s\" (error=%lu).\n", #Provider, status); \
        return false; \
    } \
} while (0)

    auto const& args = GetCommandLineArgs();
    auto simple = args.mVerbosity == Verbosity::Simple;

    gPMConsumer = new PMTraceConsumer(simple);

                 EnableProvider(DXGI_PROVIDER_GUID,          INFORMATION, 0);
                 EnableProvider(D3D9_PROVIDER_GUID,          INFORMATION, 0);
    if (!simple) EnableProvider(DXGKRNL_PROVIDER_GUID,       INFORMATION, 1);
    if (!simple) EnableProvider(WIN32K_PROVIDER_GUID,        INFORMATION, 0x1000);
    if (!simple) EnableProvider(DWM_PROVIDER_GUID,           VERBOSE,     0);
    if (!simple) EnableProvider(Win7::DWM_PROVIDER_GUID,     VERBOSE,     0);
    if (!simple) EnableProvider(Win7::DXGKRNL_PROVIDER_GUID, INFORMATION, 1);

    if (args.mIncludeWindowsMixedReality) {
        gMRConsumer = new MRTraceConsumer(simple);

                     EnableProvider(DHD_PROVIDER_GUID,                VERBOSE, 0x1C00000);
        if (!simple) EnableProvider(SPECTRUMCONTINUOUS_PROVIDER_GUID, VERBOSE, 0x800000);
    }

#undef EnableProvider

    return true;
}

static void DisableProviders()
{
    ULONG status = 0;
    status = EnableTraceEx2(gSessionHandle, &DXGI_PROVIDER_GUID,               EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr);
    status = EnableTraceEx2(gSessionHandle, &D3D9_PROVIDER_GUID,               EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr);
    status = EnableTraceEx2(gSessionHandle, &DXGKRNL_PROVIDER_GUID,            EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr);
    status = EnableTraceEx2(gSessionHandle, &WIN32K_PROVIDER_GUID,             EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr);
    status = EnableTraceEx2(gSessionHandle, &DWM_PROVIDER_GUID,                EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr);
    status = EnableTraceEx2(gSessionHandle, &Win7::DWM_PROVIDER_GUID,          EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr);
    status = EnableTraceEx2(gSessionHandle, &Win7::DXGKRNL_PROVIDER_GUID,      EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr);
    status = EnableTraceEx2(gSessionHandle, &DHD_PROVIDER_GUID,                EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr);
    status = EnableTraceEx2(gSessionHandle, &SPECTRUMCONTINUOUS_PROVIDER_GUID, EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr);
}

static void CALLBACK SimpleEventRecordCallback(EVENT_RECORD* pEventRecord)
{
    auto const& hdr = pEventRecord->EventHeader;

    if (gQpcTraceStart == 0) {
        gQpcTraceStart = hdr.TimeStamp.QuadPart;
    }

         if (hdr.ProviderId == DXGI_PROVIDER_GUID)               HandleDXGIEvent                (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == D3D9_PROVIDER_GUID)               HandleD3D9Event                (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == NT_PROCESS_EVENT_GUID)            HandleNTProcessEvent           (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == EventMetadataGuid)                HandleMetadataEvent            (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == DHD_PROVIDER_GUID)                HandleDHDEvent                 (pEventRecord, gMRConsumer);
}

static void CALLBACK EventRecordCallback(EVENT_RECORD* pEventRecord)
{
    auto const& hdr = pEventRecord->EventHeader;

    if (gQpcTraceStart == 0) {
        gQpcTraceStart = hdr.TimeStamp.QuadPart;
    }

         if (hdr.ProviderId == DXGKRNL_PROVIDER_GUID)            HandleDXGKEvent                (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == WIN32K_PROVIDER_GUID)             HandleWin32kEvent              (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == DWM_PROVIDER_GUID)                HandleDWMEvent                 (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == DXGI_PROVIDER_GUID)               HandleDXGIEvent                (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == D3D9_PROVIDER_GUID)               HandleD3D9Event                (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == NT_PROCESS_EVENT_GUID)            HandleNTProcessEvent           (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == Win7::DWM_PROVIDER_GUID)          HandleDWMEvent                 (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == Win7::DXGKBLT_GUID)               Win7::HandleDxgkBlt            (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == Win7::DXGKFLIP_GUID)              Win7::HandleDxgkFlip           (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == Win7::DXGKPRESENTHISTORY_GUID)    Win7::HandleDxgkPresentHistory (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == Win7::DXGKQUEUEPACKET_GUID)       Win7::HandleDxgkQueuePacket    (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == Win7::DXGKVSYNCDPC_GUID)          Win7::HandleDxgkVSyncDPC       (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == Win7::DXGKMMIOFLIP_GUID)          Win7::HandleDxgkMMIOFlip       (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == EventMetadataGuid)                HandleMetadataEvent            (pEventRecord, gPMConsumer);
    else if (hdr.ProviderId == DHD_PROVIDER_GUID)                HandleDHDEvent                 (pEventRecord, gMRConsumer);
    else if (hdr.ProviderId == SPECTRUMCONTINUOUS_PROVIDER_GUID) HandleSpectrumContinuousEvent  (pEventRecord, gMRConsumer);
}

static ULONG CALLBACK BufferCallback(EVENT_TRACE_LOGFILEA* pLogFile)
{
    (void) pLogFile;
    return gContinueProcessingBuffers; // TRUE = continue processing events, FALSE = return out of ProcessTrace()
}

bool StartTraceSession()
{
    auto const& args = GetCommandLineArgs();
    auto simple = args.mVerbosity == Verbosity::Simple;

    // -------------------------------------------------------------------------
    // Configure session properties
    TraceProperties sessionProps = {};
    sessionProps.Wnode.BufferSize = (ULONG) sizeof(TraceProperties);
    sessionProps.Wnode.ClientContext = 1;                     // Clock resolution to use when logging the timestamp for each event; 1 == query performance counter
    sessionProps.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;    // We have a realtime consumer, not writing to a log file
    sessionProps.LogFileNameOffset = 0;                       // 0 means no output log file
    sessionProps.LoggerNameOffset = offsetof(TraceProperties, mSessionName);  // Location of session name; will be written by StartTrace()
    /* Not used:
    sessionProps.Wnode.Guid               // Only needed for private or kernel sessions, otherwise it's an output
    sessionProps.FlushTimer               // How often in seconds buffers are flushed; 0=min (1 second)
    sessionProps.EnableFlags              // Which kernel providers to include in trace
    sessionProps.AgeLimit                 // n/a
    sessionProps.BufferSize = 0;          // Size of each tracing buffer in kB (max 1MB)
    sessionProps.MinimumBuffers = 200;    // Min tracing buffer pool size; must be at least 2 per processor
    sessionProps.MaximumBuffers = 0;      // Max tracing buffer pool size; min+20 by default
    sessionProps.MaximumFileSize = 0;     // Max file size in MB
    */
    /* The following members are output variables, set by StartTrace() and/or ControlTrace()
    sessionProps.Wnode.HistoricalContext  // handle to the event tracing session
    sessionProps.Wnode.TimeStamp          // time this structure was updated
    sessionProps.Wnode.Guid               // session Guid
    sessionProps.Wnode.Flags              // e.g., WNODE_FLAG_TRACED_GUID
    sessionProps.NumberOfBuffers          // trace buffer pool size
    sessionProps.FreeBuffers              // trace buffer pool free count
    sessionProps.EventsLost               // count of events not written
    sessionProps.BuffersWritten           // buffers written in total
    sessionProps.LogBuffersLost           // buffers that couldn't be written to the log
    sessionProps.RealTimeBuffersLost      // buffers that couldn't be delivered to the realtime consumer
    sessionProps.LoggerThreadId           // tracing session identifier
    */

    // -------------------------------------------------------------------------
    // Configure trace properties
    EVENT_TRACE_LOGFILEA traceProps = {};
    traceProps.LogFileName = (LPSTR) args.mEtlFileName;
    traceProps.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    traceProps.EventRecordCallback = simple ? SimpleEventRecordCallback : EventRecordCallback;
    /* Optional:
    traceProps.BufferCallback
    traceProps.Context
     * Output members (passed also to BufferCallback()):
    traceProps.CurrentTime
    traceProps.BuffersRead
    traceProps.CurrentEvent
    traceProps.LogfileHeader
    traceProps.BufferSize
    traceProps.Filled
    traceProps.IsKernelTrace
     * Not used:
    traceProps.Context
    traceProps.EventsLost
    */

    // When processing log files, we need to use the buffer callback in case
    // the user wants to stop processing before the entire log has been parsed.
    if (traceProps.LogFileName != nullptr) {
        traceProps.BufferCallback = BufferCallback;
    }

    // Set realtime parameters
    if (traceProps.LogFileName == nullptr) {
        traceProps.LoggerName = (LPSTR) args.mSessionName;
        traceProps.ProcessTraceMode |= PROCESS_TRACE_MODE_REAL_TIME;
    }

    // -------------------------------------------------------------------------
    // Start the session
    auto status = StartTraceA(&gSessionHandle, args.mSessionName, &sessionProps);

    // If a session with this same name is already running, we either exit or
    // stop it and start a new session.  This is useful if a previous process
    // failed to properly shut down the session for some reason.
    if (status == ERROR_ALREADY_EXISTS) {
        if (args.mStopExistingSession) {
            fprintf(stderr,
                "warning: a trace session named \"%s\" is already running and it will be stopped.\n"
                "         Use -session_name with a different name to start a new session.\n",
                args.mSessionName);
        } else {
            fprintf(stderr,
                "error: a trace session named \"%s\" is already running. Use -stop_existing_session\n"
                "       to stop the existing session, or use -session_name with a different name to\n"
                "       start a new session.\n",
                args.mSessionName);
            gSessionHandle = 0;
            return false;
        }

        status = ControlTraceA((TRACEHANDLE) 0, args.mSessionName, &sessionProps, EVENT_TRACE_CONTROL_STOP);
        if (status == ERROR_SUCCESS) {
            status = StartTraceA(&gSessionHandle, args.mSessionName, &sessionProps);
        }
    }

    // Report error if we failed to start a new session
    if (status != ERROR_SUCCESS) {
        fprintf(stderr, "error: failed to start session (error=%lu).\n", status);
        gSessionHandle = 0;
        return false;
    }

    // Enable desired providers
    if (!EnableProviders()) {
        StopTraceSession();
        return false;
    }

    // -------------------------------------------------------------------------
    // Open the trace
    gTraceHandle = OpenTraceA(&traceProps);
    if (gTraceHandle == INVALID_PROCESSTRACE_HANDLE) {
        fprintf(stderr, "error: failed to open trace");
        auto lastError = GetLastError();
        switch (lastError) {
        case ERROR_FILE_NOT_FOUND:    fprintf(stderr, " (file not found)"); break;
        case ERROR_INVALID_PARAMETER: fprintf(stderr, " (Logfile is NULL)"); break;
        case ERROR_BAD_PATHNAME:      fprintf(stderr, " (invalid LoggerName)"); break;
        case ERROR_ACCESS_DENIED:     fprintf(stderr, " (access denied)"); break;
        default:                      fprintf(stderr, " (error=%u)", lastError); break;
        }
        fprintf(stderr, ".\n");
        StopTraceSession();
        return false;
    }

    // -------------------------------------------------------------------------
    // Store trace properties
    gQpcFrequency = traceProps.LogfileHeader.PerfFreq.QuadPart;

    // Use current time as start for realtime traces (instead of the first event time)
    if (!args.mEtlFileName) {
        QueryPerformanceCounter((LARGE_INTEGER*) &gQpcTraceStart);
    }

    DebugInitialize(&gQpcTraceStart, gQpcFrequency);

    // -------------------------------------------------------------------------
    // Start the consumer and output threads
    StartConsumerThread(gTraceHandle);
    StartOutputThread();

    return true;
}

void StopTraceSession()
{
    ULONG status = 0;

    // If collecting realtime events, CloseTrace() will cause ProcessTrace() to
    // stop filling buffers and it will return after it finishes processing
    // events already in it's buffers.
    //
    // If collecting from a log file, ProcessTrace() will continue to process
    // the entire file though, which is why we cancel the processing from the
    // BufferCallback in this case.
    gContinueProcessingBuffers = FALSE;

    // Shutdown the trace and session.
    status = CloseTrace(gTraceHandle);
    gTraceHandle = INVALID_PROCESSTRACE_HANDLE;

    DisableProviders();

    TraceProperties sessionProps = {};
    sessionProps.Wnode.BufferSize = (ULONG) sizeof(TraceProperties);
    sessionProps.LoggerNameOffset = offsetof(TraceProperties, mSessionName);
    status = ControlTraceW(gSessionHandle, nullptr, &sessionProps, EVENT_TRACE_CONTROL_STOP);
    gSessionHandle = 0;

    // Wait for the consumer and output threads to end (which are using the
    // consumers).
    WaitForConsumerThreadToExit();
    StopOutputThread();

    // Destruct the consumers
    delete gMRConsumer;
    delete gPMConsumer;
    gMRConsumer = nullptr;
    gPMConsumer = nullptr;
}

void CheckLostReports(uint32_t* eventsLost, uint32_t* buffersLost)
{
    TraceProperties sessionProps = {};
    sessionProps.Wnode.BufferSize = (ULONG) sizeof(TraceProperties);
    sessionProps.LoggerNameOffset = offsetof(TraceProperties, mSessionName);

    auto status = ControlTraceW(gSessionHandle, nullptr, &sessionProps, EVENT_TRACE_CONTROL_QUERY);
    (void) status;

    *eventsLost = sessionProps.EventsLost;
    *buffersLost = sessionProps.RealTimeBuffersLost;
}

void DequeueAnalyzedInfo(
    std::vector<NTProcessEvent>* ntProcessEvents,
    std::vector<std::shared_ptr<PresentEvent>>* presents,
    std::vector<std::shared_ptr<LateStageReprojectionEvent>>* lsrs)
{
    gPMConsumer->DequeueProcessEvents(*ntProcessEvents);
    gPMConsumer->DequeuePresents(*presents);
    if (gMRConsumer != nullptr) {
        gMRConsumer->DequeueLSRs(*lsrs);
    }
}

double QpcDeltaToSeconds(uint64_t qpcDelta)
{
    return (double) qpcDelta / gQpcFrequency;
}

uint64_t SecondsDeltaToQpc(double secondsDelta)
{
    return (uint64_t) (secondsDelta * gQpcFrequency);
}

double QpcToSeconds(uint64_t qpc)
{
    return QpcDeltaToSeconds(qpc - gQpcTraceStart);
}

