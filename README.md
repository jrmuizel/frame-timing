# PresentMon

ETW based FPS monitor for DX10+ programs, including Windows Store Apps / UWP Apps.

### command line options:

```html
 -captureall: record ALL processes (default).
 -process_name [exe name]: record specific process.
 -process_id [integer]: record specific process ID.
 -output_file [path]: override the default output path.
 -etl_file [path]: consume events from an ETL file instead of real-time.
 -delay [seconds]: wait before starting to consume events.
 -timed [seconds]: stop listening and exit after a set amount of time.
 -no_csv: do not create any output file.
 -exclude_dropped: exclude dropped presents from the csv output.
 -scroll_toggle: only record events while scroll lock is enabled.
 -simple: disable advanced tracking. try this if you encounter crashes.
 -terminate_on_proc_exit: terminate PresentMon when all instances of the specified process exit.
 -hotkey: use F11 to start and stop listening, writing to a unique file each time.
          delay kicks in after hotkey (each time), timer starts ticking from hotkey press.
```

## License

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
