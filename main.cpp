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

bool g_Quit = false;
static std::thread *g_PresentMonThread;

BOOL WINAPI HandlerRoutine(
    _In_ DWORD dwCtrlType
    )
{
    g_Quit = true;
    if (g_PresentMonThread) {
        g_PresentMonThread->join();
    }
    exit(0);
    return TRUE;
}

int main(int argc, char ** argv)
{
    --argc;
    ++argv;

    if (argc == 0) {
        printf(
            "command line options:\n"
            " -captureall: record ALL processes (default).\n"
            " -process_name [exe name]: record specific process.\n"
            " -process_id [integer]: record specific process ID.\n"
            " -output_file [path]: override the default output path.\n"
            " -no_csv: do not create any output file.\n"
            );
        return 0;
    }

    int waitpid = -1;
    PresentMonArgs args;

    args.mTargetProcessName = "*";

    for (int i = 0; i < argc; ++i)
    {
        // 2-component arguments
        if (i + 1 < argc)
        {
            if (!strcmp(argv[i], "-waitpid"))
            {
                waitpid = atoi(argv[++i]);
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
        }
        // 1-component args
        else
        {
            if (!strcmp(argv[i], "-no_csv"))
            {
                args.mOutputFileName = "*";
            }
        }
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

    // Run PM in a separate thread so we can join it in the CtrlHandler (can't join the main thread)
    std::thread pm(PresentMonEtw, args);
    g_PresentMonThread = &pm;
    Sleep(INFINITE);
}