# GC550 kernel driver — design

Status (2026-11-22): **implemented and working.** All bring-up gates passed on the
author's card. It captures 1080p60 video and audio in OBS on Linux, and the card still
works with the official Windows driver afterwards. It's installed via DKMS. See
"Results" at the end.

## Goals

1. The AVerMedia Live Gamer Extreme GC550 (USB `07ca:4710`) shows up as a normal
   V4L2 capture device (`/dev/videoN`) and an ALSA capture card. OBS, ffmpeg and
   browsers can use it with no helper process.
2. **The card must keep working with the official Windows driver.** No code path may
   ever reach the AT88SC CryptoMemory, config memory or the FX3 flash.
3. The driver loads under Secure Boot with the MOK that is already enrolled.
4. **Works on the newest kernels and needs no setup from a new user.** The end goal
   is the mainline kernel (`drivers/media/usb/gc550/`). Once it's there, the user plugs
   in the card and it works, on any distro, with Secure Boot on, no DKMS, no MOK,
   no udev rule. Every design choice below is made so upstream review can accept it.
5. Lightweight: written in C against the standard USB, V4L2/vb2 and ALSA APIs only,
   with no userspace daemon, no runtime dependencies, and nothing to build beyond
   the kernel's own toolchain.

## Distribution path

| Stage | Who it's for | What the user does |
|---|---|---|
| A. Out-of-tree DKMS | us, early testers | install a package. On Secure Boot machines without a MOK, DKMS asks for a one-time MOK enrollment and reboot (Ubuntu, Fedora, Arch all automate this) |
| B. Mainline (`linux-media` tree) | everyone | nothing: the distro kernel already contains the driver, signed by the distro |

To make stage B realistic, the code follows upstream rules from the first line:
- kernel coding style, clean under `checkpatch.pl --strict`;
- SPDX `GPL-2.0-only OR MIT`. lgx2userspace is MIT, so its notice is kept;
- passes `v4l2-compliance`;
- only standard APIs, with no private ioctls and no module-parameter knobs beyond debug.

The bootstrap table is generated **once** by a script in `tools/` and the C output
is committed. Building the driver never needs Python, Rust or anything else.

Rust: not used. The kernel's Rust layer has no USB-transfer, V4L2 or ALSA support
yet (checked on 7.3.0-11), and a C driver is what `linux-media` accepts today.

Non-goals for v1: GC551 (`07ca:0551`), resolutions other than the 1080p the card
emits, HDMI-out (passthrough) control, the analog audio input.

## What we know about the card (from lgx2userspace, verified on this card)

| Item | Value |
|---|---|
| USB | one vendor-class interface; bulk EP `01`/`81` (command bridge), `02`/`82` (unused), `03`/`83` (stream IN) |
| Command bridge | OUT `0x01`: `01 <addr> 01 <reg> <data…>` = I2C write (no reply); `02 <addr> 01 <count> <reg>` = I2C read, `<count>` bytes back on IN `0x81` |
| Bootstrap | the f150158 "stripped" Windows recording: 6576 transactions, only I2C to volatile chips, one `0E 02 07 FE 02 00` board-info read and one `08`. Proven Windows-safe on 2026-11-22 |
| Stream (IN `0x83`) | BT.1120-style markers. `00 FF FF C0` + 4-byte header = SAV; `00 FF FF C1` + 5-byte header = EAV; one SAV→EAV pair = one 1920×1080 YUYV frame (4 147 200 B); `00 FF FF 58` + BE u32 length = audio packet |
| Audio | 48 kHz, 2 ch, S16_LE interleaved, inside the `58` packets |
| Status | ADV7604 IO/HDMI/CP registers and the AVI InfoFrame (map `0x46`) read through the bridge, ~1 s poll |

## Architecture

```
            USB core
               │ probe 07ca:4710 (interface 0)
        ┌──────┴────────────────────────────────────────┐
        │ gc550_core   (device lifetime, kref, disconnect)│
        ├──────────────┬───────────────┬────────────────┤
        │ gc550_bridge │ gc550_stream  │ gc550_status   │
        │ EP 01/81     │ EP 83 URBs    │ delayed_work   │
        │   ▲ ONLY via │   │           │ (uses bridge)  │
        │ gc550_guard  │ gc550_parser  │                │
        │ + bootstrap  │  ├─ video ──▶ gc550_v4l2 (vb2)  │
        │   table      │  └─ audio ──▶ gc550_alsa (PCM)  │
        └──────────────┴───────────────┴────────────────┘
```

