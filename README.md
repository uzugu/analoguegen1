# analoguegen1

Native C Pokémon Gen 1 engine with a modular Dramatic-Shape-style 3D voxel renderer for the **Analogue Pocket** via **openfpgaOS** (`VexiiRiscv` RV32 @ 100 MHz, 64 MB SDRAM, `of_gpu.h` hardware triangle rasterizer & Z-buffer).

> **No ROMs included.** You must supply your own legally obtained Pokémon Red, Blue, or Yellow `.gb` ROM on your SD card (or locally for PC testing).

## Architecture

- **Game Brain (`src/core/gb_brain.c`)** — Embeds [Peanut-GB](https://github.com/deltabeard/Peanut-GB) to execute authentic Gen 1 gameplay, scripts, battles, and `.sav` files while exposing live WRAM/SRAM state (`wCurMap`, player/NPC coordinates, party, battle state, DVs).
- **Renderer (`src/render/renderer.c`)** — Single translation unit owning `openfpgaSDK`'s `of_video.h` + `of_gpu.h` (320×240 RGB565 framebuffer, hardware 3D triangles/Z-buffer on Pocket `os30`, and SDL2 PC test harness).
- **Modular C Mods (`src/mods/`)** — Toggleable in-game Mods menu (`mod_manager.c`):
  - `mod_voxel` — Dramatic Shape-style 3D voxel overworld & 3D battles *(Phase 2+)*
  - `mod_moveinfo` — Battle move type-effectiveness HUD overlay *(Phase 1)*
  - `mod_followers` — PokéPC-style lead Pokémon follower tracking *(Phase 1 tracking, Phase 2 3D billboard)*
  - Planned: `mod_crystal_sprites`, `mod_wilds`, `mod_gen3box`, `mod_unique_icons`

## Building with Docker

```powershell
# 1. Build the Docker toolchain image (gcc-riscv64-unknown-elf + SDL2)
.\tools\docker_run.ps1 image

# 2. Build the Analogue Pocket RISC-V ELF (outputs build/app.elf)
.\tools\docker_run.ps1 pocket

# 3. Build the Linux/SDL2 test binary (outputs src/app_pc)
.\tools\docker_run.ps1 pc
```

## Submodules

Clone with submodules:
```bash
git clone --recursive https://github.com/uzugu/analoguegen1.git
```
