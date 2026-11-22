# gc550 — Linux driver for the AVerMedia Live Gamer Extreme (GC550)

Work in progress. See [DESIGN.md](DESIGN.md) for the design, the safety rules and the
bring-up plan and test results. **Working:** 1080p60 video and HDMI audio on Linux (V4L2 and
ALSA, so OBS, ffmpeg and browsers just see a webcam and a sound card), with the card still
working on Windows afterwards. Tested on one GC550 (bcdDevice 2.07) under Ubuntu's 7.3 kernel
with Secure Boot on. Only the GC550 (`07ca:4710`) is supported; the GC551/LGX2 is not.

## Installing (DKMS)

Needs DKMS and the headers for your kernel (`sudo apt install dkms linux-headers-$(uname -r)`
on Ubuntu). Then:

```
sudo make dkms-install
```

DKMS builds the driver, signs it with the machine's MOK if Secure Boot is on
(Ubuntu's DKMS does this by itself), rebuilds it for every new kernel, and the kernel
loads it automatically whenever the card is plugged in. Unplug and replug the card
after installing.

- Check: `dkms status gc550` and `modinfo gc550 | grep -E 'filename|signer'`
- Upgrade: pull the new sources and run `sudo make dkms-install` again; older versions
  are removed first.
- Remove: `sudo make dkms-uninstall`
- While the driver is bound to the card, userspace tools such as lgx2userspace cannot
  open it. To use one, run `sudo modprobe -r gc550` first.

## Building

```
make            # static endpoint check, then gc550.ko for the running kernel
make test       # userspace unit tests (no hardware)
make checkpatch # kernel coding style
```

Building never loads anything. Under Secure Boot the module must be signed before
`insmod` will accept it; see DESIGN.md, "Secure Boot".

Module parameters: `bootstrap=0` binds to the card and checks its endpoints but sends
it nothing; `index=` picks the ALSA card index.

## Layout

| Path | What |
|---|---|
| `src/gc550-guard.[ch]` | the command allow-list; nothing reaches the card without passing it |
| `src/gc550-bootstrap.[ch]` | walking and checking the bootstrap table |
| `src/gc550-bootstrap-data.c` | the bootstrap table, **generated** by `tools/gen-bootstrap.py` |
| `src/gc550-parser.[ch]` | capture stream parser (video frames and audio) |
| `src/gc550-signal.[ch]` | HDMI input status decoding |
| `src/gc550-core.c` | USB probe/disconnect, module parameters, lifetime |
| `src/gc550-bridge.c` | the only code that sends to the card; every command goes through the guard |
| `src/gc550-boot.c` | board-state check, guarded replay, colour-space and picture fixes |
| `src/gc550-status.c` | once-a-second input status poll |
| `src/gc550-stream.c` | scatter-gather bulk transfers on IN 0x83, parsed in a work item |
| `src/gc550-v4l2.c` | `/dev/videoN`: fixed 1920x1080 YUYV, vb2 |
| `src/gc550-alsa.c` | ALSA capture, 48 kHz S16_LE stereo |
| `tools/check-ep-cmd.sh` | build-time check that nothing but the bridge can send to the card |
| `tools/recordings/` | the recorded Windows bootstrap the table is generated from |
| `tests/` | userspace unit tests; they never touch a device |

## Tests

```
make -C tests                 # guard, bootstrap table, parser, and a differential
                              # test against lgx2userspace's FrameBuilder
make -C tests LGX_SRC=        # without the differential test
```

Regenerate the table (the output is committed, so building never needs Python):

```
tools/gen-bootstrap.py tools/recordings/gc550-f150158.txt > src/gc550-bootstrap-data.c
```

## Credits

The protocol knowledge, the bootstrap recording and the stream format come from
[lgx2userspace](https://github.com/ChrisAJS/lgx2userspace) (MIT, Copyright 2021 Chris
Sawczuk and contributors). This driver is `GPL-2.0-only OR MIT`.