Files (one module, `gc550.ko`):

| File | Job |
|---|---|
| `gc550-core.c` | `usb_driver`, probe/disconnect, endpoint checks, refcounting |
| `gc550-guard.c` | C port of `CommandGuard`: the allow-list. Pure function, no USB, so it can be unit-tested in userspace |
| `gc550-bridge.c` | `gc550_bridge_send()` is the **only** function that submits to EP `0x01`, and it calls the guard first. `i2c_read`/`i2c_write` helpers on top, one mutex |
| `gc550-bootstrap.c` + generated `gc550-bootstrap-data.h` | the recording as a `const` table of `{op, len, bytes}`, generated at build time from `commanddata_lgx.cpp` by a script. The whole table is guard-checked **before the first byte is sent** |
| `gc550-stream.c` | anchored bulk-IN URBs on EP `0x83`, gap tracking, restart |
| `gc550-parser.c` | C port of `FrameBuilder` (SAV/EAV/audio state machine, checksums, carry-over across URBs) |
| `gc550-v4l2.c` | `video_device` + vb2 (vmalloc) queue, fixed 1920×1080 YUYV, input status, `V4L2_EVENT_SOURCE_CHANGE` |
| `gc550-alsa.c` | `snd_card` with one capture PCM, 48 kHz / 2 ch / S16_LE |
| `gc550-status.c` | the same register reads as `StatusMonitor`: cable, TMDS lock, timings, HDCP |

### Why not reuse the in-tree `adv7604` driver yet

I planned to wrap the bridge as an `i2c_adapter` and bind the in-tree `adv7604` driver. Reading its
platform data and probe path, it doesn't fit v1:

- `adv76xx` **writes the I2C map addresses into IO registers `0xF1`–`0xFE`** and creates
  dummy clients for *every* page (CEC, AVLINK, DPP, VDP, TEST…). Several of these
  pages are not in the GC550's allow-list, so the guard would refuse them, and
  remapping pages would put them where the FPGA and the recording don't expect them.
- Its probe does a reset and its own core init, which is tuned for SoC boards. The FPGA
  downstream expects the exact configuration from the Windows recording. All of these
  chips are volatile, so this isn't a safety problem, but it's a likely cause of "no picture".
- The card outputs a fixed 1080p YUYV stream anyway, so `adv7604`'s DV-timings
  machinery would add little in v1.

**v1:** replay the proven recording and do our own status reads, the same way the userspace
driver works today. **v2 (optional):** expose a *guarded* `i2c_adapter` that only lets
through the allow-listed addresses, and see whether `adv7604` can be bound with platform data
that matches the GC550's map layout. `adv7511-v4l2` is an HDMI *transmitter* driver.
It's only relevant to the passthrough output, which the bootstrap already sets up, so it isn't planned.

## Safety invariants (enforced in code, covered by tests)

1. Exactly one function submits to EP `0x01`, and it rejects anything the guard rejects.
   No raw-command ioctl, debugfs or sysfs knob exists.
2. Allowed, exactly as in the tested userspace guard:
   - `01`/`02` I2C with a 1-byte register and ≤32 bytes, to the GC550 write and read address sets;
   - the exact `0E 02 07 FE 02 00`;
   - the lone `08`.
   Everything else is refused. That covers AT88SC `05/06/07/09`, config-memory writes `0D/15`,
   all flash/firmware opcodes, and unknown opcodes.
3. The bootstrap table is checked in full before the first byte goes out. A table that
   fails the check means the probe fails and the card is untouched.
4. The `id_table` matches only `07ca:4710`, so no other AVerMedia device can bind.
5. No EP `0x02` writes, no USB control requests beyond what usbcore does itself, and no
   firmware download.
6. Unit tests: `gc550-guard.c` is compiled into a small userspace test program (the kernel
   has no built-in KUnit here) that runs the same cases as `CommandGuard.t.cpp`.
   One case is the old pre-f150158 string, which must be rejected at command 20 (`05 B8 00 00 0A`).

## Streaming details

