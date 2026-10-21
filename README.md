# Mario Kart 64 PC Ultimate Edition — Ricky Gameplays

Unofficial fork of [HarbourMasters/SpaghettiKart](https://github.com/HarbourMasters/SpaghettiKart). Original PC port and engine by HarbourMasters and the SpaghettiKart contributors; customization by Ricky Gameplays.

## Added features

- **Up to 16 local players** in Grand Prix, VS and Battle, with optional bots filling race grids.
- **AFK takeover:** inactive players become bots after a configurable timeout; controller input immediately restores human control. Bots can use items, with separate race and Battle behavior.
- **1, 2 or 3 monitors**, improved split-screen layouts, duplicate character selection and saved controller-device assignments.
- Optional **FPS counter** and **hold Tab to fast-forward**.
- Adjustable **minimap size and transparency**, including player markers, with a separate shared map on each selected monitor.
- Expanded results, standings, 9th–16th place graphics and Grand Prix ceremonies.
- Multiplayer HUD, audio, fullscreen/monitor-switching and visual fixes, plus a live Luigi Raceway video screen.
- Faster item-box respawns and reduced lightning frequency in 16-player mode; configurable race bomb obstacles.
- Optional detailed crash logging and a Grand Prix finale shortcut.

## Defaults

8-player and 16-player options, Grand Prix bots, AFK takeover, Tab fast-forward and the Luigi Raceway multiplayer screen are enabled. AFK timeout is **5 seconds**; minimap size is **85%** with **25% transparency**. FPS counter and detailed logging default off. Existing saved settings override fresh defaults.

## Windows download

Download `Mario-Kart-64-PC-Ultimate-Edition-Ricky-Gameplays-v0.3.0-windows-x64.zip` below, extract the entire ZIP into a new folder, then run `Launch-Combined.cmd` or `Spaghettify.exe`. The `.sha256` asset provides its checksum. Open **Escape → Multiplayer → Local play** for multiplayer options.

**No ROM, extracted Mario Kart 64 game archive (`mk64.o2r`), saves, personal settings or logs are included.** Supply your own supported US Mario Kart 64 dump on first launch. The included `spaghetti.o2r` contains port resources and custom artwork. Keep this release's executable and resource archive together.

Windows x64 only. Release compilation succeeded; the latest changes have not had a fresh gameplay pass. Extended multiplayer and Battle AI remain experimental.

## Source and building

Clone this repository with submodules and follow [the build guide](docs/BUILDING.md). The original project documentation and credits are preserved in [the upstream README](docs/UPSTREAM-README.md). The controller dependency patch is applied by CMake during configuration.
