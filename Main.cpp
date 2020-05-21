
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

int main(int argc, char *argv[])
{
    bool expectFilteredEvents = false;
    bool simple = false;
    gPMConsumer = new PMTraceConsumer(expectFilteredEvents, simple);
    auto status = gSession.Start(gPMConsumer, nullptr, argv[1], nullptr);
    ProcessTrace(&gSession.mTraceHandle, 1, NULL, NULL);
    /*for (auto p : gPMConsumer->mCompletedPresents) {
        std::cout << p->ThreadId << " " << p->QueueSubmitSequence << " "  << p->ReadyTime - gSession.mStartQpc.QuadPart << "\n";
    }*/
    int late_frames = 0;
    for (auto f : gPMConsumer->mFrames) {
        if (f.present) {
            auto p = f.present;
            auto start_time = f.StartTime - gSession.mStartQpc.QuadPart;
            auto combined_time = QpcDeltaToMilliSeconds(p->ReadyTime - f.StartTime);
            auto renderer_time = QpcDeltaToMilliSeconds(p->QpcTime - f.StartTime);
            auto gpu_time = QpcDeltaToMilliSeconds(p->ReadyTime - p->QpcTime);
            auto screen_time = QpcDeltaToMilliSeconds(p->ScreenTime - f.StartTime);
            if (screen_time > 33.) {
                late_frames++;
            }
            std::cout << start_time << "," << renderer_time << ", " << gpu_time << ", " << combined_time << ", " << screen_time << "\n";
        }
    }
    std::cout << "late_frames: " << late_frames;
}