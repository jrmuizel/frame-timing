# PresentMon

PresentMon is a tool to capture and analyze
[ETW](https://msdn.microsoft.com/en-us/library/windows/desktop/bb968803%28v=vs.85%29.aspx?f=255&MSPPError=-2147217396)
events related to swap chain presentation on Windows.  It can be used to
trace key performance metrics for graphics applications (e.g.,
CPU and Display frame durations and latencies) and works across all graphics
APIs, including UWP applications.

While PresentMon itself is focused on lightweight collection and analysis,
there are several other programs that build on its functionality and/or helps
visualize the resulting data.  For example, see
- [presentmon-graph](https://github.com/PaulPiatek/presentmon-graph)
- [CapFrameX](https://github.com/DevTechProfile/CapFrameX)
- [OCAT](https://github.com/GPUOpen-Tools/OCAT)
- [FrameView](https://www.nvidia.com/en-us/geforce/technologies/frameview/)

## License

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

## Releases

Binaries for main release versions of PresentMon are provided on GitHub:
- [Latest release](https://github.com/GameTechDev/PresentMon/releases/latest)
- [List of all releases](https://github.com/GameTechDev/PresentMon/releases)

Please see
[CONTRIBUTING](https://github.com/GameTechDev/PresentMon/blob/master/CONTRIBUTING.md)
for information on how to request features, report issues, or contribute code
changes.

## Command line options

#### Capture target options

```
Capture target options:
  -captureall               Record all processes (default).
  -process_name [exe name]  Record only processes with the provided name. This
                            argument can be repeated to capture multiple
                            processes.
  -exclude [exe name]       Don't record specific process specified by name.
                            This argument can be repeated to exclude multiple
                            processes.
  -process_id [integer]     Record only the process specified by ID.
  -etl_file [path]          Consume events from an ETL file instead of running
                            processes.

Output options (see README for file naming defaults):
  -output_file [path]       Write CSV output to specified path.
  -output_stdout            Write CSV output to STDOUT.
  -multi_csv                Create a separate CSV file for each captured
                            process.
  -no_csv                   Do not create any output file.
  -no_top                   Don't display active swap chains in the console
                            window.

Recording options:
  -hotkey [key]             Use specified key to start and stop recording,
                            writing to a unique CSV file each time. 'key' is of
                            the form MODIFIER+KEY, e.g., alt+shift+f11. (See
                            README for subsequent file naming).
  -delay [seconds]          Wait for specified time before starting to record.
                            If using -hotkey, delay occurs each time recording
                            is started.
  -timed [seconds]          Stop recording after the specified amount of time.
  -exclude_dropped          Exclude dropped presents from the csv output.
  -scroll_indicator         Enable scroll lock while recording.
  -simple                   Disable GPU/display tracking.
  -verbose                  Adds additional data to output not relevant to
                            normal usage.

Execution options:
  -session_name [name]      Use the specified name to start a new realtime ETW
                            session, instead of the default "PresentMon". This
                            can be used to start multiple realtime capture
                            process at the same time (using distinct names). A
                            realtime PresentMon capture cannot start if there
                            are any existing sessions with the same name.
  -stop_existing_session    If a trace session with the same name is already
                            running, stop the existing session (to allow this
                            one to proceed).
  -dont_restart_as_admin    Don't try to elevate privilege.  Elevated privilege
                            isn't required to trace a process you started, but
                            PresentMon requires elevated privilege in order to
                            query processes started on another account. Without
                            it, these processes cannot be targetted by name and
                            will be listed as '<error>', and if they are
                            targetted -terminate_on_proc_exit won't work and
                            there may be tracking errors near process
                            termination.
  -terminate_on_proc_exit   Terminate PresentMon when all the target processes
                            have exited.
  -terminate_after_timed    When using -timed, terminate PresentMon after the
                            timed capture completes.

Beta options:
  -include_mixed_reality    Capture Windows Mixed Reality data to a CSV file
                            with "_WMR" suffix.
```

## Comma-separated value (CSV) file output

### CSV file names

By default, PresentMon creates a CSV file named `PresentMon-TIME.csv`, where
`TIME` is the creation time in ISO 8601 format.  To specify your own output
location, use the `-output_file PATH` command line argument.

If `-multi_csv` is used, then one CSV is created for each process captured and
`-PROCESSNAME` appended to the file name.

If `-hotkey` is used, then one CSV is created for each time recording is started
and `-INDEX` appended to the file name.

### CSV columns

| Column Header | Data Description | Required argument |
|---|---|---|
| Application            | The name of the process that called Present (if known) |
| ProcessID              | The process ID of the process that called Present |
| SwapChainAddress       | The address of the swap chain that was presented into |
| Runtime                | The runtime used to present (e.g., D3D9 or DXGI) |
| SyncInterval           | Sync interval used in the Present call |
| PresentFlags           | Flags used in the Present call |
| PresentMode            | The presentation mode used by the system for this Present | not `-simple` |
| AllowsTearing          | Whether tearing is possible (1) or not (0) | not `-simple` |
| TimeInSeconds          | The time of the Present call, measured from when PresentMon recording started in seconds | |
| MsInPresentAPI         | The time spent inside the Present call, in milliseconds |
| MsUntilRenderComplete  | The time between the Present call (TimeInSeconds) and when the GPU work completed, in milliseconds | not `-simple` |
| MsUntilDisplayed       | The time between the Present call (TimeInSeconds) and when the frame was displayed, in milliseconds | not `-simple` |
| Dropped                | Whether the frame was dropped (1) or displayed (0); if dropped, MsUntilDisplayed will be 0 |
| MsBetweenPresents      | The time between this Present call and the previous one, in milliseconds |
| MsBetweenDisplayChange | The time between when the previous frame was displayed and this frame was, in milliseconds | not `-simple` |
| WasBatched             | Whether the frame was submitted by the driver on a different thread than the app (1) or not (0) | `-verbose` |
| DwmNotified            | Whether the desktop compositor was notified about the frame (1) or not (0) | `-verbose` |

PresentMon doesn't directly measure the latency from a user's input to the
display of that frame because it doesn't have insight into when the application
collects and applies user input.  A potential approximation is to assume that
the application collects user input immediately after presenting the previous
frame.  To compute this, search for the previous row that uses the same swap
chain and then:

```LatencyMs =~ 1000 * MsBetweenPresents + MsUntilDisplayed - previous(MsInPresentAPI)```

## Windows Mixed Reality CSV file output

*Note: Windows Mixed Reality support is in beta, with limited OS support.*

If `-include_mixed_reality` is used, a second CSV file will be generated with
`_WMR` appended to the filename with the following columns:

| Column Header | Data Description | Required argument |
|---|---|---|
| Application                                  | Process name (if known) | `-include_mixed_reality` |
| ProcessID                                    | Process ID | `-include_mixed_reality` |
| DwmProcessID                                 | Compositor Process ID | `-include_mixed_reality` |
| TimeInSeconds                                | Time since PresentMon recording started | `-include_mixed_reality` |
| MsBetweenLsrs                                | Time between this Lsr CPU start and the previous one | `-include_mixed_reality` |
| AppMissed                                    | Whether Lsr is reprojecting a new (0) or old (1) App frame (App GPU work must complete before Lsr CPU start) | `-include_mixed_reality` |
| LsrMissed                                    | Whether Lsr displayed a new frame (0) or not (1+) at the intended V-Sync (Count V-Syncs with no display change) | `-include_mixed_reality` |
| MsAppPoseLatency                             | Time between App's pose sample and the intended mid-photon frame display | `-include_mixed_reality` |
| MsLsrPoseLatency                             | Time between Lsr's pose sample and the intended mid-photon frame display | `-include_mixed_reality` |
| MsActualLsrPoseLatency                       | Time between Lsr's pose sample and mid-photon frame display | `-include_mixed_reality` |
| MsTimeUntilVsync                             | Time between Lsr CPU start and the intended V-Sync | `-include_mixed_reality` |
| MsLsrThreadWakeupToGpuEnd                    | Time between Lsr CPU start and GPU work completion | `-include_mixed_reality` |
| MsLsrThreadWakeupError                       | Time between intended Lsr CPU start and Lsr CPU start | `-include_mixed_reality` |
| MsLsrPreemption                              | Time spent preempting the GPU with Lsr GPU work | `-include_mixed_reality` |
| MsLsrExecution                               | Time spent executing the Lsr GPU work | `-include_mixed_reality` |
| MsCopyPreemption                             | Time spent preempting the GPU with Lsr GPU cross-adapter copy work (if required) | `-include_mixed_reality` |
| MsCopyExecution                              | Time spent executing the Lsr GPU cross-adapter copy work (if required) | `-include_mixed_reality` |
| MsGpuEndToVsync                              | Time between Lsr GPU work completion and V-Sync | `-include_mixed_reality` |
| MsBetweenAppPresents                         | Time between App's present and the previous one | `-include_mixed_reality` and not `-simple` |
| MsAppPresentToLsr                            | Time between App's present and Lsr CPU start | `-include_mixed_reality` and not `-simple` |
| HolographicFrameID                           | App's Holographic Frame ID | `-include_mixed_reality` `-verbose` |
| MsSourceReleaseFromRenderingToLsrAcquire     | Time between composition end and Lsr acquire | `-include_mixed_reality` `-verbose` |
| MsAppCpuRenderFrame                          | Time between App's CreateNextFrame() API call and PresentWithCurrentPrediction() API call | `-include_mixed_reality` `-verbose` |
| MsAppMisprediction                           | Time between App's intended pose time and the intended mid-photon frame display | `-include_mixed_reality` `-verbose` |
| MsLsrCpuRenderFrame                          | Time between Lsr CPU render start and GPU work submit | `-include_mixed_reality` `-verbose` |
| MsLsrThreadWakeupToCpuRenderFrameStart       | Time between Lsr CPU start and CPU render start | `-include_mixed_reality` `-verbose` |
| MsCpuRenderFrameStartToHeadPoseCallbackStart | Time between Lsr CPU render start and pose sample | `-include_mixed_reality` `-verbose` |
| MsGetHeadPose                                | Time between Lsr pose sample start and pose sample end | `-include_mixed_reality` `-verbose` |
| MsHeadPoseCallbackStopToInputLatch           | Time between Lsr pose sample end and input latch | `-include_mixed_reality` `-verbose` |
| MsInputLatchToGpuSubmission                  | Time between Lsr input latch and GPU work submit | `-include_mixed_reality` `-verbose` |

