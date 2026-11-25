<p align="center"><img src="docs/images/banner.png" alt="Dreamstar Recomp" width="900"></p>

<p align="center">
  <a href="https://github.com/pixelnorth/DreamstarRecomp/releases/latest"><b>Download</b></a> ·
  <a href="#play">How to play</a> ·
  <a href="#features">Features</a>
</p>

> [!IMPORTANT]
> **This project was made almost entirely with AI** (Anthropic's Claude, through Claude Code), directed and tested by a
> human. You run it at your own discretion.

**Dreamstar Recomp** is a static recompilation of *Kirby Star Allies* (Nintendo Switch, version 1.0.0 or 4.0.0a) into a native
Windows application with a Vulkan renderer. It is a fork of [LAS Recomp](https://github.com/peiikai/LASRecomp).

**No game code and no game files are included.** You need your own copy of the game, already extracted: the folder
with `exefs` and `romfs` in it. On the first launch, Dreamstar builds the game code on your PC from your own files
(a few minutes, once).

<table>
  <tr>
    <td><img src="docs/images/stage.jpg" alt="A stage at 60 fps"></td>
    <td><img src="docs/images/map.jpg" alt="The world map"></td>
  </tr>
  <tr>
    <td><img src="docs/images/minigame.jpg" alt="A four-player minigame"></td>
    <td><img src="docs/images/launcher.jpg" alt="The start screen"></td>
  </tr>
</table>

## Features

- 60 fps in every scene (the original runs its 3D scenes at 30), and 90 to 240 fps with Frame Generation
- Any resolution up to 4K, dynamic resolution, ultrawide
- HDR10, SMAA, FSR 1, sharpening, anisotropic filtering
- The two movies at 60 fps (made on your PC on first play)
- Mods: RomFS replacements and HD texture packs (`docs/mods.md`)
- Save backups at each launch, restore, and import of a save made with a Switch save manager
- Keyboard and controller, in-game menu (F1)

## Requirements

- Windows 10 or 11, x64, a CPU with AVX2
- A GPU with a Vulkan 1.3 driver
- Your own extracted Kirby Star Allies 1.0.0 or 4.0.0a (`exefs` + `romfs`)

## Play

1. Download the latest `DreamstarRecomp-<version>-windows-x64.zip` from Releases and unzip it.
2. Run `lasrecomp.exe`, pick your game folder, wait for the first-launch build, then **Play**.

With RecompLauncher, the bundled `recomplauncher.json` does the same steps.

## Build from source

MSYS2 UCRT64 (`tools/setup_windows.ps1`), then from this folder: `make`, `make deps-windows`, `tools/fetch_zig.sh`,
`tools/dreamstar_dist.sh <version>`.

## Licence and credits

MIT (see `LICENSE`). Based on LAS Recomp by peiikai. Third-party components and their licences:
`THIRD-PARTY-NOTICES.txt`. Kirby Star Allies is a trademark of Nintendo / HAL Laboratory; this project is not affiliated
with or endorsed by them.
