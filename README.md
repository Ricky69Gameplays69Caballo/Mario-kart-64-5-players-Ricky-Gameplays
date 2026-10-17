# Mario kart 64 5+players Ricky Gameplays

An unofficial fork of **[HarbourMasters/SpaghettiKart](https://github.com/HarbourMasters/SpaghettiKart)**, customized by Ricky Gameplays. The original PC port, engine, and upstream work are by HarbourMasters and the SpaghettiKart contributors. This fork preserves their Git history and credits.

## Added features

- Local Grand Prix, VS and Battle for up to 16 humans, expanded controller ports and reusable profiles.
- Optional 16-racer grids with bots, faster item-box respawns, expanded results and custom place labels 9–16.
- One, two or three screens: 16 players use 4x4, 8 / 8, or 6 / 5 / 5 layouts respectively.
- One shared minimap or progress line on each screen, with configurable minimap size and transparency including racer markers.
- Cyclic 1–16 player selection, smaller expanded HUD, visible item roulette and restored drift audio.
- Battle rankings, safer arena spawns, pause menu fixes and optional race bomb karts for 3+ players.
- Racer render distance, reversible All tracks completed cheat, Grand Prix finale preview and expanded spectator ceremonies.
- Extra-racer crash fixes and Windows crash/diagnostic logging.

## Getting started

Download **Windows x64 v0.2.0** from [Releases](https://github.com/Ricky69Gameplays69Caballo/Mario-kart-64-5-players-Ricky-Gameplays/releases/latest), extract the entire ZIP, and run `Launch-Combined.cmd` or `Spaghettify.exe`. Read the [release setup guide](docs/RELEASE-NOTES.md). Other platforms are deferred.

Open **Escape → Multiplayer → Local play**. Enable **16 players** for the 9–16P choices and select **Number of screens**. Windows must extend the desktop across the selected monitors. Left from 1P wraps to 16P; right from 16P wraps to 1P. With 16-player mode disabled, the loop ends at 8P. **Enable bots in 3+ players** controls the Grand Prix bot option. Use **C-right** to switch maps.

This repository contains source changes and custom artwork. ROMs, extracted game archives, saves, logs and personal controller settings are not included. Supply your own supported US Mario Kart 64 dump; see the [upstream setup guide](docs/UPSTREAM-README.md). Extended multiplayer remains experimental; the latest map/menu changes were compiled but have not had a fresh gameplay pass.

## Building

Clone with submodules:

```sh
 git clone --recurse-submodules https://github.com/Ricky69Gameplays69Caballo/Mario-kart-64-5-players-Ricky-Gameplays.git
 cd Mario-kart-64-5-players-Ricky-Gameplays
```

Follow the [upstream build instructions](docs/BUILDING.md) for dependencies and ROM placement. For Windows, install Visual Studio 2022 with C++ tools and CMake, then run:

```sh
cmake -S . -B build/x64 -G "Visual Studio 17 2022" -T v143 -A x64
cmake --build build/x64 --config Release --target ExtractAssets
cmake --build build/x64 --config Release --target GenerateO2R
cmake --build build/x64 --config Release
```

During configuration, CMake automatically applies [the controller dependency patch](patches/libultraship-combined.patch) to the pinned libultraship submodule. Reconfiguration recognizes an already applied patch. GenerateO2R includes this fork's custom multiplayer menu texture; keep the generated `spaghetti.o2r` alongside the executable.

The Combined changes were developed and built on Windows x64. Other platforms have not been checked for this fork, and the extended multiplayer modes may still have bugs. Upstream release downloads do not contain these changes.

## Credits

- [HarbourMasters/SpaghettiKart](https://github.com/HarbourMasters/SpaghettiKart): original project and contributors.
- libultraship and its contributors: supporting port framework.
- Ricky Gameplays: this fork and custom multiplayer menu artwork.

See the [original README](docs/UPSTREAM-README.md) for upstream maintainers, dependencies, and documentation. Existing attribution and copyright notices are retained.
