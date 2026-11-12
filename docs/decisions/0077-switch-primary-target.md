# 0077 — Nintendo Switch homebrew is the primary target of this checkout

Date: 2026-11-11. TASK-174.

## Decision

This checkout’s **primary objective** is a Nintendo Switch (Switch 1) Atmosphere homebrew port of GT4Recomp:

- ship a **prebuilt ARM64 `.nro`** (translated EE/VU and host code compiled offline with the Switch toolchain);
- the player places the NRO next to their own **Gran Turismo 4 USA v2.00** plain `.iso` on the SD card;
- the NRO finds the ISO, loads game data from it, and runs — **no on-device recompile and no JIT**;
- graphics are **CoreGS only**, presented through **deko3d** (1× scale and Performance Mode by default);
- **GSdx / `gt4-gs.exe` is not built** for Switch;
- there is **no Tauri launcher** on Switch; the NRO is the app.

The published Phase 3/4 ordering (native Linux, then Android) in the upstream README is **superseded for this checkout**. Windows remains useful as a reference build for identity and tooling, but Switch playability is the success metric here.

## Why

The player goal is the same distribution shape as other homebrew static recomps: one executable beside the owned disc image. Switch has no desktop Vulkan 1.2 WSI and no separate GS process model that matches decision 0070; a Switch-first host and a deko3d CoreGS path are required.

## Consequences

- New docs: [SWITCH-PORT-PLAN](../SWITCH-PORT-PLAN.md).
- New host surface: `native/platform/` with Win32 and `nx` backends; Switch sources under `switch/`.
- Spec II, ultrawide, high internal resolution, and texture packs are out of scope until v2.00 boots and races are playable.
- Target hardware is Switch 1 (Erista / Mariko / OLED / Lite), not Switch 2.

## Related

- TASK-174
- [LINUX-PORT-PLAN](../LINUX-PORT-PLAN.md) (platform seam and arm64 determinism inventory reused; Linux itself is not a prerequisite)
- [ANDROID-PORT-PLAN](../ANDROID-PORT-PLAN.md) (memory and tiler notes reused as Hypothesis only)