- **URBs:** userspace uses 4 in-flight reads of `0x1FC000` B (~2 MiB). In the kernel I'll keep
  that size so the device sees the same behavior, using scatter-gather URBs (`urb->sg`,
  xHCI supports it) built from order-0 pages. That avoids 2 MiB contiguous allocations.
  Fallback if needed: 8–16 URBs of 512 KiB. The parser doesn't care where chunks split.
- **Parsing** runs in a work item that the URB completion queues, not in interrupt context.
  It copies video straight into the current vb2 buffer and audio straight into the ALSA ring,
  so there is no intermediate frame copy.
- **Frame emission rule** (from `FrameBuilder`): only a checksum-valid SAV→EAV pair whose
  span is 4 147 200 B ± 0x4000 becomes a frame. Anything else returns the buffer and counts a desync.
- **Timestamps:** `ktime_get_ns()` at EAV. `sequence` counts frames, and a gap bumps it so
  apps can see drops.
- **No signal:** the card keeps streaming its own black frames. v1 passes them through.
  `VIDIOC_ENUMINPUT` reports `V4L2_IN_ST_NO_SIGNAL` / `NO_POWER`, and a status change raises
  `V4L2_EVENT_SOURCE_CHANGE`. HDCP-protected input is reported in the log and through a
  read-only control. It is never captured around.

## Lifecycle

