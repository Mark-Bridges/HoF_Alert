# EA HOF Monitor (ESP32)

A £4-class device that watches one (optionally two) Environment Agency gauging
station(s) named in an abstraction licence and pushes phone alerts when flow or
level approaches or crosses a Hands-Off Flow / Hands-Off Level condition.

## How it works

- The ESP32 polls the EA real-time API every 15 minutes (the data itself only
  updates ~every 15 min) with a single tiny request per site:
  `/id/measures/{measureId}/readings?latest&_limit=1`, parsed with an
  ArduinoJson filter so RAM use is small and fixed.
- Setup is done in a browser. The ESP32 serves the config page, but the
  **browser** performs the station search against the EA API directly (the API
  allows cross-origin requests), so the device never handles large responses.
- Thresholds are entered in **licence units** (litres/second for flow, metres
  for level) and converted to the gauge's native units (m³/s) for comparison.
- Notifications go via [ntfy.sh](https://ntfy.sh): warning (approaching HOF),
  trigger (HOF crossed — urgent), recovery, gauge-data-stale, and data-resumed.
- Alert logic has hysteresis: after a trigger, the device stays in TRIGGERED
  until the value recovers past the *warning* level, so a river hovering at the
  threshold doesn't ping you every 15 minutes. The warning margin doubles as
  the re-arm deadband.
- Alert state is stored in NVS, so a reboot mid-event doesn't re-fire or lose
  the alarm. After 8 consecutive failed polls (~2 h) the device restarts itself.

## Hardware

Any standard ESP32 dev board (ESP32-WROOM/WROVER, ESP32-C3, etc.), USB power.
No other components required. Optional later additions: buzzer/LED on a GPIO
for an on-site alarm, or a small e-paper display.

If you want a simple on-device status light, wire three external LEDs through
330-1k resistors to the GPIOs used in `HoF_Alert.ino`:

- green = `LED_OK_PIN`
- amber = `LED_WARN_PIN`
- red = `LED_ALERT_PIN`

Each LED is active HIGH. Change the pin numbers in the sketch if your board
uses those GPIOs for something else.

Tip: avoid strapping pins for indicator LEDs where possible. On many ESP32
boards GPIO15 is a boot strap pin, so prefer non-strap outputs such as
GPIO16, GPIO17, GPIO18, GPIO19, GPIO21, GPIO22, GPIO23, GPIO25-27, GPIO32-33.

## Build & flash

1. Arduino IDE (or arduino-cli) with the **Arduino-ESP32 core 3.x** installed.
2. Library Manager → install:
   - **ArduinoJson** (Benoit Blanchon), v7.x
   - **WiFiManager** (tzapu), v2.x
3. Open `ea_hof_monitor.ino`, select your board, flash.

## First run

1. The device broadcasts a Wi-Fi network **EA-HOF-Setup**. Join it and the
   captive portal will ask for your home/site Wi-Fi credentials.
2. Once connected, browse to [http://ea-hof.local](http://ea-hof.local) (or the IP printed on the
   Serial monitor at 115200 baud).
3. Install the **ntfy** app on your phone, subscribe to a topic of your choice
   (pick something unguessable), enter the same topic on the setup page and
   press **Send test alert** to confirm the path works end-to-end.
4. Search for the gauging station named in your licence, choose the exact
   measure (flow vs level — stations often publish several), and check the
   live reading shown matches expectations for that site.
5. Enter the licence trigger value in the units the licence uses
   (e.g. `250 l/s`), pick a warning margin, and **Save & start monitoring**.
   The first poll runs immediately and the status board shows
   armed / warned / TRIGGERED per site.

If you later move the device to a place where the saved Wi-Fi is not
available, it will fall back to the **EA-HOF-Setup** captive portal on boot so
you can enter new Wi-Fi credentials. The saved station and notification
settings remain intact, so no separate reset or re-register flow is needed for
normal use.

## Web UI navigation

- **Monitor** is the landing view once a valid configuration is saved.
- **Settings** contains notifications and station setup.
- If settings are incomplete (no topic or station), the UI opens on
  **Settings** automatically.

Wi-Fi configuration is separate from the station/alert setup. If the device
cannot reach the previously saved network, it starts the Wi-Fi setup portal
again without clearing the monitoring configuration.

The Monitor view now includes expanded per-station trend cards for:

- **7 day trend** (change, percent change, and range)
- **30 day trend** (change, percent change, and range)

It also shows the configured HoF settings in plain view:

- A monitor overview line listing each station trigger and direction
  (falls below / rises above).
- A per-station line showing HoF trigger and warning threshold values.

These are fetched directly from the EA API reading history in the browser,
so the device does not store long-term history itself.

Trend cards and sparkline are shown in licence units for consistency with the
rest of the Monitor panel.

## Trend language

Trend labels are deliberately coarse and tuned for the cadence of EA readings.
They are based on the change between the last two successful polls and group
movement into practical bands rather than raw arithmetic detail:

- `steady`: the reading has barely moved.
- `nudging up` / `nudging down`: a small change.
- `moving up` / `moving down`: a clear change, but not yet aggressive.
- `rising briskly` / `falling briskly`: a material shift.
- `rising fast` / `falling fast`: a large change that deserves attention.

The UI shows both the band label and a short rate note so you can see whether
the gauge is drifting, moving, or shifting quickly without overloading the
display with numbers.

In the trend cards, **Period range (lowest to highest)** means exactly that:
the minimum and maximum readings seen over that period.

In the 30-day sparkline, the chart direction is chronological:

- left = oldest reading
- right = latest reading

The sparkline also includes threshold context:

- green segments = safe side of trigger
- red segments = alert side of trigger
- dashed line = trigger level

To change the station later, open the page and run a new search — saving
overwrites the previous configuration and resets alert states.

## Second station

Firmware and config schema support two sites (each with independent alert
state and its own notifications). The setup page keeps the second slot behind
an "Add a second station" link since most licences reference a single gauge.

## Notes & caveats

- **TLS**: requests use `setInsecure()` (no certificate validation) for
  simplicity on public read-only data. For a hardened build, embed the root CA
  and use `setCACert()` in `fetchLatest()` / `sendNtfy()`.
- **Telemetry cadence**: gauges *measure* every 15 minutes, but some stations
  only transmit data back to the EA a few times a day — and transmission
  frequency rises during flood risk, i.e. it may be *lowest* in exactly the
  dry conditions that matter for HOF. The stale-data threshold defaults to
  3 hours (`STALE_AFTER_SECS`); watch your station's actual update pattern for
  a week and set it just above the normal gap.
- **Provisional data**: EA real-time readings are unvalidated. The device is
  an early-warning aid; the licence and the EA's own determination remain the
  authority on when abstraction must stop. The stale-data alert exists because
  gauges do go offline — treat "no data" as seriously as a trigger.
- **Battery operation**: as written the device assumes mains/USB power. For
  battery use, replace the `loop()` timing with deep sleep between polls and
  keep alert state in NVS (already done) — wake, poll, sleep.
- **ntfy privacy**: anyone who knows your topic name can subscribe to it.
  Use a random suffix, or self-host ntfy / use its auth features if needed.
