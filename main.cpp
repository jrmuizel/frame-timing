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

#include "CommandLine.hpp"
#include "PresentMon.hpp"

bool g_StopRecording = false;

namespace {

const uint32_t c_Hotkey = 0x80;

HWND g_hWnd = 0;

std::mutex g_RecordingMutex;
std::thread g_RecordingThread;
bool g_IsRecording = false;

void LockedStartRecording(CommandLineArgs& args)
{
    assert(g_IsRecording == false);
    g_StopRecording = false;
    g_RecordingThread = std::thread(PresentMonEtw, args);
    g_IsRecording = true;

    if (args.mSimpleConsole) {
        printf("Started recording.\n");
    }
}

void LockedStopRecording(CommandLineArgs const& args)
{
    if (args.mSimpleConsole) {
        printf("Stopping recording.\n");
    }

    g_StopRecording = true;
    if (g_RecordingThread.joinable()) {
        g_RecordingThread.join();
    }
    g_IsRecording = false;
}

void StartRecording(CommandLineArgs& args)
{
    std::lock_guard<std::mutex> lock(g_RecordingMutex);
    LockedStartRecording(args);
}

void StopRecording(CommandLineArgs const& args)
{
    std::lock_guard<std::mutex> lock(g_RecordingMutex);
    if (g_IsRecording) {
        LockedStopRecording(args);
    }
}

BOOL WINAPI ConsoleCtrlHandler(
    _In_ DWORD dwCtrlType
    )
{
    QuitPresentMon();
    return TRUE;
}

LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_HOTKEY && wParam == c_Hotkey) {
        auto& args = *reinterpret_cast<CommandLineArgs*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

        std::lock_guard<std::mutex> lock(g_RecordingMutex);
        if (g_IsRecording) {
            LockedStopRecording(args);
            args.mRestartCount++;
        } else {
            LockedStartRecording(args);
        }
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

HWND CreateMessageQueue(CommandLineArgs& args)
{
    WNDCLASSEXW Class = { sizeof(Class) };
    Class.lpfnWndProc = WindowProc;
    Class.lpszClassName = L"PresentMon";
    if (!RegisterClassExW(&Class)) {
        fprintf(stderr, "error: failed to register hotkey class.\n");
        return 0;
    }

    HWND hWnd = CreateWindowExW(0, Class.lpszClassName, L"PresentMonWnd", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, nullptr);
    if (!hWnd) {
        fprintf(stderr, "error: failed to create hotkey window.\n");
        return 0;
    }

    if (args.mHotkeySupport) {
        if (!RegisterHotKey(hWnd, c_Hotkey, MOD_NOREPEAT, VK_F11)) {
            fprintf(stderr, "error: failed to register hotkey.\n");
            DestroyWindow(hWnd);
            return 0;
        }
    }

    SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&args));

    return hWnd;
}

bool HaveAdministratorPrivileges()
{
    enum {
        PRIVILEGE_UNKNOWN,
        PRIVILEGE_ELEVATED,
        PRIVILEGE_NOT_ELEVATED,
    } static privilege = PRIVILEGE_UNKNOWN;

    if (privilege == PRIVILEGE_UNKNOWN) {

        typedef BOOL(WINAPI *OpenProcessTokenProc)(HANDLE ProcessHandle, DWORD DesiredAccess, PHANDLE TokenHandle);
        typedef BOOL(WINAPI *GetTokenInformationProc)(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass, LPVOID TokenInformation, DWORD TokenInformationLength, DWORD *ReturnLength);
        HMODULE advapi = LoadLibraryA("advapi32");
        if (advapi) {
            OpenProcessTokenProc OpenProcessToken = (OpenProcessTokenProc)GetProcAddress(advapi, "OpenProcessToken");
            GetTokenInformationProc GetTokenInformation = (GetTokenInformationProc)GetProcAddress(advapi, "GetTokenInformation");
            if (OpenProcessToken && GetTokenInformation) {
                HANDLE hToken = NULL;
                if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
                    /** BEGIN WORKAROUND: struct TOKEN_ELEVATION and enum value
                     * TokenElevation are not defined in the vs2003 headers, so
                     * we reproduce them here. **/
                    enum { WA_TokenElevation = 20 };
                    struct {
                        DWORD TokenIsElevated;
                    } token = {};
                    /** END WA **/

                    DWORD dwSize = 0;
                    if (GetTokenInformation(hToken, (TOKEN_INFORMATION_CLASS) WA_TokenElevation, &token, sizeof(token), &dwSize)) {
                        privilege = token.TokenIsElevated ? PRIVILEGE_ELEVATED : PRIVILEGE_NOT_ELEVATED;
                    }

                    CloseHandle(hToken);
                }
            }
            FreeLibrary(advapi);
        }
    }

    return privilege == PRIVILEGE_ELEVATED;
}

}

void QuitPresentMon()
{
    g_StopRecording = true;
    PostMessage(g_hWnd, WM_QUIT, 0, 0);
}

int main(int argc, char** argv)
{
    // Parse command line arguments
    CommandLineArgs args;
    if (!ParseCommandLine(argc, argv, &args)) {
        return 1;
    }

    // Check required privilege
    if (!args.mEtlFileName && !HaveAdministratorPrivileges()) {
        if (args.mTryToElevate) {
            fprintf(stderr, "warning: process requires administrator privilege; attempting to elevate.\n");
            if (!RestartAsAdministrator(argc, argv)) {
                fprintf(stderr, "error: failed to elevate privilege.\n");
                return 1;
            }
        } else {
            fprintf(stderr, "error: process requires administrator privilege.\n");
        }
        return 2;
    }

    // Set console title to command line arguments
    SetConsoleTitle(argc, argv);

    // Create a message queue to handle WM_HOTKEY and WM_QUIT messages.
    HWND hWnd = CreateMessageQueue(args);
    if (hWnd == 0) {
        return 3;
    }
    g_hWnd = hWnd;  // Store the hWnd in a global for the CTRL handler to use.
                    // This must be stored before setting the handler.

    // Set CTRL handler to capture when the user tries to close the process by
    // closing the console window or CTRL-C or similar.  The handler will
    // ignore this and instead post WM_QUIT to our message queue.
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // Now that everything is running we can start recording on the recording
    // thread.  If we're using -hotkey then we don't start recording until the
    // user presses the hotkey (F11).
    if (!args.mHotkeySupport)
    {
        StartRecording(args);
    }

    // Start the message queue loop, which is the main mechanism for starting
    // and stopping the recording from the hotkey as well as terminating the
    // process.
    //
    // This thread will block waiting for any messages.
    for (MSG message = {}; GetMessageW(&message, hWnd, 0, 0); ) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    // Wait for tracing to finish, to ensure the PM thread closes the session
    // correctly Prevent races on joining the PM thread between the control
    // handler and the main thread
    StopRecording(args);

    return 0;
}
