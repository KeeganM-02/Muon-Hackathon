# Build and flash `active_sta` (FreeWili OG + Bottlenose Orca, ESP32-C6)

How to get this CSI tool onto the ESP32-C6 that sits on the FreeWili OG, and
how to see the data afterwards. Written from what actually worked on the bench;
every command below was run as-is.

Everything here assumes ESP-IDF **v6.1** installed at `~/.espressif/v6.1` and
the toolchain shim at `~/.espressif/tools/activate_idf_v6.1.sh`.

---

## 0. Ports on this board

The OG enumerates four serial devices. Numbering is not stable across replugs —
prefer the `by-id` links when scripting.

| Device | What it is |
| --- | --- |
| `/dev/ttyACM0` | OG **main** CPU CDC (`usb-FreeWili_OG_FWOG_main_espuart_*`) — mirrors the ESP32 bytes |
| `/dev/ttyACM1` | OG **display** CPU CDC (`usb-FreeWili_OG_FWOG_display_espuart_*`) |
| `/dev/ttyACM2` | ESP32-C6 USB-Serial/JTAG (`usb-Espressif_USB_JTAG_serial_debug_unit_*`) — this is the one you flash |
| `/dev/ttyUSB0` | Intrepid interface (`usb-Intrepid_FreeWili_FW4633-if00-port0`) |

```sh
ls -l /dev/serial/by-id/
```

Your user must be in the `uucp` group to open them:

```sh
sudo usermod -aG uucp "$USER"   # then log out and back in
```

---

## 1. One-time setup

```sh
# Python side of the plotting tools
python3 -m pip install pyserial numpy matplotlib

# ESP-IDF environment (do this in every new shell)
source ~/.espressif/tools/activate_idf_v6.1.sh
```

The project is already pinned to the right target in `sdkconfig`
(`CONFIG_IDF_TARGET="esp32c6"`), so **no `idf.py set-target` step is needed** —
and you should not run one, because `set-target` throws away the console and
partition settings that make CSI reach the FreeWili.

---

## 2. Configure the network

The station config lives in `active_sta/sdkconfig`. It is a normal ESP-IDF
config, so `idf.py menuconfig` works, but these three lines are the whole
story for a simple WPA2 network:

```
CONFIG_ESP_WIFI_SSID="DIRECT-TF-TetherFi"
CONFIG_ESP_WIFI_PASSWORD="..."
CONFIG_ESP_WIFI_ENTERPRISE=n
```

Faster to edit directly, and safe to edit directly:

```sh
cd active_sta
sed -i 's/CONFIG_ESP_WIFI_SSID=.*/CONFIG_ESP_WIFI_SSID="MyNetwork"/' sdkconfig
sed -i 's/CONFIG_ESP_WIFI_PASSWORD=.*/CONFIG_ESP_WIFI_PASSWORD="secret"/' sdkconfig
sed -i 's/CONFIG_ESP_WIFI_ENTERPRISE=.*/CONFIG_ESP_WIFI_ENTERPRISE=n/' sdkconfig
```

**The SSID is case-sensitive.** `DIRECT-TF-TetherFI` is a different network
from `DIRECT-TF-TetherFi`, and the symptom is `reason 201` (NO_AP_FOUND)
forever, which looks exactly like "out of range". If association keeps
failing, the boot-time scan dump lists every 2.4 GHz AP the radio can hear and
flags the wanted SSID with `MATCH`.

### WPA2-Enterprise (eduroam)

Set `CONFIG_ESP_WIFI_ENTERPRISE=y` and put the credentials in
`active_sta/main/eap_credentials.h` (copy it from
`eap_credentials.h.example`), **not** in `sdkconfig` — the header is read at
compile time and the secrets stay out of the config file:

```c
#define EAP_IDENTITY "user@institution.edu"
#define EAP_USERNAME "user@institution.edu"
#define EAP_PASSWORD "..."
```

`eap_credentials.h` is git-ignored. PEAP/MSCHAPv2 is hardcoded, and the
server certificate is **not** verified — fine on a bench, not something to
ship.

Note: the C6 is a **2.4 GHz-only** radio. If the site publishes eduroam on
5 GHz only, no amount of configuration will associate; the scan dump will show
the SSID absent.

### Bench proxy

This bench has no direct route out; everything is proxied through
`192.168.49.1:8228`. Two constants in `_components/sockets_component.h`:

```c
#define PROXY_HOST "192.168.49.1"
#define PROXY_PORT 8228
```

`proxy_http_get()` is a hand-written absolute-URI `GET` over a TCP socket,
because the `esp_http_client` shipped with IDF v6.1 has no
`proxy_host`/`proxy_port` fields. It runs once after the first association and
logs `proxy probe 192.168.49.1 -> HTTP 204`. **204 means internet works.**
Traffic that generates CSI is a UDP probe at the same proxy address rather
than NTP to a public server: a proxied network never answers that query, and
CSI is computed on *received* frames, so a send with no reply produces no data.

### Packet rate

`CONFIG_PACKET_RATE=2` (Hz) sets how often a probe goes out, which bounds the
CSI frame rate. 2 Hz gives ~2 frames/s, enough to see a person move but gappy
for a waterfall. Raise it to 10 for a demo; the link will happily carry it.

