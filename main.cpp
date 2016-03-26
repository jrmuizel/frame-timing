//--------------------------------------------------------------------------------------
// Copyright 2015 Intel Corporation
// All Rights Reserved
//
// Permission is granted to use, copy, distribute and prepare derivative works of this
// software for any purpose and without fee, provided, that the above copyright notice
// and this statement appear in all copies.  Intel makes no representations about the
// suitability of this software for any purpose.  THIS SOFTWARE IS PROVIDED "AS IS."
// INTEL SPECIFICALLY DISCLAIMS ALL WARRANTIES, EXPRESS OR IMPLIED, AND ALL LIABILITY,
// INCLUDING CONSEQUENTIAL AND OTHER INDIRECT DAMAGES, FOR THE USE OF THIS SOFTWARE,
// INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PROPRIETARY RIGHTS, AND INCLUDING THE
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.  Intel does not
// assume any responsibility for any errors which may appear in this software nor any
// responsibility to update it.
//--------------------------------------------------------------------------------------

#include <windows.h>
#include <thread>
#include <cstdio>

#include "PresentMon.hpp"
#include "Util.hpp"
#include <mutex>

bool g_Quit = false;
static std::thread *g_PresentMonThread;
static std::mutex *g_ExitMutex;

BOOL WINAPI HandlerRoutine(
    _In_ DWORD dwCtrlType
    )
{
    std::lock_guard<std::mutex> lock(*g_ExitMutex);
    g_Quit = true;
    if (g_PresentMonThread) {
        g_PresentMonThread->join();
    }
    return TRUE;
}

void printHelp()
{
    printf(
        "command line options:\n"
        " -captureall: record ALL processes (default).\n"
        " -process_name [exe name]: record specific process.\n"
        " -process_id [integer]: record specific process ID.\n"
        " -output_file [path]: override the default output path.\n"
        " -etl_file [path]: consume events from an ETL file instead of real-time.\n"
        " -delay [seconds]: wait before starting to consume events (allowing time for alt+tab)\n"
        " -timed [seconds]: stop listening and exit after a set amount of time\n"
        " -no_csv: do not create any output file.\n"
        " -scroll_toggle: only record events while scroll lock is enabled.\n"
        );
}

int main(int argc, char ** argv)
{
    --argc;
    ++argv;

    if (argc == 0) {
        printHelp();
        return 0;
    }

    int waitpid = -1;
    PresentMonArgs args;
    std::string title_string = "PresentMon";

    args.mTargetProcessName = "*";

    for (int i = 0; i < argc; ++i)
    {
        // 2-component arguments
        if (i + 1 < argc)
        {
            if (!strcmp(argv[i], "-waitpid"))
            {
                waitpid = atoi(argv[++i]);
                continue;
            }
            else if (!strcmp(argv[i], "-process_name"))
            {
                args.mTargetProcessName = argv[++i];
            }
            else if (!strcmp(argv[i], "-process_id"))
            {
                args.mTargetPid = atoi(argv[++i]);
            }
            else if (!strcmp(argv[i], "-output_file"))
            {
                args.mOutputFileName = argv[++i];
            }
            else if (!strcmp(argv[i], "-etl_file"))
            {
                args.mEtlFileName = argv[++i];
            }
            else if (!strcmp(argv[i], "-delay"))
            {
                args.mDelay = atoi(argv[++i]);
            }
            else if (!strcmp(argv[i], "-timed"))
            {
                args.mTimer = atoi(argv[++i]);
            }
        }
        // 1-component args
        {
            if (!strcmp(argv[i], "-no_csv"))
            {
                args.mOutputFileName = "*";
            }
            else if (!strcmp(argv[i], "-scroll_toggle"))
            {
                args.mScrollLockToggle = true;
            }
            else if (!strcmp(argv[i], "-?") || !strcmp(argv[i], "-help"))
            {
                printHelp();
                return 0;
            }
        }

        title_string += ' ';
        title_string += argv[i];
    }

    if (waitpid >= 0) {
        WaitForProcess(waitpid);
        if (!HaveAdministratorPrivileges()) {
            printf("Elevation process failed. Aborting.\n");
            return 0;
        }
    }

    if (!HaveAdministratorPrivileges()) {
        printf("Process is not running as admin. Attempting to elevate.\n");
        RestartAsAdministrator(argc, argv);
        return 0;
    }

    SetConsoleCtrlHandler(HandlerRoutine, TRUE);
    SetConsoleTitleA(title_string.c_str());

    std::mutex exit_mutex;
    g_ExitMutex = &exit_mutex;

    // Run PM in a separate thread so we can join it in the CtrlHandler (can't join the main thread)
    std::thread pm(PresentMonEtw, args);
    g_PresentMonThread = &pm;
    while (!g_Quit)
    {
        Sleep(100);
    }

    // Wait for tracing to finish, to ensure the PM thread closes the session correctly
    // Prevent races on joining the PM thread between the control handler and the main thread
    std::lock_guard<std::mutex> lock(exit_mutex);
    if (g_PresentMonThread->joinable()) {
        g_PresentMonThread->join();
    }
    return 0;
}