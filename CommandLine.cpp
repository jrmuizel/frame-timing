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

#include <stdio.h>

#include "PresentMon.hpp"

namespace {

bool CombineArguments(
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

void PrintHelp()
{
    // NOTE: remember to update README.md when modifying usage
    fprintf(stderr,
        "PresentMon version 1.0.1\n"
        "\n"
        "Capture target options (use one of the following):\n"
        "    -captureall                Record all processes (default).\n"
        "    -process_name [exe name]   Record specific process specified by name.\n"
        "    -process_id [integer]      Record specific process specified by ID.\n"
        "    -etl_file [path]           Consume events from an ETL file instead of a running process.\n"
        "\n"
        "Output options:\n"
        "    -no_csv                    Do not create any output file.\n"
        "    -output_file [path]        Write CSV output to specified path. Otherwise, the default is\n"
        "                               PresentMon-PROCESSNAME-TIME.csv.\n"
        "\n"
        "Control and filtering options:\n"
        "    -scroll_toggle             Only record events while scroll lock is enabled.\n"
        "    -hotkey                    Use F11 to start and stop recording, writing to a unique file each time.\n"
        "    -delay [seconds]           Wait for specified time before starting to record. When using\n"
        "                               -hotkey, delay occurs each time recording is started.\n"
        "    -timed [seconds]           Stop recording after the specified amount of time.  PresentMon will exit\n"
        "                               timer expires.\n"
        "    -exclude_dropped           Exclude dropped presents from the csv output.\n"
        "    -terminate_on_proc_exit    Terminate PresentMon when all instances of the specified process exit.\n"
        "    -simple                    Disable advanced tracking (try this if you encounter crashes).\n"
        "    -dont_restart_as_admin     Don't try to elevate privilege.\n"
        "    -no_top                    Don't display active swap chains in the console window.\n"
        );
}

}

bool ParseCommandLine(int argc, char** argv, CommandLineArgs* args)
{
    for (int i = 1; i < argc; ++i) {
#define ARG1(Arg, Assign) \
        if (strcmp(argv[i], Arg) == 0) { \
            Assign; \
            continue; \
        }

#define ARG2(Arg, Assign) \
        if (strcmp(argv[i], Arg) == 0) { \
            if (++i < argc) { \
                Assign; \
                continue; \
            } \
            fprintf(stderr, "error: %s expecting argument.\n", Arg); \
        }

        // Capture target options
             ARG1("-captureall",             args->mTargetProcessName   = nullptr)
        else ARG2("-process_name",           args->mTargetProcessName   = argv[i])
        else ARG2("-process_id",             args->mTargetPid           = atoi(argv[i]))
        else ARG2("-etl_file",               args->mEtlFileName         = argv[i])

        // Output options
        else ARG1("-no_csv",                 args->mOutputFile          = false)
        else ARG2("-output_file",            args->mOutputFileName      = argv[i])

        // Control and filtering options
        else ARG1("-hotkey",                 args->mHotkeySupport       = true)
        else ARG1("-scroll_toggle",          args->mScrollLockToggle    = true)
        else ARG2("-delay",                  args->mDelay               = atoi(argv[i]))
        else ARG2("-timed",                  args->mTimer               = atoi(argv[i]))
        else ARG1("-exclude_dropped",        args->mExcludeDropped      = true)
        else ARG1("-terminate_on_proc_exit", args->mTerminateOnProcExit = true)
        else ARG1("-simple",                 args->mSimple              = true)
        else ARG1("-dont_restart_as_admin",  args->mTryToElevate        = false)
        else ARG1("-no_top",                 args->mSimpleConsole       = true)

        // Provided argument wasn't recognized
        else fprintf(stderr, "error: unexpected argument '%s'.\n", argv[i]);

        PrintHelp();
        return false;
    }

    // Validate command line arguments
    if (((args->mTargetProcessName == nullptr) ? 0 : 1) +
        ((args->mTargetPid         <= 0      ) ? 0 : 1) +
        ((args->mEtlFileName       == nullptr) ? 0 : 1) > 1) {
        fprintf(stderr, "error: only specify one of -captureall, -process_name, -process_id, or -etl_file.\n");
        PrintHelp();
        return false;
    }

    if (args->mEtlFileName && args->mHotkeySupport) {
        fprintf(stderr, "error: -etl_file and -hotkey arguments are not compatible.\n");
        PrintHelp();
        return false;
    }

    return true;
}

bool RestartAsAdministrator(
    int argc,
    char** argv)
{
    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));

    char args[MAX_PATH] = {};
    if (!CombineArguments(argc, argv, args, MAX_PATH)) {
        return false;
    }

    ShellExecuteA(NULL, "runas", exe_path, args, NULL, SW_SHOW);
    return true;
}

void SetConsoleTitle(
    int argc,
    char** argv)
{
    char args[MAX_PATH] = {};
    size_t idx = snprintf(args, MAX_PATH, "PresentMon");
    if (!CombineArguments(argc, argv, args + idx, MAX_PATH - idx)) {
        args[MAX_PATH - 4] = '.';
        args[MAX_PATH - 3] = '.';
        args[MAX_PATH - 2] = '.';
    }

    SetConsoleTitleA(args);
}
