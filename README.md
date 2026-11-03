# AGB-RA

Native **Nintendo 3DS** homebrew: Game Boy, Game Boy Color, and Game Boy
Advance with **RetroAchievements** (`rcheevos`). GBA uses the gpSP dynarec.
Dual-screen — game on top, RA + menus on the touch screen.

This is a **devkitPro** project. Build it on your machine, copy `AGBRA.3dsx`
to the SD card, run it from Homebrew Launcher / FBI.

## What you need

- A 3DS / 2DS with **Luma3DS** and the Homebrew Launcher
- [devkitPro](https://devkitpro.org) with the 3DS packages
- `git`, `cmake`, `make`
- Your GBA ROMs (you supply these)
- Official GBA BIOS as `gba_bios.bin` (recommended, required for some hardcore sets)
- Internet on the 3DS for RetroAchievements login / unlocks

---

## Windows (devkitPro already installed)

Do **not** use cmd.exe or PowerShell. The compiler lives in the **MSYS2**
shell that the installer put next to itself.

1. Start Menu → **MSYS2**, or run `C:\devkitPro\msys2\msys2.exe`
2. One-time host + 3DS packages (skip any already installed):

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM

pacman -S --needed git cmake make
pacman -S --needed 3ds-dev 3ds-cmake 3ds-zlib
```

3. `cd` into the unzipped folder. MSYS paths look like `/c/Users/...`:

```sh
cd /c/Users/YOU/Downloads/agbra-3ds
./fetch-deps.sh     # clones mGBA, builds libmgba.a  (once)
make -j$(nproc)     # writes AGBRA.3dsx in this folder
```

If `$DEVKITPRO` is empty, the installer used a custom drive — set it to
that folder with forward slashes, e.g. `export DEVKITPRO=/d/devkitPro`.

`fetch-deps.sh` takes several minutes the first time. After that, only
`make` is needed when you edit source.

Copy `AGBRA.3dsx` to the SD card (see Install below).

---

## Linux / macOS

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
sudo dkp-pacman -S 3ds-dev 3ds-cmake 3ds-zlib   # or pacman, on dkp MSYS

chmod +x fetch-deps.sh
./fetch-deps.sh
make -j$(nproc)
```

## Install on the 3DS

```
sdmc:/3ds/AGBRA.3dsx
sdmc:/3ds/agbra/gba_bios.bin      # official BIOS, md5 a860e8c0b6d573d191e4ec7db1b1e4f6
sdmc:/roms/gba/*.gba
```

Saves land in `sdmc:/3ds/agbra/saves/`. States in `sdmc:/3ds/agbra/states/`.
Config (RA token, hardcore, scale) in `sdmc:/3ds/agbra/config.ini`.

## Controls

| 3DS | GBA |
|-----|-----|
| A B Start Select D-pad L R | same |
| Circle Pad | D-pad |
| L + R + Down | pause menu |
| X / Y | save / load state (softcore only) |
| ZL | fast-forward (softcore, New 3DS) |
| X on ROM list | RetroAchievements login (software keyboard) |
| Y on ROM list | rescan SD |
| L on ROM list | download from a RomM server onto the SD card |
| Up / Down on an achievement | description, and the percent of players who earned it |

Hardcore mode disables save states and turbo, matching RetroAchievements rules.

On the achievement list, the highlighted row shows its description and how
many players have earned it (the RetroAchievements rarity percent).

## RomM

L on the ROM list downloads Game Boy, Game Boy Color, and Game Boy Advance
games from your own [RomM](https://github.com/rommapp/romm) server onto the
SD card (`sdmc:/roms/gb`, `sdmc:/roms/gbc`, `sdmc:/roms/gba`).

The sign-in asks for the server URL (`http://192.168.1.10:8080`), your
username, and either the password or a client API token that starts with
`rmm_`. The password is not stored. The token is saved in `config.ini`,
same as the RetroAchievements token. Create a long-lived token in RomM
with access to read platforms and ROMs. Y on a game replaces a file that
is already on the card.

## Architecture

```
source/                 3DS frontend (libctru)
third_party/rcheevos/   vendored RA client (MIT)
third_party/mgba/       cloned by fetch-deps.sh (MPL-2.0)
third_party/prefix/     libmgba.a + headers after fetch-deps
```

- **Core:** gpSP dynarec for GBA, mGBA for Game Boy and Game Boy Color. GBA falls back to mGBA if the dynarec cannot map executable pages.
- **Achievements:** rcheevos `rc_client` over 3DS `httpc` (HTTPS, worker thread)
- **Video:** RGB565 blit to the top screen (1x / 360×240 / 400×240)
- **Audio:** NDSP stereo PCM16
- **UI:** bottom-screen console (ROM browser, cheevo list, settings)

## Notes

- New 3DS clock boost (`osSetSpeedupEnable`) is on. GBA on an old 3DS uses the dynarec; the first seconds compile blocks and run slower. The HUD shows `dyn` while that core is active.
- RetroAchievements needs Wi-Fi. The 3DS TLS store is old, so certificate verify is disabled for `retroachievements.org` only in the http helper.
- Do not distribute BIOS or ROMs with this source.
