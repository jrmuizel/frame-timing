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

#define BUILDNUM 106
static const wchar_t* c_ClassName = L"PresentMon";

#include <windows.h>
#include <thread>
#include <cstdio>

#include "PresentMon.hpp"
#include "Util.hpp"
#include <mutex>

static const uint32_t c_Hotkey = 0x80;

bool g_Quit = false;
static std::thread *g_PresentMonThread;
static std::mutex *g_ExitMutex;

void StartPresentMonThread(PresentMonArgs& args)
{
    *g_PresentMonThread = std::thread(PresentMonEtw, args);
}

BOOL WINAPI HandlerRoutine(
    _In_ DWORD dwCtrlType
    )
{
    std::lock_guard<std::mutex> lock(*g_ExitMutex);
    g_Quit = true;
    if (g_PresentMonThread && g_PresentMonThread->joinable()) {
        g_PresentMonThread->join();
    }
    return TRUE;
}

void printHelp()
{
    printf("PresentMon Build %d\n",
        BUILDNUM);
    printf(
        "\nCommand line options:\n"
        " -captureall: record ALL processes (default).\n"
        " -process_name [exe name]: record specific process.\n"
        " -process_id [integer]: record specific process ID.\n"
        " -output_file [path]: override the default output path.\n"
        " -etl_file [path]: consume events from an ETL file instead of real-time.\n"
        " -delay [seconds]: wait before starting to consume events.\n"
        " -timed [seconds]: stop listening and exit after a set amount of time.\n"
        " -no_csv: do not create any output file.\n"
        " -exclude_dropped: exclude dropped presents from the csv output.\n"
        " -scroll_toggle: only record events while scroll lock is enabled.\n"
        " -simple: disable advanced tracking. try this if you encounter crashes.\n"
        " -terminate_on_proc_exit: terminate PresentMon when all instances of the specified process exit.\n"
        " -hotkey: use F11 to start and stop listening, writing to a unique file each time.\n"
        "          delay kicks in after hotkey (each time), timer starts ticking from hotkey press.\n"
        );
    printf("\nCSV columns explained (self explanatory columns omitted):\n"
        "  Dropped: boolean indicator. 1 = dropped, 0 = displayed.\n"
        "  MsBetweenPresents: time between this Present() API call and the previous one.\n"
        "  MsBetweenDisplayChange: time between when this frame was displayed, and previous was displayed.\n"
        "  MsInPresentAPI: time spent inside the Present() API call.\n"
        "  MsUntilRenderComplete: time between present start and GPU work completion.\n"
        "  MsUntilDisplayed: time between present start and frame display.\n"
        );
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (uMsg == WM_HOTKEY && wParam == c_Hotkey)
	{
		auto& args = *reinterpret_cast<PresentMonArgs*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
		if (g_PresentMonThread->joinable())
		{
			HandlerRoutine(CTRL_C_EVENT);
			g_Quit = false;
			args.mRestartCount++;
		}
		else
		{
			StartPresentMonThread(args);
		}
	}

	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

HWND CreateMessageWindow(PresentMonArgs& args)
{
    WNDCLASSEXW Class = { sizeof(Class) };
    Class.lpfnWndProc = WindowProc;
    Class.lpszClassName = c_ClassName;
    if (!RegisterClassExW(&Class))
    {
        printf("Failed to register hotkey class.\n");
        return 0;
    }

    HWND hWnd = CreateWindowExW(0, c_ClassName, L"PresentMonWnd", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, nullptr);
    if (!hWnd)
    {
        printf("Failed to create hotkey window.\n");
        return 0;
    }

    if (!RegisterHotKey(hWnd, c_Hotkey, MOD_NOREPEAT, VK_F11))
    {
        printf("Failed to register hotkey.\n");
        DestroyWindow(hWnd);
        return 0;
    }

    SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&args));

    return hWnd;
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
            else if (!strcmp(argv[i], "-exclude_dropped"))
            {
                args.mExcludeDropped = true;
            }
            else if (!strcmp(argv[i], "-scroll_toggle"))
            {
                args.mScrollLockToggle = true;
            }
            else if (!strcmp(argv[i], "-simple"))
            {
                args.mSimple = true;
            }
            else if (!strcmp(argv[i], "-terminate_on_proc_exit"))
            {
                args.mTerminateOnProcExit = true;
            }
            else if (!strcmp(argv[i], "-hotkey"))
            {
                args.mHotkeySupport = true;
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

    if (!args.mEtlFileName && !HaveAdministratorPrivileges()) {
        printf("Process is not running as admin. Attempting to elevate.\n");
        RestartAsAdministrator(argc, argv);
        return 0;
    }

    if (args.mEtlFileName && args.mHotkeySupport) {
        printf("ETL files not supported with hotkeys.\n");
        return 0;
    }

    SetConsoleCtrlHandler(HandlerRoutine, TRUE);
    SetConsoleTitleA(title_string.c_str());

    std::mutex exit_mutex;
    g_ExitMutex = &exit_mutex;

    // Run PM in a separate thread so we can join it in the CtrlHandler (can't join the main thread)
    std::thread pm;
    g_PresentMonThread = &pm;

    if (args.mHotkeySupport)
    {
        HWND hWnd = CreateMessageWindow(args);
        MSG message = {};
        while (hWnd && !g_Quit)
        {
            while (PeekMessageW(&message, hWnd, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
    else
    {
        StartPresentMonThread(args);
        while (!g_Quit)
        {
            Sleep(100);
        }
    }

    // Wait for tracing to finish, to ensure the PM thread closes the session correctly
    // Prevent races on joining the PM thread between the control handler and the main thread
    std::lock_guard<std::mutex> lock(exit_mutex);
    if (g_PresentMonThread->joinable()) {
        g_PresentMonThread->join();
    }
    return 0;
}
