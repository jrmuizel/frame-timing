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

#include <string>
#include <windows.h>

std::string FormatString(const char *fmt, ...)
{
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}


void SetConsoleText(const char *text)
{
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    enum { MAX_BUFFER = 16384 };
    char buffer[16384];
    int bufferSize = 0;
    auto write = [&](int ch) {
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
        int ch = *text;
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

bool HaveAdministratorPrivileges(
    void)
{
    static int elevated = -1;

    if (elevated == -1)
    {
        elevated = 0;

        typedef BOOL(WINAPI *OpenProcessTokenProc)(HANDLE ProcessHandle, DWORD DesiredAccess, PHANDLE TokenHandle);
        typedef BOOL(WINAPI *GetTokenInformationProc)(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass, LPVOID TokenInformation, DWORD TokenInformationLength, DWORD *ReturnLength);
        HMODULE advapi = LoadLibraryA("advapi32");
        if (advapi)
        {
            OpenProcessTokenProc OpenProcessToken = (OpenProcessTokenProc)GetProcAddress(advapi, "OpenProcessToken");
            GetTokenInformationProc GetTokenInformation = (GetTokenInformationProc)GetProcAddress(advapi, "GetTokenInformation");
            if (OpenProcessToken && GetTokenInformation)
            {
                HANDLE hToken = NULL;
                if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
                {
                    //TOKEN_ELEVATION is not defined in the vs2003 headers, so this is an easy workaround --
                    // the struct just has a single DWORD member anyways, so we can use DWORD instead of TOKEN_ELEVATION.
                    DWORD dwSize;
                    /*TOKEN_ELEVATION*/ DWORD TokenIsElevated;
                    if (GetTokenInformation(hToken, (TOKEN_INFORMATION_CLASS)20 /*TokenElevation*/, &TokenIsElevated, sizeof(TokenIsElevated), &dwSize))
                    {
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
    int argc, char **argv)
{
    std::string command = FormatString("-waitpid %d", GetCurrentProcessId());
    char exe_path[MAX_PATH];

    for (int i = 0; i < argc; ++i) {
        const char *arg = argv[i];
        if (arg[0] != '\"' && strchr(arg, ' ')) {
            command += FormatString(" \"%s\"", arg);
        } else {
            command += FormatString(" %s", arg);
        }
    }

    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
    if ((HINSTANCE)SE_ERR_ACCESSDENIED != ShellExecuteA(NULL, "runas", exe_path, command.c_str(), NULL, SW_SHOW))
    {
        // success - kill process.
        ExitProcess(0);
    }
}

void WaitForProcess(
    int pid)
{
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!hProcess)
    {
        return;
    }

    WaitForSingleObject(hProcess, INFINITE);
    CloseHandle(hProcess);
}
