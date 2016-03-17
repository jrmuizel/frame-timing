// Code based on:
// http://chabster.blogspot.com/2012/10/realtime-etw-consumer-howto.html
//

#include "TraceSession.hpp"
#include <tchar.h>
#include <cstdio>

static VOID WINAPI EventRecordCallback(_In_ PEVENT_RECORD pEventRecord)
{
    reinterpret_cast<ITraceConsumer *>(pEventRecord->UserContext)->OnEventRecord(pEventRecord);
}

static ULONG WINAPI BufferRecordCallback(_In_ PEVENT_TRACE_LOGFILE Buffer)
{
    return reinterpret_cast<ITraceConsumer *>(Buffer->Context)->ContinueProcessing();
}

TraceSession::TraceSession(LPCTSTR szSessionName) 
    : _szSessionName(_tcsdup(szSessionName))
    , _status(0)
    , _pSessionProperties(0)
    , hSession(0)
    , _hTrace(0)
    , _eventsLost(0)
    , _buffersLost(0)
{
}

TraceSession::~TraceSession(void)
{
    free(_szSessionName);
    free(_pSessionProperties);
}

bool TraceSession::Start()
{
    if (!_pSessionProperties) {
        static_assert(sizeof(EVENT_TRACE_PROPERTIES) == 120, "");
        const ULONG buffSize = sizeof(EVENT_TRACE_PROPERTIES) + (ULONG)(_tcslen(_szSessionName) + 1) * sizeof(TCHAR);
        _pSessionProperties = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(malloc(buffSize));
        ZeroMemory(_pSessionProperties, buffSize);
        _pSessionProperties->Wnode.BufferSize = buffSize;
        _pSessionProperties->Wnode.ClientContext = 1;
        _pSessionProperties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        _pSessionProperties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        _pSessionProperties->BufferSize = 0;
        _pSessionProperties->MaximumBuffers = 100;
        _pSessionProperties->MaximumBuffers = 200;
    }

    // Create the trace session.
    _status = StartTraceW(&hSession, _szSessionName, _pSessionProperties);

    return (_status == ERROR_SUCCESS);
}

bool TraceSession::EnableProvider(const GUID& providerId, UCHAR level, ULONGLONG anyKeyword, ULONGLONG allKeyword)
{
    _status = EnableTraceEx2(hSession, &providerId, EVENT_CONTROL_CODE_ENABLE_PROVIDER, level, anyKeyword, allKeyword, 0, NULL);
    return (_status == ERROR_SUCCESS);
}

bool TraceSession::OpenTrace(ITraceConsumer *pConsumer)
{
    if (!pConsumer)
        return false;

    ZeroMemory(&_logFile, sizeof(EVENT_TRACE_LOGFILE));
    _logFile.LoggerName = _szSessionName;
    _logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    _logFile.EventRecordCallback = &EventRecordCallback;
    _logFile.BufferCallback = &BufferRecordCallback;
    _logFile.Context = pConsumer;

    _hTrace = ::OpenTraceW(&_logFile);
    return (_hTrace != 0);
}

bool TraceSession::Process()
{
    _status = ProcessTrace(&_hTrace, 1, NULL, NULL);
    return (_status == ERROR_SUCCESS);
}

bool TraceSession::CloseTrace()
{
    _status = ::CloseTrace(_hTrace);
    return (_status == ERROR_SUCCESS);
}

bool TraceSession::DisableProvider(const GUID& providerId)
{
    _status = EnableTraceEx2(hSession, &providerId, EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, NULL);
    return (_status == ERROR_SUCCESS);
}

bool TraceSession::Stop()
{
    _status = ControlTraceW(hSession, _szSessionName, _pSessionProperties, EVENT_TRACE_CONTROL_STOP);
    delete _pSessionProperties;
    _pSessionProperties = NULL;

    return (_status == ERROR_SUCCESS);
}

bool TraceSession::AnythingLost(uint32_t &events, uint32_t &buffers)
{
    _status = ControlTraceW(hSession, _szSessionName, _pSessionProperties, EVENT_TRACE_CONTROL_QUERY);
    if (_status != ERROR_SUCCESS && _status != ERROR_MORE_DATA) {
        return true;
    }
    _status = ERROR_SUCCESS;
    events = _pSessionProperties->EventsLost - _eventsLost;
    buffers = _pSessionProperties->RealTimeBuffersLost - _buffersLost;
    _eventsLost = _pSessionProperties->EventsLost;
    _buffersLost = _pSessionProperties->RealTimeBuffersLost;
    return events > 0 || buffers > 0;
}

ULONG TraceSession::Status() const
{
    return _status;
}

LONGLONG TraceSession::PerfFreq() const
{
    return _logFile.LogfileHeader.PerfFreq.QuadPart;
}