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

static bool CombineArguments(
    int argc,
    char** argv,
    char* out,
    size_t outSize)
{
    size_t idx = 0;
    for (int i = 1; i < argc && idx < outSize; ++i) {
        if (idx >= outSize) {
            return false; // was truncated
        }

        if (argv[i][0] != '\"' && strchr(argv[i], ' ')) {
            idx += snprintf(out + idx, outSize - idx, " \"%s\"", argv[i]);
        } else {
            idx += snprintf(out + idx, outSize - idx, " %s", argv[i]);
        }
    }

    return true;
}

void SetConsoleTitle(
    int argc,
    char** argv)
{
    char args[MAX_PATH] = "PresentMon";
    size_t idx = strlen(args);
    if (!CombineArguments(argc, argv, args + idx, MAX_PATH - idx)) {
        args[MAX_PATH - 4] = '.';
        args[MAX_PATH - 3] = '.';
        args[MAX_PATH - 2] = '.';
    }

    SetConsoleTitleA(args);
}

void SetConsoleText(const char *text)
{
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    enum { MAX_BUFFER = 16384 };
    char buffer[16384];
    int bufferSize = 0;
    auto write = [&](char ch) {
        if (bufferSize < MAX_BUFFER) {
            buffer[bufferSize++] = ch;
        }
    };

    if (!GetConsoleScreenBufferInfo(hConsole, &csbi))
    {
        return;
    }

    int oldBufferSize = int(csbi.dwSize.X * csbi.dwSize.Y);
    if (oldBufferSize > MAX_BUFFER) {
        oldBufferSize = MAX_BUFFER;
    }

    int x = 0;
    while (*text) {
        int repeat = 1;
        char ch = *text;
        if (ch == '\t') {
            ch = ' ';
            repeat = 4;
        }
        else if (ch == '\n') {
            ch = ' ';
            repeat = csbi.dwSize.X - x;
        }
        for (int i = 0; i < repeat; ++i) {
            write(ch);
            if (++x >= csbi.dwSize.X) {
                x = 0;
            }
        }
        text++;
    }

    for (int i = bufferSize; i < oldBufferSize; ++i)
    {
        write(' ');
    }

    COORD origin = { 0,0 };
    DWORD dwCharsWritten;
    WriteConsoleOutputCharacterA(
        hConsole,
        buffer,
        bufferSize,
        origin,
        &dwCharsWritten);

    SetConsoleCursorPosition(hConsole, origin);
}

void UpdateConsole(PresentMonData const& pm, uint64_t now, uint64_t perfFreq, std::string* display)
{
    // ProcessInfo
    for (auto const& p : pm.mProcessMap) {
        auto processId = p.first;
        auto const& proc = p.second;

        // Don't display non-specified or empty processes
        if (!proc.mTargetProcess ||
            proc.mModuleName.empty() ||
            proc.mChainMap.empty()) {
            continue;
        }

        char str[256] = {};
        _snprintf_s(str, _TRUNCATE, "\n%s[%d]:\n", proc.mModuleName.c_str(), processId);
        *display += str;

        for (auto& chain : proc.mChainMap)
        {
            double fps = chain.second.ComputeFps(perfFreq);

            _snprintf_s(str, _TRUNCATE, "\t%016llX (%s): SyncInterval %d | Flags %d | %.2lf ms/frame (%.1lf fps, ",
                chain.first,
                RuntimeToString(chain.second.mRuntime),
                chain.second.mLastSyncInterval,
                chain.second.mLastFlags,
                1000.0/fps,
                fps);
            *display += str;

            if (pm.mArgs->mVerbosity > Verbosity::Simple) {
                _snprintf_s(str, _TRUNCATE, "%.1lf displayed fps, ", chain.second.ComputeDisplayedFps(perfFreq));
                *display += str;
            }

            _snprintf_s(str, _TRUNCATE, "%.2lf ms CPU", chain.second.ComputeCpuFrameTime(perfFreq) * 1000.0);
            *display += str;

            if (pm.mArgs->mVerbosity > Verbosity::Simple) {
                _snprintf_s(str, _TRUNCATE, ", %.2lf ms latency) (%s",
                    1000.0 * chain.second.ComputeLatency(perfFreq),
                    PresentModeToString(chain.second.mLastPresentMode));
                *display += str;

                if (chain.second.mLastPresentMode == PresentMode::Hardware_Composed_Independent_Flip) {
                    _snprintf_s(str, _TRUNCATE, ": Plane %d", chain.second.mLastPlane);
                    *display += str;
                }

                if ((chain.second.mLastPresentMode == PresentMode::Hardware_Composed_Independent_Flip ||
                     chain.second.mLastPresentMode == PresentMode::Hardware_Independent_Flip) &&
                    pm.mArgs->mVerbosity >= Verbosity::Verbose &&
                    chain.second.mDwmNotified) {
                    _snprintf_s(str, _TRUNCATE, ", DWM notified");
                    *display += str;
                }

                if (pm.mArgs->mVerbosity >= Verbosity::Verbose &&
                    chain.second.mHasBeenBatched) {
                    _snprintf_s(str, _TRUNCATE, ", batched");
                    *display += str;
                }
            }

            _snprintf_s(str, _TRUNCATE, ")%s\n",
                (now - chain.second.mLastUpdateTicks) > 1000 ? " [STALE]" : "");
            *display += str;
        }
    }
}

