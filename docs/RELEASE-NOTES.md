# Mario Kart 64 5+ Players — Ricky Gameplays v0.2.0 (Windows)

Unofficial fork of [HarbourMasters/SpaghettiKart](https://github.com/HarbourMasters/SpaghettiKart). Original port and engine by HarbourMasters and the SpaghettiKart contributors; fork customization by Ricky Gameplays.

## Changes since v0.1.2

- Local Grand Prix, VS and Battle for up to 16 human players, with expanded controller ports and profiles, menus, HUD and rankings.
- Optional 16-racer race grids with bots filling unused slots; faster item-box respawns for 16-racer races and 16-player Battle.
- One, two or three screen layouts. At 16 humans: 4x4 on one screen, 8 / 8 on two screens, or 6 / 5 / 5 on three screens. Windows must use an extended desktop.
- One centered minimap or race-progress line per selected screen. Minimap size and transparency sliders also accept numeric values; transparency includes racer markers.
- Player selection cycles left/right between 1P and 16P (1P and 8P when the 16-player option is disabled).
- Native-style expanded results, custom position labels for places 9–16, visible item roulette, drift audio fixes, and an optional race bomb-kart toggle for 3+ players.
- Fixes for crashes involving extra racers, controller reconnect handling, Battle pause text and crowded Big Donut spawn positions.
- Windows crash logging and optional detailed diagnostic logging in the PC menu.

## Windows download and setup

Download `Mario-Kart-64-5-Players-Ricky-Gameplays-v0.2.0-windows-x64.zip` from Assets. Extract the whole ZIP into a new folder, then run `Launch-Combined.cmd` or `Spaghettify.exe`. The launcher saves session logs locally. The `.sha256` file contains the ZIP checksum. GitHub's source archives are for developers, not playable builds.

**No ROM, extracted Mario Kart 64 game archive (`mk64.o2r`), saves, or personal settings are included.** Supply your own supported US Mario Kart 64 dump when prompted on first launch. The included `spaghetti.o2r` contains port resources and custom artwork.

Open Escape → Multiplayer → Local play. Enable 16 players for 9–16P choices, and select the number of screens. Move left from 1P or right past 4P to access expanded player pages. Use C-right to switch map displays. The bots option is labeled **Enable bots in 3+ players**. Controller profiles are in the bindings window.

For an existing installation, back up your saves and settings first, and use the matching executable and `spaghetti.o2r` from this ZIP together. Do not mix an old resource archive with this executable.

Windows x64 only. Other platforms remain deferred. Release compilation succeeded; the latest menu/map changes have not had a fresh gameplay pass. Extended multiplayer remains experimental.
