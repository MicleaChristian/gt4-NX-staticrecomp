<div align="center">

# GT4Recomp NX

**Gran Turismo 4, statically recompiled, running as Nintendo Switch homebrew.**<br>
A work-in-progress port of [GT4Recomp](https://github.com/moesuito/gt4-staticrecomp) to the Switch.

![Status](https://img.shields.io/badge/status-work%20in%20progress-d29922?style=for-the-badge)
![Platform](https://img.shields.io/badge/Nintendo%20Switch-homebrew%20NRO-e60012?style=for-the-badge)
![MIT](https://img.shields.io/badge/license-MIT-1f6feb?style=for-the-badge)

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/W1A12BEHQ8)

</div>

This repository is a fork of [moesuito/gt4-staticrecomp](https://github.com/moesuito/gt4-staticrecomp), the static
recompilation of **Gran Turismo 4 (NTSC-U, SCUS-97328)** for Windows. This fork takes the same recompiled game code
and brings it to the **Nintendo Switch** as a homebrew `.nro`, with a GPU renderer written for the console's own
graphics API (deko3d).

The Switch work lives on the branch
[`task/174-switch-nro-port`](https://github.com/MicleaChristian/gt4-NX-staticrecomp/tree/task/174-switch-nro-port).

> [!IMPORTANT]
> **You need your own original copy of the game.** This project does not include or distribute Gran Turismo 4. To
> play, make a disc image of your own **Gran Turismo 4 (USA, v2.00, SCUS-97328)** disc as a plain `.iso`.

> [!WARNING]
> **This is not playable at full speed yet.** The game boots, the menus work and races run, but well below the 60 FPS
> target. See [Where it stands](#where-it-stands). Expect bugs, slowdowns and picture errors.

## Where it stands

### Progress to 1.0

**1.0 means a locked 60 FPS on every screen, on a first-model Switch at the stock clock (no overclock).**

![Progress to 1.0](https://img.shields.io/badge/progress%20to%201.0-43%25-d29922?style=for-the-badge)
![Version](https://img.shields.io/badge/version-0.2.0-1f6feb?style=for-the-badge)

```
0.2.0  [#################.......................]  43%  ->  1.0 (60 FPS, stock clock)
```

| Reference screen | Speed in 0.2.0 | Target | Share of target |
|---|---|---|---|
| Car manufacturer menu | 55 FPS | 60 | 92% |
| Menus with video behind them | 25 FPS | 60 | 42% |
| Car select | 11 FPS | 60 | 18% |
| Race (Nürburgring, 6 cars) | 12 FPS | 60 | 20% |
| **Overall** | | | **43%** |

How the number is made: each screen's average speed divided by 60, capped at 100%, then the plain average of the four
screens. Speeds are 15-second averages from the built-in logs, measured on a first-model Switch, handheld, stock clock
(CPU 1020 MHz, GPU 307 MHz), on 2026-11-27. The number only moves when a measured run moves it.

With the console overclocked (CPU 1785 MHz) the race runs at about 20 FPS; that does not count toward 1.0.

What already works:

- The game's own code (EE) and its VU1 vertex programs run as precompiled native ARM code. Nothing is translated
  while the game runs.
- Graphics are drawn by the Switch GPU through deko3d, not by a software rasteriser.
- Three worker threads: game code, VU1 and graphics each have their own CPU core.
- Sound, controller input, memory card saves and disc reading from the SD card.
- A 60 FPS limiter, an on-screen FPS counter and detailed performance logs, including a sampling profiler.

Known problems:

- Speed, on every screen except one (table above).
- The race picture is slightly darker than it should be.
- Cars can show through walls and scenery in races.
- A long stall when a car fades in on the car select screen.
- Choppy sound when the game runs below full speed.

## How it works

The upstream project translates the PS2 game's machine code ahead of time into C++, and reproduces the PS2 hardware
the game talks to in software. This is a **hybrid recompilation**: PS2 hardware is still modelled in software, so
there is no claim of zero emulation. This fork compiles that same C++ for the Switch's ARM processor and replaces the
parts that depend on the host machine:

- **Game code:** the recompiled EE units and VU1 programs, built with devkitA64.
- **Graphics:** the CoreGS front end from upstream, with a new back end that draws on the Switch GPU (deko3d) and
  writes pixels back to the PS2's video memory only when the game really reads them.
- **Platform layer:** audio, input, file access, threads and timing through libnx.

Design notes: [docs/SWITCH-PORT-PLAN.md](docs/SWITCH-PORT-PLAN.md). Upstream architecture:
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Requirements

- A Nintendo Switch that can run homebrew (Atmosphère).
- Your own disc image of Gran Turismo 4 (USA, v2.00, SCUS-97328) as a plain `.iso`.
- An SD card with room for the image and the program.
- Full memory access for the program: start it from a game title or a forwarder, not from the album applet.

## Install

There is no packaged release yet. To try a development build:

1. Build `gt4recomp.nro` (see below).
2. Create the folder `switch/GT4` on the SD card.
3. Copy `gt4recomp.nro` and your `.iso` into it.
4. Start it from the homebrew menu.

Logs are written next to the program (`stderr_*.log`, `perf_*.log`, `sys_*.log`, `prof_*.log`).

## Build

You need [devkitPro](https://devkitpro.org/) with devkitA64, libnx and deko3d, and the recompiled game code generated
from your own copy of the game (the upstream [Build guide](docs/BUILD.md) covers the recompiler).

```sh
cd switch
make require SWITCH_EE_GENERATED=../private/switch-ee/generated BUILD=../private/build -j
```

The result is `gt4recomp.nro` in the build folder.

## Support

If you want to support the Switch port:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/W1A12BEHQ8)

The recompilation itself is the work of the upstream project. If you enjoy it, please support them as well:
[ko-fi.com/moesuito](https://ko-fi.com/moesuito).

Bug reports are welcome in the [issues](https://github.com/MicleaChristian/gt4-NX-staticrecomp/issues). Please say
which Switch model you use, docked or handheld, and attach the log files.

## Credits

**Switch port:** [MicleaChristian](https://github.com/MicleaChristian).

**GT4Recomp** by [moesuito](https://github.com/moesuito) and contributors is the project this port is built on: the
static recompilation, the runtime and the CoreGS renderer. Its contributors on GitHub: Wise
([gustavocanepa](https://github.com/gustavocanepa)), Julio Vitor Rufino ([JulioHeroZ](https://github.com/JulioHeroZ))
and Luan Antony ([luanantony](https://github.com/luanantony)).

| Project | Author | What it gives this project |
|---|---|---|
| [GT4Recomp](https://github.com/moesuito/gt4-staticrecomp) (MIT) | moesuito and contributors | The PC recompilation this repository is a fork of. |
| gt4-staticrecomp, the original project | [Kaezzey](https://github.com/Kaezzey) (Niket Shah) | Where GT4Recomp started. |
| [ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp) (MIT) | snuri00 | The runtime foundation: EE-to-C++ translator, PS2 hardware models and the original VU1 compiler. |
| [gt4-decomp](https://github.com/gustavocanepa/gt4-decomp) (MIT) | gustavocanepa | GT4's decompilation: function names, class layouts and engine notes. |
| [Nenkai's GT research](https://github.com/Nenkai) | Nenkai | File formats and the VU1 microcode encryption scheme. |
| [GT4FS](https://github.com/Razer2015/GT4FS) | Razer2015 | GT4 volume file-system research. |
| [PCSX2](https://github.com/PCSX2/pcsx2) (GPL-3.0) | PCSX2 team | Decades of PS2 knowledge. No PCSX2 code is in the Switch program. |
| [devkitPro](https://devkitpro.org/), libnx, deko3d | devkitPro team, fincs | The Switch homebrew toolchain, system library and graphics API. |

Exact revisions and licenses of the upstream dependencies: [THIRD_PARTY.md](native/THIRD_PARTY.md),
[NOTICE.md](NOTICE.md), [SOURCES.md](SOURCES.md).

## License and legal

- The code in this repository is under the [MIT License](LICENSE), as upstream.
- **You bring your own game.** The project does not include Gran Turismo 4, disc images, BIOS files, keys or saves.
- "Gran Turismo" and "PlayStation" are trademarks of Sony Interactive Entertainment. Gran Turismo 4, its logos and
  artwork belong to Sony Interactive Entertainment and Polyphony Digital. "Nintendo Switch" is a trademark of
  Nintendo. This is an unofficial fan project, not affiliated with or endorsed by any of them.

## Documentation

[Switch port plan](docs/SWITCH-PORT-PLAN.md) · [TASKS](TASKS.md) · [BUILD](docs/BUILD.md) ·
[ARCHITECTURE](docs/ARCHITECTURE.md) · [STATUS](docs/STATUS.md)
