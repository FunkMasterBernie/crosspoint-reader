# Deadtree

Open-source e-reader firmware for the Xteink X3 and X4.

**Deadtree is a fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).**
Practically all of the engineering here — the EPUB engine, the UI, the display
drivers, the networking — is CrossPoint's work and the work of the
[FreeInk SDK](https://github.com/crosspoint-reader/freeink-sdk) authors, used under the
MIT licence. This fork exists to carry changes that don't fit upstream's scope, not to
compete with it. If you want the maintained, multi-device firmware with a community
behind it, **go use CrossPoint** — it is the better choice for almost everyone.

Deadtree is not affiliated with or endorsed by the CrossPoint Reader project.

## What's different here

- **OPDS auto-download.** Entering the catalogue browser pulls up to five new books
  from the server's recently-added feed, skipping anything already on the card.
  Setting: `opdsAutoFetch`.
- **Automatic progress sync on sleep.** The reader publishes your place on the way
  into deep sleep, which is the one moment the radio is free — every other Wi-Fi
  session ends in a reboot to clear heap fragmentation, and sleep is already a full
  reset on wake. It runs headless: the page stays on screen and the sleep image lands
  on top of it. Setting: `koAutoSync`, off by default.
- **English-only build.** Dropping the other 33 translations returns about 370 KB of
  flash. Set by `custom_i18n_builtin_langs` in `platformio.ini`; delete that line to
  get all languages back.

Everything else — formats, library, wireless workflows, themes, dictionaries — is
CrossPoint's. Read [their README](https://github.com/crosspoint-reader/crosspoint-reader#readme)
for what the firmware actually does.

## Target hardware

Built and tested only on the **Xteink X3** (ESP32-C3, UC8253 panel, 792×528). The
`gh_release` target still compiles for the X4, and upstream supports the X4 Pro, X4
Classic, Seeed reTerminal Sticky and M5PaperMono, but none of those are tested here.

## Build

Needs the official PlatformIO installer, not Homebrew's. One pin is required: the
platform ships tool-scons 4.8.1 while pioarduino core 6.2.0 wants 4.11.1, and they
reinstall over each other mid-build, surfacing as
`ModuleNotFoundError: No module named 'SCons.Tool.FortranCommon'` at the link step.

```bash
git clone --recurse-submodules <this repo>   # the freeink-sdk submodule is mandatory
~/.platformio/penv/bin/pip install "pioarduino==6.1.19"
rm -rf ~/.platformio/packages/tool-scons
~/.platformio/penv/bin/pio run -e gh_release
```

`gh_release` is the X3/X4 build. `default` is the dev build: debug logging plus
`CROSSPOINT_WAIT_FOR_USB_SERIAL`, which makes the device wait for a serial connection
at boot — not what you want on a device you read on.

## Flash

App-only at `0x10000`, keeping the OEM bootloader and partition table.

```bash
esptool --chip esp32c3 --port /dev/cu.usbmodem* --baud 921600 \
  --after no-reset write-flash 0x10000 .pio/build/gh_release/firmware.bin
esptool --chip esp32c3 --port /dev/cu.usbmodem* --after hard-reset run
```

`--after no-reset` matters on the X3: a reset boots the reading app, and there is no
BOOT button to fall back on, so you lose the port until you unplug and replug the
magnetic cable. **Back up the factory image before your first flash:**
`esptool read_flash 0 0x1000000 stock.bin`.

Some units bought from third-party sellers ship with USB flashing locked. That is not
a dead end — see upstream's notes on the Xteink Unlocker — but heed their warning that
flashing unsupported firmware on a locked unit can leave it with no recovery path.
Deadtree is not one of the firmwares that tool supports.

## Licence

MIT, the same as upstream. See [`LICENSE`](LICENSE), which carries both copyright
lines as the licence requires.

**The firmware binary is effectively GPLv2**, because wolfSSL is linked in under its
GPL option. Distribute a `.bin` and you owe the corresponding source under
GPLv2-compatible terms; publishing this repository covers it. A closed-source release
would need a commercial wolfSSL licence or a different TLS stack. Upstream is in the
same position.

Every component and its licence is listed in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

## Credit

- **CrossPoint Reader** — the firmware this is built on.
  https://github.com/crosspoint-reader/crosspoint-reader
- **FreeInk SDK** — the hardware abstraction underneath it.
- Portions of this software are copyright © The FreeType Project
  (https://www.freetype.org). This software is based in part on the work of the
  FreeType Team.
- The authors of Expat, miniz, uzlib, minibidi, wolfSSL, ArduinoJson, SdFat, PNGdec,
  JPEGDEC, arduinoWebSockets, QRCode, the Noto and OpenDyslexic fonts, and Lucide
  icons.

If you find Deadtree useful, consider supporting the upstream project instead of this
one — they did the work.
