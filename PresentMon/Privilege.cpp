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

namespace {

typedef BOOL(WINAPI *OpenProcessTokenProc)(HANDLE ProcessHandle, DWORD DesiredAccess, PHANDLE TokenHandle);
typedef BOOL(WINAPI *GetTokenInformationProc)(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass, LPVOID TokenInformation, DWORD TokenInformationLength, DWORD *ReturnLength);
typedef BOOL(WINAPI *LookupPrivilegeValueAProc)(LPCSTR lpSystemName, LPCSTR lpName, PLUID lpLuid);
typedef BOOL(WINAPI *AdjustTokenPrivilegesProc)(HANDLE TokenHandle, BOOL DisableAllPrivileges, PTOKEN_PRIVILEGES NewState, DWORD BufferLength, PTOKEN_PRIVILEGES PreviousState, PDWORD ReturnLength);

struct Advapi {
    HMODULE HModule;
    OpenProcessTokenProc OpenProcessToken;
    GetTokenInformationProc GetTokenInformation;
    LookupPrivilegeValueAProc LookupPrivilegeValueA;
    AdjustTokenPrivilegesProc AdjustTokenPrivileges;

    Advapi()
        : HModule(NULL)
    {
    }

    ~Advapi()
    {
        if (HModule != NULL) {
            FreeLibrary(HModule);
        }
    }

    bool Load()
    {
        HModule = LoadLibraryA("advapi32.dll");
        if (HModule == NULL) {
            return false;
        }

        OpenProcessToken = (OpenProcessTokenProc) GetProcAddress(HModule, "OpenProcessToken");
        GetTokenInformation = (GetTokenInformationProc) GetProcAddress(HModule, "GetTokenInformation");
        LookupPrivilegeValueA = (LookupPrivilegeValueAProc) GetProcAddress(HModule, "LookupPrivilegeValueA");
        AdjustTokenPrivileges = (AdjustTokenPrivilegesProc) GetProcAddress(HModule, "AdjustTokenPrivileges");

        if (OpenProcessToken == nullptr ||
            GetTokenInformation == nullptr ||
            LookupPrivilegeValueA == nullptr ||
            AdjustTokenPrivileges == nullptr) {
            FreeLibrary(HModule);
            HModule = NULL;
            return false;
        }

        return true;
    }

    bool HasElevatedPrivilege() const
    {
        HANDLE hToken = NULL;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
            return false;
        }

        /** BEGIN WORKAROUND: struct TOKEN_ELEVATION and enum value TokenElevation
         * are not defined in the vs2003 headers, so we reproduce them here. **/
        enum { WA_TokenElevation = 20 };
        DWORD TokenIsElevated = 0;
        /** END WA **/

        DWORD dwSize = 0;
        if (!GetTokenInformation(hToken, (TOKEN_INFORMATION_CLASS) WA_TokenElevation, &TokenIsElevated, sizeof(TokenIsElevated), &dwSize)) {
            TokenIsElevated = 0;
        }

        CloseHandle(hToken);

        return TokenIsElevated != 0;
    }

    bool EnableDebugPrivilege() const
    {
        HANDLE hToken = NULL;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken)) {
            return false;
        }

        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        bool enabled =
            LookupPrivilegeValueA(NULL, "SeDebugPrivilege", &tp.Privileges[0].Luid) &&
            AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr) &&
            GetLastError() != ERROR_NOT_ALL_ASSIGNED;

        CloseHandle(hToken);

        return enabled;
    }
};

bool RestartAsAdministrator(
    int argc,
    char** argv)
{
    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));

    // Combine arguments into single array
    char args[1024] = {};
    for (int idx = 0, argsSize = (int) sizeof(args), i = 1; i < argc && idx < argsSize; ++i) {
        if (idx >= argsSize) {
            fprintf(stderr, "internal error: command line arguments too long.\n");
            return false; // was truncated
        }

        if (argv[i][0] != '\"' && strchr(argv[i], ' ')) {
            idx += snprintf(args + idx, argsSize - idx, " \"%s\"", argv[i]);
        } else {
            idx += snprintf(args + idx, argsSize - idx, " %s", argv[i]);
        }
    }

#pragma warning(suppress: 4302 4311) // truncate HINSTANCE to int
    auto ret = (int) ShellExecuteA(NULL, "runas", exe_path, args, NULL, SW_SHOW);
    if (ret > 32) {
        return true;
    }
    fprintf(stderr, "warning: failed to elevate privilege");
    switch (ret) {
    case 0:                      fprintf(stderr, " (out of memory)"); break;
    case ERROR_FILE_NOT_FOUND:   fprintf(stderr, " (file not found)"); break;
    case ERROR_PATH_NOT_FOUND:   fprintf(stderr, " (path was not found)"); break;
    case ERROR_BAD_FORMAT:       fprintf(stderr, " (image is invalid)"); break;
    case SE_ERR_ACCESSDENIED:    fprintf(stderr, " (access denied)"); break;
    case SE_ERR_ASSOCINCOMPLETE: fprintf(stderr, " (association is incomplete)"); break;
    case SE_ERR_DDEBUSY:         fprintf(stderr, " (DDE busy)"); break;
    case SE_ERR_DDEFAIL:         fprintf(stderr, " (DDE transaction failed)"); break;
    case SE_ERR_DDETIMEOUT:      fprintf(stderr, " (DDE transaction timed out)"); break;
    case SE_ERR_DLLNOTFOUND:     fprintf(stderr, " (DLL not found)"); break;
    case SE_ERR_NOASSOC:         fprintf(stderr, " (no association)"); break;
    case SE_ERR_OOM:             fprintf(stderr, " (out of memory)"); break;
    case SE_ERR_SHARE:           fprintf(stderr, " (sharing violation)"); break;
    }
    fprintf(stderr, ".\n");

    return false;
}

}

bool ElevatePrivilege(CommandLineArgs const &args, int argc, char** argv)
{
    // If we are processing an ETL file, then we don't need elevated privilege
    if (args.mEtlFileName != nullptr) {
        return true;
    }

    // Otherwise, we will attempt to elevate the privilege as necessary.  On
    // failure, we warn the user but continue to try and capture what we can.
    Advapi advapi;
    if (!advapi.Load()) {
        fprintf(stderr, "warning: unable to detect privilege level. If not running with sufficient\n");
        fprintf(stderr, "         privilege, PresentMon may not capture correctly.\n");
        return true;
    }

    if (!advapi.HasElevatedPrivilege() &&
        args.mTryToElevate &&
        RestartAsAdministrator(argc, argv)) {
        return false;
    }

    // On some versions of Windows, DWM processes run under a separate
    // account.  Try to adjust permissions to get data about a process
    // owned by another account.
    if (!advapi.EnableDebugPrivilege()) {
        fprintf(stderr, "warning: unable to enable debug privilege; PresentMon may not be able to trace all processes.\n");
        return true;
    }

    return true;
}

