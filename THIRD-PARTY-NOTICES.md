# Third-party notices

Deadtree is a fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
and redistributes a good deal of other people's work. This file lists it, with the
licence each component is used under.

Nothing here is legal advice; it is an inventory, kept accurate against the actual
build. Paths are relative to the repository root.

---

## 1. The fork itself

| Component | Licence | Copyright |
|---|---|---|
| Deadtree (this fork) | MIT | 2026 Deadtree contributors |
| CrossPoint Reader (upstream) | MIT | 2025 CrossPoint Reader organization — see [`LICENSE`](LICENSE) |
| `freeink-sdk` (git submodule) | MIT | 2026 FreeInk — see [`freeink-sdk/LICENSE`](freeink-sdk/LICENSE) |

The MIT text covering all three is in [`LICENSE`](LICENSE); the SDK keeps its own
copy in its own directory.

---

## 2. Read this part: the firmware binary is GPLv2

**wolfSSL is licensed GPLv2-or-later, or commercially.** This project uses the GPL
option and links wolfSSL into the firmware image, which makes any compiled `.bin`
distributed from this repository a combined work containing GPLv2 code.

In practice that means:

- **Distributing a binary obliges you to offer the corresponding source** under
  GPLv2-compatible terms. Publishing this repository satisfies that.
- **MIT is GPLv2-compatible**, so Deadtree's own source staying MIT is fine. Someone
  who wants only the MIT parts can take them under MIT; the *combined binary* is the
  thing that carries the GPL obligation.
- **A closed-source release is not possible** on these terms. That would need a
  commercial wolfSSL licence or a different TLS stack.

`links2004/WebSockets` is **LGPL-2.1**, which additionally expects users to be able to
relink the library. Shipping the full source satisfies it.

Upstream CrossPoint is in exactly the same position. This fork does not change it.

---

## 3. Libraries linked into the firmware

Resolved from `lib_deps` in [`platformio.ini`](platformio.ini).

| Library | Version | Licence |
|---|---|---|
| [wolfSSL](https://github.com/wolfSSL/Arduino-wolfSSL) | 5.7.2 | **GPL-2.0-or-later**, or commercial. © wolfSSL Inc. |
| [WebSockets](https://github.com/Links2004/arduinoWebSockets) | 2.7.3 | **LGPL-2.1**. © Markus Sattler |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | 7.4.2 | MIT. © Benoit Blanchon |
| [SdFat](https://github.com/greiman/SdFat) | transitive | MIT. © 2011–2020 Bill Greiman |
| [QRCode](https://github.com/ricmoo/QRCode) | 0.0.1 | MIT. © 2017 Richard Moore |
| [PNGdec](https://github.com/bitbank2/PNGdec) | 1.1.6 | Apache-2.0. © 2020 BitBank Software, Inc. |
| [JPEGDEC](https://github.com/bitbank2/JPEGDEC) | pinned commit | Apache-2.0. © 2020 BitBank Software, Inc. |

The following come from the `freeink-sdk` submodule, all MIT (© 2026 FreeInk):
`BatteryMonitor`, `InputManager`, `FreeInkDisplay`, `SDCardManager`, `UsbMassStorage`,
`BoardConfig`, `XteinkDetect`, `PowerManager`, `MemoryManager`, `FrontlightManager`,
`Rtc`, `Imu`, `SecureNet`, `FreeInkUI`, `Icons`, `FreeInkFont`.

---

## 4. Code vendored into this repository

Under [`lib/`](lib), compiled directly into the firmware.

| Component | Path | Licence |
|---|---|---|
| Expat XML parser | `lib/expat` | MIT. © 1997–2000 Thai Open Source Software Center Ltd, © 2000 Clark Cooper, © 2002–2003 Fred L. Drake Jr., and contributors |
| miniz | `lib/miniz` | Unlicense (public domain). © 2013–2014 RAD Game Tools and Valve Software |
| uzlib | `lib/uzlib` | Zlib licence. © 2014–2018 Paul Sokolovsky and uzlib authors |
| minibidi (UAX #9) | `lib/MiniBidi` | MIT. Original author Ahmad Khalifa (arabeyes.org); isolate and rule fixes from mintty by Thomas Wolff |

### Font engine, via the SDK's `FreeInkFont`

| Component | Path | Licence |
|---|---|---|
| FreeType | `freeink-sdk/libs/font/FreeInkFont/third_party/freetype` | **FreeType Licence (FTL)** — see `FTL.TXT` there — or GPLv2, at your option. © The FreeType Project |
| stb_truetype | `freeink-sdk/libs/font/FreeInkFont/third_party/stb` | Public domain (Unlicense) or MIT, at your option. © Sean Barrett |

**The FTL requires this credit, so here it is:**

> Portions of this software are copyright © The FreeType Project
> (https://www.freetype.org). All rights reserved.
>
> This software is based in part on the work of the FreeType Team.

### Bundled fonts

All under the **SIL Open Font License 1.1**; each keeps its `OFL.txt` beside it in
`lib/EpdFont/builtinFonts/source/`.

| Font | Copyright |
|---|---|
| Noto Sans, Noto Serif | © The Noto Project Authors (Google) |
| Noto Sans Arabic, Noto Sans Hebrew | © The Noto Project Authors (Google) |
| OpenDyslexic | © Abbie Gonzalez and contributors |

Two OFL terms worth remembering: the fonts may not be sold on their own, and a
modified version may not keep a Reserved Font Name.

### Icons

Lucide icons, `freeink-sdk/libs/assets/Icons/lucide` — **ISC**, © 2026 Lucide Icons
and Contributors, itself a fork of Feather (MIT, © 2013–2017 Cole Bemis).

---

## 5. Present in the source tree, not linked by the current build targets

The `freeink-sdk` submodule carries libraries that the X3/X4 build does not compile.
They ship with the source, so their notices apply to source distribution.

`freeink-sdk/libs/book/FreeInkBook/third_party`:

| Component | Licence |
|---|---|
| Expat | MIT. © 1997–2000 Thai Open Source Software Center Ltd and contributors |
| libunibreak | Zlib licence. © 2015–2026 Wu Yongwei, © 2018 Andreas Röver |
| pngle | MIT |
| TJpgDec | Free with attribution, no warranty; redistributions must keep the notice. © 2021 ChaN |
| stb_truetype | Public domain (Unlicense) or MIT |
| Unicode data files | Unicode Licence v3 (https://www.unicode.org/license.txt) |
| English hyphenation patterns | © 1990, 2004, 2005 Gerard D.C. Kuiken — see `hyphen-patterns/LICENSE` |

`freeink-sdk/libs/book/ContentProtection/third_party`: miniz, as above.

---

## 6. Toolchain

Built with [PlatformIO](https://platformio.org) (Apache-2.0) against the
[pioarduino](https://github.com/pioarduino/platform-espressif32) fork of
platform-espressif32, which pulls in Espressif's Arduino core and ESP-IDF
(Apache-2.0). These are build tools, not redistributed in the firmware image, with
the exception of the Arduino core and ESP-IDF runtime components linked into it.

---

## Keeping this file honest

Dependencies drift. Re-check it whenever `lib_deps` in `platformio.ini` changes, the
`freeink-sdk` submodule is bumped, or anything new lands under `lib/`.