---

## 3. Build

```sh
cd active_sta
source ~/.espressif/tools/activate_idf_v6.1.sh
CCACHE_DISABLE=1 idf.py -B build_c6 build
```

Two deliberate details:

* **`-B build_c6`** — the build directory name is the project's own choice, so
  the same tree can hold an ESP32-C6 build next to anything else.
* **`CCACHE_DISABLE=1`** — this cost an afternoon once: ccache served a stale
  object for `main.cc` after a header change and the flashed binary did not
  contain the new code, with a perfectly clean build log. Use it whenever you
  are chasing "my change had no effect".

A good build ends with something like:

```
active-sta.bin binary size 0x111770 bytes. Smallest app partition is 0x177000 bytes. 0x65890 bytes (27%) free.
```

---

## 4. Flash

```sh
CCACHE_DISABLE=1 idf.py -B build_c6 -p /dev/ttyACM2 flash
```

No button, no bootloader dance: the C6's USB-Serial/JTAG handles reset itself.
Expected tail:

```
Hash of data verified.
Hard resetting via RTS pin...
Done
```

`flash` implies `build`, so the two commands can be chained with `&&`. To flash
a specific port when several boards are attached, point `-p` at the `by-id`
link instead of the `ttyACM` number.

---

## 5. Watch the console

```sh
idf.py -B build_c6 -p /dev/ttyACM2 monitor
```

**Careful with the reset lines.** On this chip, asserting DTR and RTS together
is the esptool download strap: the board reboots into the bootloader and you
get a flash dump instead of CSI. `idf.py monitor` normally handles this, but if
you write your own reader, leave both deasserted:

```python
s = serial.Serial('/dev/ttyACM2', 115200, timeout=0.2)
s.dtr = False
s.rts = False        # never True/True: that is download mode
```

What a healthy boot looks like:

```
I (376) wifi:mode : sta (e4:b3:23:a6:06:5c)
I (377) Active CSI collection (Station): connect to ap SSID:DIRECT-TF-TetherFi ...
I (2860) Active CSI collection (Station): scan: 20 APs visible
I (2859) Active CSI collection (Station): MATCH 'DIRECT-TF-TetherFi' ch11 rssi -41 auth 3
I (5xxx) Active CSI collection (Station): Got ip:192.168.49.194
I (6090) sockets: proxy probe 192.168.49.1 -> HTTP 204
I (6091) sockets: sending UDP probes to 192.168.49.1:8228.
CSI_DATA,STA,D2:D3:61:3C:20:2E,-48,11,...
```

The console is also on **UART0, GPIO16 TX / GPIO17 RX at 3 Mbaud**, which is the
link the OG's main CPU reads — so the same `CSI_DATA` stream is available on
`/dev/ttyACM0` for the FreeWili side, at the cost of colliding with your own
reads of `/dev/ttyACM2` (one reader per port at a time).

---

## 6. Plot the data

Live, from the board, locked to one BSSID so the score means something:

```sh
cd python_utils
python3 plot_csi.py /dev/ttyACM2 --mac d2:d3:61:3c:20:2e --calibrate 40
```

* `--mac` filters to one BSSID (substring, case-insensitive). The radio hears
  every AP in earshot across channels 1–11; mixing them makes the subcarrier
  shape jump for reasons that have nothing to do with a person moving.
* `--calibrate N` consumes the first N matching frames as the empty-room
  baseline. At 2 Hz, `40` is 20 seconds of standing still — and the room really
  does need to be empty, or the baseline learns *you*.
* `--save out.png` swaps the live window for a 15-second capture and writes a
  PNG instead of opening a window (and forces the Agg backend, so it works over
  SSH).

Replay a saved capture:

```sh
python3 plot_csi.py --file ../work/csi-live.log --calibrate 12 --mac d2:d3 --save csi.png
```

---

## 7. Troubleshooting

| Symptom | Cause |
| --- | --- |
| `assert failed: xTaskCreatePinnedToCore ... xCoreID < 1` | Task pinned to core 1 on a unicore C6. Use `portNUM_PROCESSORS - 1`. |
| `assert failed: prvInitialiseNewTask ... uxPriority < 25` | Priority above `configMAX_PRIORITIES`. Use ≤ 24 (this tree uses 5). |
| Reboots every ~2 s with a register dump | Not a hang — an assert. Read the line just before `ELF file SHA256`. |
| `Retry connecting to the AP (reason 201)` | NO_AP_FOUND: wrong SSID *case*, or the AP is 5 GHz only, or out of range. Read the scan dump. |
| `reason 205` instead | Association reached the AP and failed: wrong password, or the AP is WPA3-only against `WPA2_PSK` threshold. |
| `CSI_DATA` lines but no WiFi | Expected while unassociated — the radio still reports frames from neighbouring APs during scans. |
| Build succeeds, binary behaves like the old code | ccache. Rebuild with `CCACHE_DISABLE=1`. |
| `Could not open /dev/ttyACM2, the port is busy` | Another monitor/plot still holds it. One reader per port. |
| Scan says `STA is connecting, scan are not allowed` | A scan was attempted while a connect was in flight; the event handler is gated on `s_connect_enabled` for exactly this reason. |
