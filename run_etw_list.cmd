@echo off
setlocal
set etw_list=%~1
if not exist "%etw_list%" (
    echo usage: run_etw_list.cmd path_to_etw_list_exe
    exit /b 1
)

set year=
for /f "tokens=1 delims=/-." %%a in ('date /t') do set year=%%a

set version=
for /f "tokens=*" %%a in ('%etw_list% --version') do set version=%%a

set events=
set events=%events% --event=Present::Start
set events=%events% --event=Present::Stop
call :etw_list "*d3d9" "%~dp0PresentData\D3d9EventStructs.hpp"

set events=
set events=%events% --event=MILEVENT_MEDIA_UCE_PROCESSPRESENTHISTORY_GetPresentHistory::Info
set events=%events% --event=SCHEDULE_PRESENT::Start
set events=%events% --event=SCHEDULE_SURFACEUPDATE::Info
call :etw_list "*dwm-core" "%~dp0PresentData\DwmEventStructs.hpp"

set events=
set events=%events% --event=Present::Start
set events=%events% --event=Present::Stop
set events=%events% --event=PresentMultiplaneOverlay::Start
set events=%events% --event=PresentMultiplaneOverlay::Stop
call :etw_list "*dxgi" "%~dp0PresentData\DxgiEventStructs.hpp"

set events=
set events=%events% --event=Blit::Info
set events=%events% --event=Flip::Info
set events=%events% --event=FlipMultiPlaneOverlay::Info
set events=%events% --event=HSyncDPCMultiPlane::Info
set events=%events% --event=MMIOFlip::Info
set events=%events% --event=MMIOFlipMultiPlaneOverlay::Info
set events=%events% --event=Present::Info
set events=%events% --event=PresentHistory::Start
set events=%events% --event=PresentHistory::Info
set events=%events% --event=PresentHistoryDetailed::Start
set events=%events% --event=QueuePacket::Start
set events=%events% --event=QueuePacket::Stop
set events=%events% --event=VSyncDPC::Info
call :etw_list "*dxgkrnl" "%~dp0PresentData\DxgKrnlEventStructs.hpp"

set events=
set events=%events% --event=TokenCompositionSurfaceObject::Info
set events=%events% --event=TokenStateChanged::Info
call :etw_list "*win32k" "%~dp0PresentData\Win32kEventStructs.hpp"

set events=
call :etw_list "{3d6fa8d0-fe05-11d0-9dda-00c04fd7ba7c}" "%~dp0PresentData\NTProcessEventStructs.hpp"

exit /b 0

:etw_list
    echo /*> %2
    echo Copyright %year% Intel Corporation>> %2
    echo.>>%2
    echo Permission is hereby granted, free of charge, to any person obtaining a copy of>>%2
    echo this software and associated documentation files (the "Software"), to deal in>>%2
    echo the Software without restriction, including without limitation the rights to>>%2
    echo use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies>>%2
    echo of the Software, and to permit persons to whom the Software is furnished to do>>%2
    echo so, subject to the following conditions:>>%2
    echo.>>%2
    echo The above copyright notice and this permission notice shall be included in all>>%2
    echo copies or substantial portions of the Software.>>%2
    echo.>>%2
    echo THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR>>%2
    echo IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,>>%2
    echo FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE>>%2
    echo AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER>>%2
    echo LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,>>%2
    echo OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE>>%2
    echo SOFTWARE.>>%2
    echo */>>%2
    echo.>>%2
    %etw_list% --show=all --output=c++ %events% --provider=%~1>>%2
    exit /b 0

