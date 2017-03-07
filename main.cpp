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

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <thread>

#include "PresentMon.hpp"

static const uint32_t c_Hotkey = 0x80;

bool g_Quit = false;
static std::thread *g_PresentMonThread;
static std::mutex *g_ExitMutex;

bool HaveAdministratorPrivileges();
void RestartAsAdministrator(int argc, char** argv);
void SetConsoleTitle(int argc, char** argv);

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
    Class.lpszClassName = L"PresentMon";
    if (!RegisterClassExW(&Class))
    {
        printf("Failed to register hotkey class.\n");
        return 0;
    }

    HWND hWnd = CreateWindowExW(0, Class.lpszClassName, L"PresentMonWnd", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, nullptr);
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

void PrintHelp()
{
    // NOTE: remember to update README.md when modifying usage
    fprintf(stderr,
        "PresentMon development version\n"
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
        );
}

int main(int argc, char** argv)
{
    // Parse command line arguments
    PresentMonArgs args;
    bool tryToElevate = true;

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
             ARG1("-captureall",             args.mTargetProcessName   = nullptr)
        else ARG2("-process_name",           args.mTargetProcessName   = argv[i])
        else ARG2("-process_id",             args.mTargetPid           = atoi(argv[i]))
        else ARG2("-etl_file",               args.mEtlFileName         = argv[i])

        // Output options
        else ARG1("-no_csv",                 args.mOutputFile          = false)
        else ARG2("-output_file",            args.mOutputFileName      = argv[i])

        // Control and filtering options
        else ARG1("-hotkey",                 args.mHotkeySupport       = true)
        else ARG1("-scroll_toggle",          args.mScrollLockToggle    = true)
        else ARG2("-delay",                  args.mDelay               = atoi(argv[i]))
        else ARG2("-timed",                  args.mTimer               = atoi(argv[i]))
        else ARG1("-exclude_dropped",        args.mExcludeDropped      = true)
        else ARG1("-terminate_on_proc_exit", args.mTerminateOnProcExit = true)
        else ARG1("-simple",                 args.mSimple              = true)
        else ARG1("-dont_restart_as_admin",  tryToElevate              = false)

        // Provided argument wasn't recognized
        else fprintf(stderr, "error: unexpected argument '%s'.\n", argv[i]);

        PrintHelp();
        return 1;
    }

    // Validate command line arguments
    if ((args.mTargetProcessName == nullptr ? 0 : 1) +
        (args.mTargetPid         <= 0       ? 0 : 1) +
        (args.mEtlFileName       == nullptr ? 0 : 1) > 1) {
        fprintf(stderr, "error: only specify one of -captureall, -process_name, -process_id, or -etl_file.\n");
        PrintHelp();
        return 1;
    }

    if (args.mEtlFileName && args.mHotkeySupport) {
        fprintf(stderr, "error: -etl_file and -hotkey arguments are not compatible.\n");
        PrintHelp();
        return 1;
    }

    // Check required privilege
    if (!args.mEtlFileName && !HaveAdministratorPrivileges()) {
        if (tryToElevate) {
            fprintf(stderr, "warning: process requires administrator privilege; attempting to elevate.\n");
            RestartAsAdministrator(argc, argv);
        } else {
            fprintf(stderr, "error: process requires administrator privilege.\n");
        }
        return 2;
    }

    // Set console title to command line arguments
    SetConsoleTitle(argc, argv);

    // Set CTRL handler to properly shut down when user closes process
    SetConsoleCtrlHandler(HandlerRoutine, TRUE);

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

bool HaveAdministratorPrivileges()
{
    static int elevated = -1; // -1 == unknown, 0 == no, 1 == yes
    if (elevated == -1) {
        elevated = 0;

        typedef BOOL(WINAPI *OpenProcessTokenProc)(HANDLE ProcessHandle, DWORD DesiredAccess, PHANDLE TokenHandle);
        typedef BOOL(WINAPI *GetTokenInformationProc)(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass, LPVOID TokenInformation, DWORD TokenInformationLength, DWORD *ReturnLength);
        HMODULE advapi = LoadLibraryA("advapi32");
        if (advapi) {
            OpenProcessTokenProc OpenProcessToken = (OpenProcessTokenProc)GetProcAddress(advapi, "OpenProcessToken");
            GetTokenInformationProc GetTokenInformation = (GetTokenInformationProc)GetProcAddress(advapi, "GetTokenInformation");
            if (OpenProcessToken && GetTokenInformation) {
                HANDLE hToken = NULL;
                if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
                    //TOKEN_ELEVATION is not defined in the vs2003 headers, so this is an easy workaround --
                    // the struct just has a single DWORD member anyways, so we can use DWORD instead of TOKEN_ELEVATION.
                    DWORD dwSize;
                    /*TOKEN_ELEVATION*/ DWORD TokenIsElevated;
                    if (GetTokenInformation(hToken, (TOKEN_INFORMATION_CLASS)20 /*TokenElevation*/, &TokenIsElevated, sizeof(TokenIsElevated), &dwSize)) {
                        elevated = TokenIsElevated;
                    }
                    CloseHandle(hToken);
                }
            }
            FreeLibrary(advapi);
        }
    }

    return elevated == 1;
}

void RestartAsAdministrator(
    int argc,
    char** argv)
{
    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));

    char args[MAX_PATH] = {};
    size_t idx = 0; //snprintf(args, MAX_PATH, "-internal_waitpid %d", GetCurrentProcessId());
    for (int i = 1; i < argc && idx < MAX_PATH; ++i) {
        if (argv[i][0] != '\"' && strchr(argv[i], ' ')) {
            idx += snprintf(args + idx, MAX_PATH - idx, " \"%s\"", argv[i]);
        } else {
            idx += snprintf(args + idx, MAX_PATH - idx, " %s", argv[i]);
        }
    }

    ShellExecuteA(NULL, "runas", exe_path, args, NULL, SW_SHOW);
}

void SetConsoleTitle(
    int argc,
    char** argv)
{
    char args[MAX_PATH] = {};
    size_t idx = snprintf(args, MAX_PATH, "PresentMon");
    for (int i = 1; i < argc && idx < MAX_PATH; ++i) {
        if (argv[i][0] != '\"' && strchr(argv[i], ' ')) {
            idx += snprintf(args + idx, MAX_PATH - idx, " \"%s\"", argv[i]);
        } else {
            idx += snprintf(args + idx, MAX_PATH - idx, " %s", argv[i]);
        }
    }

    SetConsoleTitleA(args);
}
