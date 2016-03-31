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
```