1. **probe:** check the endpoints and allocate. Nothing is sent. Then queue the bootstrap
   on `system_long_wq`, unless `bootstrap=0`. The bootstrap:
   - guard-checks the whole table;
   - drains IN 0x81;
   - reads the board state (ADV7604 IO `0xFA` == `0x6C` and PCA9539A `06/07` == `00 09`
     mean "configured": reuse it and don't replay);
   - when the state can't be read, as on a freshly powered card, which doesn't answer I2C
     reads before its bootstrap, does a 100 ms read-only probe of IN 0x83. No data means
     fresh: replay. Data means configured but unreadable: refuse and ask for a replug.
     This is the same fallback lgx2userspace uses.
   The parser resyncs on the next SAV, so no frame-boundary alignment probe is needed.
2. **bootstrap done:** register the video device and the sound card. Start the status poll.
3. **STREAMON / PCM trigger:** start the URBs on the first user and stop them after the last
   one leaves. Video and audio share the stream.
4. **disconnect:** cancel the bootstrap and the status poll, mark the bridge
   disconnected, kill the anchored URBs, `vb2_video_unregister_device`, and
   `snd_card_disconnect` + `snd_card_free_when_closed`. The device struct is freed on
   the last kref (probe, the V4L2 release and the ALSA private_free each hold one).

The kernel driver claims the interface, so lgx2userspace can't open the card while
`gc550.ko` is bound, and the two can't fight over it. `v4l2loopback` isn't involved
and can stay installed.

## Secure Boot

- The machine has Secure Boot on (lockdown) and an enrolled MOK, `CN=<machine> Secure Boot Module Signature key`,
  with its key pair in `/var/lib/shim-signed/mok/`. The private key is readable only by root.
- **Release:** ship as a DKMS package (`dkms.conf`, `/usr/src/gc550-<ver>`). Ubuntu's DKMS
  signs with that MOK on install and on every kernel update. No new key, no MOK
  enrollment screen, Secure Boot stays on.
- **Development:** I build as your user with `make -C /lib/modules/$(uname -r)/build M=$PWD`. You sign and load
  in your own terminal:
  ```
  sudo kmodsign sha512 /var/lib/shim-signed/mok/MOK.priv /var/lib/shim-signed/mok/MOK.der gc550.ko
  sudo insmod gc550.ko
  ```
- Licence: lgx2userspace is MIT, which is GPL-compatible. The source files use SPDX
  `GPL-2.0-only OR MIT` and `MODULE_LICENSE("Dual MIT/GPL")`, keep the original
  copyright notice, and credit the recording's source.
- Other people's machines: with the DKMS package, a Secure Boot user who has no MOK
  yet gets the distro's standard one-time enrollment prompt. Mainline removes that step entirely.

## Cleaning up the bootstrap (the "4-year-old code" part)

The recording is 6576 raw transactions. A reviewer will ask what it does, and so do we:

1. **Trim (deferred, not in v1):** from transaction #1766 on, the recording is the Windows
   driver's steady-state loop. That loop isn't only reads: it also *writes* interrupt clears
   (ADV7604 IO `*_CLR`, ADV7511 `0x96`) and HDMI-out service registers, and the last write is
   #6574. So trimming would change what the card receives. **v1 replays all 6576
   transactions exactly as recorded**, because that's the sequence proven safe on Windows.
   Trimming the loop is a separate, later experiment with its own Linux and Windows gates.
2. **Decode:** turn the remaining writes into per-chip, commented tables
   (`adv7604_init[]`, `adv7511_init[]`, `aic3x_init[]`, `fpga_init[]`, `pca9539_init[]`) and keep the
   original order. The existing `utils/decode_bootstrap.py` already labels most of them.
3. **Keep the raw recording as the reference:** a test checks that the decoded tables send
   byte-for-byte the same writes, in the same order, as the trimmed recording. The cleanup
   can't silently change what the card receives.

## Bring-up plan (each ★ gate needed the owner's OK) — all done

| # | Step | Touches the card? |
|---|---|---|
| 0 | Write the guard, parser and bootstrap generator, with userspace unit tests (guard cases, plus the parser fed a raw stream dump) | No |
| 1 | Build `gc550.ko` against 7.3.0-11 headers, with a static check that nothing else references EP 0x01 | No |
| 2 ★ | Load the module with the **card unplugged**: it loads and unloads cleanly under Secure Boot | No |
| 3 ★ | Plug in with module param `bootstrap=0`: probe and endpoint checks only, nothing sent | Enumeration only |
| 4 ★ | `bootstrap=1`: replay, then status reads only. Compare the logs with lgx2userspace's | Yes (proven sequence) |
| 5 ★ | Stream video: `v4l2-ctl --stream-mmap`, then OBS | Yes (read only) |
| 6 ★ | Audio: `arecord -D hw:GC550` | Yes (read only) |
| 7 ★ | Unplug and test on Windows again | — |
| 8 | DKMS package, then optionally upstream / share | — |

Optional for step 0: record a raw stream dump with the existing userspace build
(`LGX2_DUMP_FILE=…`). That lets the kernel parser be tested offline against real data. It's the
same proven userspace path you've already run.

## Results (2026-11-22, on a GC550 with bcdDevice 2.07, Ubuntu 7.3.0-11, Secure Boot on)

| Gate | Result |
|---|---|
| 2 load / unload | The kmodsign-signed module loads and unloads cleanly |
| 3 `bootstrap=0` | Binds on insmod and on hotplug, unbinds cleanly on rmmod and on unplug; nothing sent |
| 4 bootstrap | First try stopped safely ("board state unknown": a fresh card ignores I2C reads), fixed with the IN 0x83 fallback. Then 6576 commands in about 3 s, `/dev/videoN` and the ALSA card registered, HDMI status and cable replug reported |
| 5 video | steady 60.00 fps; 1918 frames with 0 drops, 0 bad spans, 0 desyncs |
| 6 audio | arecord clean; video and audio share the stream; OBS works via V4L2 and PulseAudio/PipeWire |
| 7 Windows | Windows 10 + official driver + OBS work afterwards |
| DKMS | Installs, is signed with the machine's MOK automatically, autoloads on plug-in |
| v4l2-compliance 1.32.0 | 48/48 without streaming, 57/57 with `-s 120` (read, MMAP with REQBUFS/CREATE_BUFS, no-poll/select/epoll); 0 failures, 0 warnings. DMABUF untested (needs `--expbuf-device`) |

Answered questions:
- **Frame rate:** the card delivers a steady 60 fps. The input status reports the source
  timing (for example "1920x1080p @ 59.89 Hz"; the pixel-clock register has 0.25 MHz resolution).
- **URB size:** 4 × 0x1FC000 scatter-gather URBs built from 64 KiB chunks work as well
  as libusb's.
- **Re-bootstrap detection:** IO `0xFA` / PCA9539A `06–07` as in lgx2userspace, plus the
  IN 0x83 fallback for fresh cards.

Still open:
- **The lone `08` command:** its meaning is unknown. It's harmless (Windows sends it) and kept as recorded.
- **Trimming the steady-state loop** (#1766+): deferred, needs its own Linux and Windows gates.
- **The in-tree `adv7604` subdevice** (DV timings, EDID control): a possible v2.
