# Mario kart 64 5+players Ricky Gameplays

An unofficial fork of **[HarbourMasters/SpaghettiKart](https://github.com/HarbourMasters/SpaghettiKart)**, customized by Ricky Gameplays. The original PC port, engine, and upstream work are by HarbourMasters and the SpaghettiKart contributors. This fork preserves their Git history and credits.

## Added features

- Local play for 5–8 humans, with Grand Prix, VS, and Battle.
- Grand Prix for 3 and 4 players; optional bots fill the Grand Prix grid to eight racers for 3–7 humans.
- Game-style menus for mode, engine class, character, cup, and course selection, including repeated characters.
- Smaller position numbers, lap counters, and item boxes for the extra split screens, with item roulette and its sound.
- A shared minimap and race progress display. The course minimap uses the widest player's screen; the line display stays at the screen divider.
- A selectable player preference for the wider split screen in 5, 7, and 8 player layouts.
- Expanded Battle rankings with colored portraits and win totals, plus retry, course change, driver change, and quit.
- Controller bindings and reusable profiles for all eight ports.
- Adjustable racer render distance, from the original 1x range to 5x.
- A reversible **All tracks completed** cheat toggle that unlocks Mirror mode and the alternate title background while enabled.
- A **Skip to Grand Prix finale** cheat for 3–8 players, using current cup standings without awarding saved trophies.
- Expanded podium ceremonies with the remaining racers as spectators, corrected shadows, and a row angled away from the winner's path.

## Getting started

Download the **Windows x64** build from **[Releases](https://github.com/Ricky69Gameplays69Caballo/Mario-kart-64-5-players-Ricky-Gameplays/releases)**. Extract the entire ZIP, then open `Spaghettify.exe`. See the [release setup guide](docs/RELEASE-NOTES.md). Downloadable releases are Windows-only for now; macOS, Linux, ARM, and Android builds are deferred.

Open the PC menu with **Escape**. **Enable 8 players** and **Enable bots in 3 to 7 player grand prix** now default to enabled; existing saved choices are preserved. These options are under **Multiplayer → Local play**. Move right past 4P on GAME SELECT to reach 5P–8P. Either multiplayer option also exposes Grand Prix in the normal 3P/4P menus.

Use **Wide screen preference** and **Racer render distance** in the same menu. Controller profiles are in the controller bindings window. The completion toggle is under **Enhancements → Cheats**. Use the normal **C-right** input to change the shared map display.

During a 3–8 player Grand Prix, **Enhancements → Cheats → Skip to Grand Prix finale** opens the ceremony using current cup standings without modifying saved trophies.

This repository contains source changes and the custom multiplayer menu texture. ROMs, extracted game archives, saves, and personal controller settings are not included. Supply your own supported US Mario Kart 64 dump, as described in the [upstream setup guide](docs/UPSTREAM-README.md).

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
