# Mario Kart 64 5+ Players — Ricky Gameplays

Unofficial fork of [HarbourMasters/SpaghettiKart](https://github.com/HarbourMasters/SpaghettiKart), with local Grand Prix, VS, and Battle for up to eight humans, updated game menus and HUD, shared minimaps, controller profiles, and racer visibility settings.

## Downloads

Choose the file that matches your computer:

- `windows-x64.zip`: 64-bit Windows on Intel or AMD. Extract the entire folder, then open `Spaghettify.exe`.
- `macos-arm64.zip`: Apple Silicon Macs (M1 and newer). Extract and open `SpaghettiKart.app`.
- `macos-x64.zip`: Intel Macs. Extract and open `SpaghettiKart.app`.
- `linux-x64.AppImage`: 64-bit Intel/AMD Linux (glibc 2.35 or newer).
- `linux-arm64.AppImage`: 64-bit ARM Linux (glibc 2.35 or newer).

Linux: make the AppImage executable, then launch it. If FUSE is unavailable, use `--appimage-extract-and-run`. Install `zenity` or `kdialog` for the first-run ROM picker.

Each download has a SHA-256 checksum file. Assets are attached as the platform builds finish. A platform is available only when its binary appears below; a source archive is not a playable build.

**No ROM or extracted Mario Kart 64 game data is included.** Provide your own supported US Mario Kart 64 dump on first launch. The included `spaghetti.o2r` contains port resources and the custom multiplayer menu texture, not the extracted game archive `mk64.o2r`.

Open the PC menu with Escape and enable **Multiplayer → Local play → Enable 8 players**. Move right past 4P in GAME SELECT for 5P–8P. Controller profiles are in the bindings window.

The macOS app uses ad-hoc signing rather than an Apple developer certificate; macOS may require approval in System Settings → Privacy & Security before its first launch.

Android APKs are not available: this fork does not yet have an Android app wrapper, mobile launcher, or touchscreen controls. The ARM64 Linux download is not an Android APK.

Original port and engine: HarbourMasters and the SpaghettiKart contributors. Fork customization: Ricky Gameplays. See the repository README for details and credits.
