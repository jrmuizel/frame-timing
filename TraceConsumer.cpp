#include "TraceConsumer.hpp"

void MultiTraceConsumer::OnEventRecord(_In_ PEVENT_RECORD pEventRecord)
{
    for (auto traceConsumer : mTraceConsumers) {
        traceConsumer->OnEventRecord(pEventRecord);
    }
}

bool MultiTraceConsumer::ContinueProcessing()
{
    for (auto traceConsumer : mTraceConsumers) {
        if (!traceConsumer->ContinueProcessing())
        {
            return false;
        }
    }

    return true;
}