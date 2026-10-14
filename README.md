# ps1-pico-video

Video playback on an original PlayStation (PS1 / PSone), streamed from an SD card through **controller port 2** by a Raspberry Pi Pico 2.

[![video that explains the details](https://img.youtube.com/vi/VIDEO_ID/maxresdefault.jpg)](https://youtu.be/Tz3f0jy09XI)


```
PC: make_video.py  ->  SD card (video.bin)  ->  Pico 2  ->  controller port 2  ->  PS1: sioplay
```

The Pico acts as a custom device on the PS1's controller/memory-card bus (SIO0). A homebrew PS1 program pulls compressed frames from it, decodes them and shows them full screen.

## Contents

| Path | What it is |
|---|---|
| `pico/` | Pico 2 firmware: PIO bus driver on core 0, SD card reader on core 1 |
| `ps1/` | PS1 player `sioplay` (C, PSn00bSDK) |
| `pc/make_video.py` | converts any video into `video.bin` |
| `pc/bw_demo.bin`, `pc/color16_demo.bin` | ready-made demo videos (black & white, 16 colours) |
| `disc/` | ready-built PS1 disc images at three bus speeds |
| `ps1_pico_video.uf2` | ready-built Pico 2 firmware |
| `sdtest/`, `sdtest.uf2` | SD card wiring test firmware |

## Video formats

| Format | Picture | Typical frame | Default speed |
|---|---|---|---|
| Black & white (`--mode bw`, default) | 160×120, enlarged 2× to full screen | a few hundred bytes | 15 fps |
| 16 colours (`--mode color16`) | 128×96, enlarged 2× | 0.7–2 KB | 10 fps |
| Raw colour (`--mode color`) | 128×128, 15-bit | 32 KB | ~1 fps |

Black & white stores only the pixels that flip since the previous frame, as run lengths; it was made for Bad Apple!!. The 16-colour mode picks a palette per scene (k-means), keeps still areas stable, and sends only changed pixels as skip / literal / fill tokens. The Pico doesn't care what is inside a frame, so new formats only change the PC script and the PS1 program.

## Requirements

- A PS1 or PSone that can boot burned CD-Rs (modchip or similar)
- Raspberry Pi Pico 2 (RP2350)
- An SD card (bare card or SPI module), FAT32 or exFAT
- A way into controller port 2, ideally a cut controller extension cable
- On the PC: Python 3, ffmpeg and numpy

## Wiring

**Controller port 2 → Pico 2.** Seen from the front of the console socket the pins run 9 8 7 | 6 5 4 | 3 2 1. The cable plug is the mirror image, so wire by signal name and check with a multimeter (about 7.6 V on pin 3, about 3.5 V on pin 5, pin 4 = GND). Wire colours in controller cables are not standard.

| Port pin | Signal | Pico GPIO | Pico pin |
|---|---|---|---|
| 1 | DATA | GP5 | 7 |
| 2 | CMD | GP2 | 4 |
| 3 | 7.6 V (rumble) | **not connected** | – |
| 4 | GND | GND | 8 |
| 5 | 3.5 V | **not connected** | – |
| 6 | ATT (select) | GP4 | 6 |
| 7 | CLK | GP3 | 5 |
| 8 | unused | – | – |
| 9 | ACK | GP6 | 9 |

**SD card → Pico 2** (SPI). Bare full-size card pins in brackets.

| SD signal | Pico GPIO | Pico pin |
|---|---|---|
| CS (1) | GP17 | 22 |
| MOSI (2) | GP19 | 25 |
| GND (3 and 6) | GND | 23, 38 |
| 3.3 V (4) | 3V3(OUT) | 36 |
| SCK (5) | GP18 | 24 |
| MISO (7) | GP16 | 21 |

A bare card is 3.3 V only. SD modules with a regulator chip want 5 V on VBUS (pin 40) instead. Keep the SD wires short.

**Safety**

- Never connect pin 3 (7.6 V) or pin 5 (3.5 V) to the Pico.
- Power the Pico from USB only, and power it **before** the console (switch the console off first when done).
- Don't push bare or solder-tinned wire ends into the console's socket; use a real plug.
- Keep memory card slot 2 empty: it shares the select line with controller port 2.
- Optional but helpful: a 10 kΩ resistor from SD CS to 3.3 V and a 100 nF capacitor across the card's power pins.

## 1. Make a video

```
pip install numpy
python pc/make_video.py myvideo.mp4                        # black & white, 160x120, 15 fps
python pc/make_video.py myvideo.mp4 --mode color16         # 16 colours, 10 fps
python pc/make_video.py myvideo.mp4 --mode color16 --preview
python pc/make_video.py myvideo.mp4 -o /path/to/sdcard/video.bin
```

The script prints the bandwidth the video needs and whether it should play at full speed. Copy `video.bin` to the root of the SD card, or rename one of the demos in `pc/` to try it.

| Option | Effect |
|---|---|
| `--fps N` | frames per second |
| `--size WxH` | picture size (width even, up to 256×240) |
| `--start 1:30 --duration 60` | convert part of a video |
| `--keyint N` | full keyframe at least every N frames |
| `--threshold 0-255` | black & white: brightness that counts as white |
| `--colors 2-16` | 16 colours: palette size |
| `--stability N` | 16 colours: higher = smaller, steadier, less exact (default 500) |
| `--saturation`, `--contrast`, `--brightness` | adjust the picture before converting |
| `--preview` | save a PNG of frames exactly as the PS1 will show them |

HDR videos (common from phones) are detected and tone-mapped automatically if ffmpeg has the `zscale` filter; otherwise they look washed out and grey.

## 2. Flash the Pico

Hold BOOTSEL, plug the Pico 2 into USB and copy `ps1_pico_video.uf2` to the drive that appears.

Open the Pico's USB serial port (any baud rate). Every 3 seconds it prints `SD ok (...). Waiting for the PS1...`, then `Playing: N frames sent to PS1` once the console is pulling frames. The LED toggles on every frame.

If the SD card won't mount, flash `sdtest.uf2` first: it checks each wire, starts the card step by step, measures which SPI speeds work and lists the files.

Building it yourself (Pico SDK 2.x, `PICO_SDK_PATH` set):

```
cd pico
git clone https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico.git lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
cmake -B build
cmake --build build
```

`sdtest/` builds the same way and uses the same library folder.

## 3. Burn the PS1 disc

Images in `disc/` are identical except for the controller-port clock:

| Disc | Bus clock | Throughput |
|---|---|---|
| `sioplay_250k` | 250 kHz (stock) | ~23 KB/s, start here |
| `sioplay_500k` | 500 kHz | ~40 KB/s |
| `sioplay_1mhz` | 1 MHz | ~60 KB/s, if the Pico and wiring keep up |

Burn the `.cue` to a CD-R at the slowest speed and boot it. The clock is `SIO_BAUD_RELOAD` in `ps1/main.c` (33.8688 MHz ÷ value).

Building it yourself:

1. Download PSn00bSDK 0.24 from its GitHub releases (it includes the `mipsel-none-elf-gcc` toolchain and `mkpsxiso`), plus CMake 3.21+ and Ninja.
2. Add the SDK's `bin` folder to `PATH` and set `PSN00BSDK_LIBS` to its `lib/libpsn00b` folder.
3. In `ps1/`: `cmake --preset default` then `cmake --build build`.
4. The image is `build/sioplay.bin` + `build/sioplay.cue`.

## Using it

Power the Pico first, wait for `Waiting for the PS1...`, then switch on the console with the disc. A dark blue screen means the player is running but no frames arrive yet.

The status line at the bottom of the screen shows what is happening:

| Counter | Meaning | If it keeps rising |
|---|---|---|
| OK | frames shown | good |
| WAIT | the Pico had no frame ready | Pico / SD side is stuck or slow |
| BUS | a transfer failed | wiring or bus timing; try a slower disc |
| BAD | damaged or unknown frame | bytes corrupted on the way |

Set `SHOW_STATUS` to 0 in `ps1/main.c` to hide it.

## Protocol

Every frame is one transaction on the controller bus. The PS1 is the master and clocks every byte; the Pico answers with device address `0x21`, so it never clashes with a real controller (`0x01`) or memory card (`0x81`).

| PS1 sends | Pico replies | Pico ACKs |
|---|---|---|
| `0x21` | – | if the address matches |
| `0x01` | – | if the command is GET_FRAME |
| `0x00` | `0xA5` ready / `0x00` not yet | if ready |
| `0x00` | length, low byte | yes |
| `0x00` | length, high byte | yes |
| `0x00` × length | frame bytes | all but the last |

`video.bin` is a list of `[u16 length, little endian][frame data]` records and loops at the end. The frame formats are documented at the top of `pc/make_video.py`; the PS1 decoders are `ps1/bwvideo.h` and `ps1/palvideo.h`.


## How this was made

The PS1 player (`ps1/`), the video converter (`pc/make_video.py`) and the
SD test firmware (`sdtest/`) were written by Claude, Anthropic's AI assistant.
The Pico firmware (`pico/`) started from Claude's version and was largely
rewritten by <your username>, who also built the hardware and tested
everything on a real PSone.

## Credits

- [PSn00bSDK](https://github.com/Lameguy64/PSn00bSDK) – PS1 SDK
- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk)
- [no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico) – SD card / FatFs library
- [psx-spx](https://psx-spx.consoledev.net/) – PS1 hardware documentation
- [PicoMemcard](https://github.com/dangiu/PicoMemcard) – proof that a Pico's PIO can speak the PS1 memory-card bus

## License

MIT, see [LICENSE](LICENSE).
