# `esp32-roomnode` — the C6 network side of the room node

Standalone ESP-IDF project for the ESP32-C6 on the Bottlenose Orca. It owns the
network and nothing else: it publishes what the main RP2040 senses and forwards
what the backend commands.

Design and the cross-CPU contract: [`../docs/roomnode.md`](../docs/roomnode.md).

## What it does

- Wi-Fi station, retrying forever.
- **Link to the main CPU** on UART0 (GPIO16 TX / GPIO17 RX, 115200 baud,
  JSON lines — see `roomnode_config.h`).
- **Polls `GET {API_BASE}/device/rooms/1`** every `POLL_MS`, forwarding
  `state`, `url`, `expires_at`, and `next_res_start` to the main CPU. A `servo`
  target or `alert` banner is also forwarded if the backend supplies one.
- **Reports observed presence** as a boolean on each transition and every 60
  seconds. The service, not the device, decides what that observation means.
- **Wall clock**: the `Date:` header of any response becomes an epoch the main
  CPU can use, because neither RP2040 has a trustworthy clock.

Console is on **USB Serial/JTAG** (the ESP32's own USB port), deliberately not
on UART0 — UART0 is the 115200-baud protocol wire, and one log line on it would
corrupt a JSON message.

## Configure

Everything lives in `main/roomnode_config.h`:

| Define | Meaning |
| --- | --- |
| `WIFI_SSID` / `WIFI_PASSWORD` | station credentials |
| `USE_PROXY` | 1 = route everything through `PROXY_HOST:PROXY_PORT` (bench default), 0 = direct |
| `API_BASE` | HTTPS API gateway base URL |
| `DEVICE_KEY` | value sent in `x-device-key` |
| `ROOM_ID` | `"1"` |
| `POLL_MS` | room-state poll interval |

### The proxy

`USE_PROXY 1` is the **bench default**: this network has no direct route out.
`net.c` opens an HTTP `CONNECT` tunnel to the proxy and then performs normal TLS
to the API host. The proxy also resolves the hostname because the tether does
not provide a working resolver.

Set `USE_PROXY 0` on a network with direct DNS and HTTPS egress.

### Device credential

The room fixture authenticates with `x-device-key`, not a Cognito user token.
The development default is replaceable from the ignored
`main/roomnode_secrets.h`; production provisioning and rotation remain an API
team decision.

## Build

```sh
source ~/.espressif/tools/activate_idf_v6.1.sh
cd esp32-roomnode
CCACHE_DISABLE=1 idf.py -B build set-target esp32c6   # once
CCACHE_DISABLE=1 idf.py -B build build
```

`CCACHE_DISABLE=1` is not superstition: ccache has already served a stale
object for a changed source file once in this project, and the flashed binary
simply did not contain the change, with a clean build log.

## Flash

```sh
CCACHE_DISABLE=1 idf.py -B build -p /dev/ttyACM2 flash
idf.py -B build -p /dev/ttyACM2 monitor
```

`/dev/ttyACM2` is the C6's own USB-Serial/JTAG port (`usb-Espressif_USB_JTAG_*`
in `/dev/serial/by-id/`). No button, no bootloader dance.

**Do not let anything assert both DTR and RTS on this port** — on the C6's
USB-Serial/JTAG that is the esptool download strap, and the board reboots into
the bootloader instead of running. A raw pyserial reader must set
`s.dtr = False; s.rts = False`.

A healthy boot:

```
I (312) roomnode: fwog room node: room 1, qr https://main.d24j7edplzktpl.amplifyapp.com/r/1
I (312) roomnode: api http://api.example.invalid/v1 (via proxy 192.168.49.1)
I (318) link: link up: uart0 tx=16 rx=17 115200 baud
I (560) wifi: got ip:192.168.49.194
I (610) link: main CPU booted: roomnode-main
W (2600) api: GET .../rooms/1 -> 401 PLACEHOLDER-COGNITO-ID-TOKEN
```

The 401 is the token placeholder doing what it says on the tin.

## Tests

The JSON reader (`main/jhelp.c`) is plain C with no IDF headers, so it is
tested from the host tree instead of by flashing:

```sh
cd .. && python3 tools/fw.py test        # 41 CTest binaries + 197 fw.py tests
```

`tests/test_jhelp.c` asserts the real `GET /rooms/1` body parses, that a key
appearing only as another key's *value* does not match, that a nested `alert`
object neither leaks into the top level nor gets lost, and that truncated
buffers and malformed input fail instead of overrunning.

Nothing in this project has been observed on hardware yet: the board dropped
off USB during bring-up and had not re-enumerated when this was written. Build
verified; behaviour unverified.
