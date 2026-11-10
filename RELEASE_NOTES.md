# CemuX v1.0.0 (Windows x64)

CemuX is a Wii U emulator derived from Cemu (created by Exzap and the Cemu developers), with game fixes, native Wii U menu support, expanded Wii Remote support, amiibo, and a built-in link to AzaharX for Wii U / 3DS games.

## Updates

- Game compatibility improvements, including Watch Dogs, The Amazing Spider-Man, ZombiU, and NBA 2K13.
- Corrected Mii rendering and Smash portraits, plus improved Mario Kart 8 online-lobby lighting.
- Original HOME menu and game manuals, game closing/switching, and persistent Start in Wii U Menu support through supervised helper processes.
- Expanded Wii Remote, MotionPlus, and Nunchuk support, including hotplug, automatic controller allocation, battery reporting, and speaker audio. An optional 3 kHz compatibility mode is available; some speaker crackle remains.
- Bluetooth passthrough through a dedicated compatible USB Bluetooth adapter using WinUSB, alongside regular Bluetooth controllers. Supports multiple Wiimotes, persistent pairing, and power-off handling.
- Virtual Guitar Hero Live controller with configurable inputs; additional physical-guitar testing remains pending.
- Cafe au Lait setup: a companion Wii U app and an integrated CemuX importer transfer console system/account files into the correct locations, with restart support. Console file access is read-only.
- Native amiibo registration and writable BIN support. Wolf Link data saving and use is verified between Twilight Princess HD and Breath of the Wild. Animal Crossing: amiibo Festival is also verified.
- Per-game GX2 full-sync controls, corrected save timestamps, improved shutdown behavior, and smoother startup/game-list scrolling.

## Shared CemuX / AzaharX Link

- Native Tools > Wii U/3DS Link setup for same-PC and LAN connections, with IP-and-passphrase Host/Join controls. No separate launcher is required.
- Native Mii Maker transfers, Sonic Lost World RC-gadget transfers, and Sonic Boom progress-based unlocks between the two emulators.
- Smash Bros.: use AzaharX as a 3DS controller for CemuX and transfer custom fighters/Miis, including corrected Wii U portraits. Remote players need a separate screen-sharing app, such as Sunshine/Moonlight or Parsec, to see and hear the Wii U game.
- Monster Hunter 3 Ultimate: bidirectional save transfers and cross-emulator local multiplayer. Direct Internet connections are available experimentally; MH3U Internet gameplay has not been verified.
- Existing Azahar multiplayer rooms remain separate from the link, and Pretendo configuration is preserved. Configure Host/Join before launching the games.

## Notes

- Windows x64 only.
- No games, keys, firmware, console dumps, saves, or amiibo data are included. You need your own.
- CemuX uses Cemu's normal user-data location by default.
- Internal helper executables (`native-home`, `native-manual`, `cafe-au-lait`) keep their `Cemu_release.exe` names on purpose. Do not rename them.
- Setup guides are in the zip: `WIIU-3DS-LAN.txt`, `CAFE-AU-LAIT.txt`, `USB-BLUETOOTH-PASSTHROUGH.txt`.
- Licensed under MPL-2.0, as Cemu is. Cemu's license and credits are retained. CemuX is a separate build and is not an official Cemu release.
