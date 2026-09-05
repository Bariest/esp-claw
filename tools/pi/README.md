# Raspberry Pi ↔ MP4 ESP32 CORE UART link

The CORE board talks to a Raspberry Pi over a plain 3.3 V UART on the J-PI
expansion header. Firmware side: `components/mpx_pi_link` (console command
`pi`). Pi side: `mp4_link.py` in this folder.

## 1. Software first — no wires needed

**On the ESP** (after `idf.py build flash monitor`):

```
pi loopback      # UART driver + pin config self-test; must say "loopback OK"
pi status        # will say NOT CONNECTED until the Pi is wired and running the script
pi help
```

**On the Pi** (any machine with Python 3 actually):

```
python3 mp4_link.py --selftest    # protocol self-test over a pty pair; must say "selftest OK"
```

## 2. Prepare the Pi 5

SD card: an ordinary microSD is fine — 32 GB or bigger, A1/A2 rated if you
have the choice (SanDisk Extreme / Samsung EVO Plus type). Flash **Raspberry
Pi OS (64-bit), Bookworm** with Raspberry Pi Imager; in its settings pre-fill
hostname, user/password, Wi-Fi and enable SSH so it comes up headless.

Then, on the Pi:

```
sudo raspi-config
    Interface Options -> Serial Port
        Would you like a login shell over serial?   No
        Would you like the serial port hardware enabled?   Yes
sudo apt install -y python3-serial
sudo usermod -aG dialout $USER
sudo reboot
ls -l /dev/ttyAMA0        # must exist after the reboot
```

On the Pi 5 the GPIO14/15 UART shows up as `/dev/ttyAMA0`. (The small
3-pin debug connector next to the HDMI ports is a different UART,
`/dev/ttyAMA10` — not this one.)

## 3. Wires — three of them

| ESP32 CORE (J-PI) | direction | Raspberry Pi 40-pin header |
|---|---|---|
| GPIO18 `PI_1` — ESP TX | → | GPIO15 RXD, **pin 10** |
| GPIO8 `PI_2` — ESP RX | ← | GPIO14 TXD, **pin 8** |
| GND | — | GND, **pin 6** (or 9, 14, 20 …) |

Both boards are 3.3 V logic: **no level shifter**. Do **not** connect 5 V or
3V3 between them — power the Pi from its own 5 V / 5 A USB-C supply and the
CORE from its battery or adapter. Only the ground is shared.

`PI_1`/`PI_2` are the schematic net names (`IO18_PI_1`, `IO8_PI_2`); check
the silkscreen or the J-PI pinout in `MP4ESP32_CORE_SCH_0827.pdf` for which
physical pin is which. If the link stays silent, swapping the two data wires
is the first thing to try — TX/RX crossover is the classic mistake.

Which GPIOs the firmware uses is `CONFIG_MP4_PI_LINK_TX_GPIO` /
`CONFIG_MP4_PI_LINK_RX_GPIO` (menuconfig → App Config → MP4 Robot), defaults
18 / 8. GPIO3 (`PI_3`) was avoided on purpose: it is an ESP32-S3 strapping pin.

## 4. Check it is connected

On the Pi:

```
python3 mp4_link.py --ping        # 5 pings; ">> CONNECTED" or ">> NOT CONNECTED" with hints
python3 mp4_link.py               # interactive: prints what the ESP sends; type `ping`, or any text
```

On the ESP console at the same time:

```
pi status        # sends one PING, prints CONNECTED / NOT CONNECTED and why
pi ping 10       # round-trip times
pi watch 30      # shows anything you type in the Pi's interactive prompt
pi send hello    # shows up as "esp> hello" on the Pi
```

Both ends answer each other's PINGs, so either side can test alone once the
other is running its script/firmware.

## 5. What is on the wire

Newline-terminated ASCII lines: `PING <n>` / `PONG <n>`, `HELLO <name>`, and
anything else passes through and is printed/logged. A real command protocol
goes in `handle_line()` in `mpx_pi_link.c` and `Link._handle()` here once
the link is proven.
