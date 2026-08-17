# ESP32-S3 Pocket Watch Clock v0.3.0

This is the first actual clock build for the proven
**Waveshare ESP32-S3-Touch-AMOLED-1.75C** hardware.

It uses the confirmed CO5300 466×466 QSPI AMOLED and CST9217 touchscreen,
synchronizes from internet NTP, and displays Central Time with automatic
CST/CDT daylight-saving changes. In family terminology, that is **God's Time**.

There is no separate hardware RTC on this board. The ESP32-S3 keeps time while
it remains powered; after a reset or power loss the firmware reconnects to
Wi-Fi, synchronizes NTP, and then switches Wi-Fi off to reduce power use.

## Configure Wi-Fi

1. In `include`, make a copy of `WatchConfig.example.h`.
2. Rename the copy to `WatchConfig.h`.
3. Edit only the two quoted values:

```cpp
#define WATCH_WIFI_SSID "YOUR_WIFI_NAME"
#define WATCH_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
```

`WatchConfig.h` is excluded by `.gitignore`, so credentials are not committed
if this project later goes into GitHub. The project will still compile without
that file, but the watch will display `CONFIGURE WI-FI` instead of attempting
to connect with placeholder credentials.

## Build, upload, and test

1. Extract the ZIP into a new folder.
2. Open that extracted folder in VS Code.
3. If VS Code says **Restricted Mode**, select **Trust this folder**.
4. Create and edit `include/WatchConfig.h` as described above.
5. Connect the watch with a data-capable USB cable.
6. Close Serial Monitor if it is holding the COM port.
7. Select the PlatformIO checkmark to build.
8. Select the PlatformIO right-arrow to upload.
9. Open Serial Monitor at 115200 baud.

Expected behavior:

- the face initially says `WAITING FOR NTP`;
- it connects to the configured Wi-Fi network;
- time appears in 12-hour Central Time;
- the bottom line reports `NTP SYNCED CDT` or `NTP SYNCED CST`;
- Wi-Fi is then switched off;
- one deliberate tap toggles 12/24-hour display.

Touch input is interrupt-driven and debounced. Repeated CST9217 packets from a
single physical tap are ignored, as are normal empty IRQ packets, so the prior
I2C error flood should not return.

## PowerShell alternative

Run from the extracted project directory:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e pocket_watch
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e pocket_watch -t upload
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" device monitor -b 115200
```

If upload says that the COM port is busy, stop Serial Monitor and retry. Do not
hold BOOT unless ordinary upload fails.

## Serial commands

- `status` prints the current local time and hardware state.
- `sync` reconnects Wi-Fi and repeats NTP synchronization.
- `12` selects 12-hour time.
- `24` selects 24-hour time.
- `help` prints the command list.

## Scope of this checkpoint

This release deliberately concentrates on the watch face, NTP, Central Time,
and clean touch handling. Battery reporting, sleep/wake behavior, stopwatch,
timer, and alarms are the next layers; none are being guessed into this build
before their hardware behavior is tested.
