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

static bool gThreadRunning = false;
static std::thread gThread;

static void Consume(TRACEHANDLE traceHandle)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // You must call OpenTrace() prior to calling this function
    //
    // ProcessTrace() blocks the calling thread until it
    //     1) delivers all events,
    //     2) the BufferCallback function returns FALSE,
    //     3) you call CloseTrace(),
    //     4) the controller stops the trace session (if realtime collection).
    //
    // There may be a several second delay before the function returns.
    //
    // ProcessTrace() is supposed to return ERROR_CANCELLED if BufferCallback
    // (EtwThreadsShouldQuit) returns FALSE; and ERROR_SUCCESS if the trace
    // completes (parses the entire ETL, fills the maximum file size, or is
    // explicitly closed).
    //
    // However, it seems to always return ERROR_SUCCESS.

    auto status = ProcessTrace(&traceHandle, 1, NULL, NULL);
    (void) status;

    // If ProcessTrace() finished on it's own, record that was the end
    // condition and signal MainThread to shut everything down.
    if (!EtwThreadsShouldQuit()) {
        gThreadRunning = false;
        PostStopRecording();
        PostQuitProcess();
    }
}

void StartConsumerThread(TRACEHANDLE traceHandle)
{
    gThreadRunning = true;
    gThread = std::thread(Consume, traceHandle);
}

bool IsConsumerThreadRunning()
{
    return gThreadRunning;
}

void WaitForConsumerThreadToExit()
{
    assert(gThread.joinable());
    gThread.join();
}
