/*
Copyright 2019 Intel Corporation

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

#pragma once

#define DEBUG_VERBOSE 0
#if DEBUG_VERBOSE

#define DEBUG_START_TIME_NS     0ull    /* 0 means first event */
#define DEBUG_STOP_TIME_NS      0ull    /* 0 means end of trace */

#define NOMINMAX
#include <stdint.h>
#include <windows.h>
#include <evntcons.h> // must include after windows.h

struct PresentEvent; // Can't include PresentMonTraceConsumer.hpp because it includes Debug.hpp (before defining PresentEvent)

void DebugInitialize(uint64_t* firstTimestamp, uint64_t timestampFrequency);
bool DebugDone();
void DebugEvent(EVENT_RECORD* eventRecord);
void DebugCreatePresent(PresentEvent const& p);
void DebugCompletePresent(PresentEvent const& p, int indent);
void DebugPrintPresentMode(PresentEvent const& p);
void DebugPrintDwmNotified(PresentEvent const& p);
void DebugPrintTokenPtr(PresentEvent const& p);

#else

#define DebugInitialize(firstTimestamp, timestampFrequency) (void) firstTimestamp, timestampFrequency
#define DebugDone()                                         false
#define DebugEvent(eventRecord)                             (void) eventRecord
#define DebugCreatePresent(p)                               (void) p
#define DebugCompletePresent(p, indent)                     (void) p, indent
#define DebugPrintPresentMode(p)                            (void) p
#define DebugPrintDwmNotified(p)                            (void) p
#define DebugPrintTokenPtr(p)                               (void) p

#endif
