/*
Copyright 2017-2018 Intel Corporation

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

#include <windows.h>
#include <vector>

enum class Verbosity {
    Simple,
    Normal,
    Verbose
};

struct CommandLineArgs {
    std::vector<const char*> mTargetProcessNames;
    std::vector<const char*> mExcludeProcessNames;
    const char *mOutputFileName;
    const char *mEtlFileName;
    const char *mSessionName;
    UINT mTargetPid;
    UINT mDelay;
    UINT mTimer;
    UINT mRecordingCount;
    UINT mHotkeyModifiers;
    UINT mHotkeyVirtualKeyCode;
    Verbosity mVerbosity;
    bool mOutputFile;
    bool mScrollLockToggle;
    bool mScrollLockIndicator;
    bool mExcludeDropped;
    bool mSimpleConsole;
    bool mTerminateOnProcExit;
    bool mTerminateAfterTimer;
    bool mHotkeySupport;
    bool mTryToElevate;
    bool mIncludeWindowsMixedReality;
    bool mMultiCsv;
    bool mStopExistingSession;
};

bool ParseCommandLine(int argc, char** argv, CommandLineArgs* out);
void SetConsoleTitle(int argc, char** argv);
bool EnableScrollLock(bool enable);
