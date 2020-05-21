
#include <iostream>
#include <windows.h>
#include <tdh.h>

#pragma comment(lib, "tdh.lib")

#include "TraceSession.hpp"
#include "PresentMonTraceConsumer.hpp"

namespace {
    TraceSession gSession;
    static PMTraceConsumer* gPMConsumer = nullptr;

}

double QpcDeltaToSeconds(uint64_t qpcDelta)
{
    return (double)qpcDelta / gSession.mQpcFrequency.QuadPart;
}

uint64_t SecondsDeltaToQpc(double secondsDelta)
{
    return (uint64_t)(secondsDelta * gSession.mQpcFrequency.QuadPart);
}

double QpcToSeconds(uint64_t qpc)
{
    return QpcDeltaToSeconds(qpc - gSession.mStartQpc.QuadPart);
}

double QpcDeltaToMilliSeconds(uint64_t qpc)
{
    return QpcDeltaToSeconds(qpc) * 1000.;
}

int main()
{
    bool expectFilteredEvents = false;
    bool simple = false;
    gPMConsumer = new PMTraceConsumer(expectFilteredEvents, simple);
    auto status = gSession.Start(gPMConsumer, nullptr, "C:\\Users\\mozilla\\Share\\Merged-frames.etl", nullptr);
    ProcessTrace(&gSession.mTraceHandle, 1, NULL, NULL);
    /*for (auto p : gPMConsumer->mCompletedPresents) {
        std::cout << p->ThreadId << " " << p->QueueSubmitSequence << " "  << p->ReadyTime - gSession.mStartQpc.QuadPart << "\n";
    }*/
    for (auto f : gPMConsumer->mFrames) {
        if (f.present) {
            auto p = f.present;
            auto time = QpcDeltaToMilliSeconds(p->ReadyTime - f.StartTime);
            std::cout << time << "\n";
        }
    }
}