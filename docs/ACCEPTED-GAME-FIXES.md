# Accepted Game Fixes

Latest2026-11-08: the verified LAN/IP/passphrase implementation is now merged into
this GitHub Builds source, including native Tools Host controls. See
`docs/WIIU-3DS-LAN.txt` for setup and limits. This supersedes the historical
localhost-only statement below. Native HOME/manual remain helper-process based.

The consolidated main source is `src/` in this repository. It includes the previously
accepted cumulative fixes, the accepted Resident Evil Revelations, Watch Dogs and
ZombiU changes, and the accepted direct animated LEGO The Hobbit workaround. Private artifact overlays are
not needed to compile these fixes. Rejected experiments and capture-only instrumentation
are not part of the accepted release.

The 2026-11-01 source also contains the user-verified Matt Mii scratch-clear fix,
the accepted direct animated Hobbit workaround, and NBA movie image publication,
packing and native row dispatch. The full-source Release build was installed into
main/HOME/manual EXE/PDB pairs on 2026-11-01 at the user's explicit request to
promote first, then run automated checks. Original-scene replays remain pending.
The subsequent 2026-11-01 rebuilt Matt/Hobbit/NBA scene checks now pass with
explicit user confirmations and matching832A4984 logs. Broader original-game
coverage and the DSU transport gap remain open; this is not all-game verification.
The unused host row-reconstruction experiment is excluded: the successful private
NBA run exercised native row callbacks, not that math. Generic NBA synchronization
root cause remains unproven; do not credit the inactive optimization.

HOME/manual currently use separate supervised Cemu helper processes. Their native
UI functionality is working, but true in-process support is **not complete**.
The user's next architecture task after regression verification is an in-process
replacement with documented title-controlled HOME locks and foreground lifecycle.
The 2026-11-01 local SDK review records startup, pending-release and denied-event
policy gaps; it does not change the accepted source or release. Detailed references
and gates are in the workspace's `docs/IN-PROCESS-HOME-REQUIREMENTS.txt`.

## Rebuild

On the configured Windows development machine, run with PowerShell 7:

```powershell
./tools/build-accepted-release.ps1
./tools/test-accepted-fixes.ps1
```

The first command configures and builds the normal CMake/MSBuild Release target and
dependencies. It produces `build/validated-release/Cemu_release.exe` and its matching
PDB; it does not install the executable or overwrite user data. Logs go to a newly
dated source-build audit directory. The second command compiles and runs the included
regression tests against `src/`, with assertions enabled.

## Runtime Data

Emulator code fixes are compiled into the executable. Game profiles, graphic packs,
controller profiles, settings, games, updates, DLC and saves are external runtime data,
not embedded in that executable. Retain the existing Cemu user-data directory and the
distribution's game profiles when replacing or rebuilding the executable.

LEGO The Hobbit's accepted direct map-light stabilization deliberately modifies one
tightly identified animation using time-based smoothing, retaining light variation.
It no longer requires the older optional graphic pack. This remains an explicitly
accepted workaround, not a proven native renderer root-cause correction.

Resident Evil's latest accepted preparation improvements recommend Vulkan; OpenGL
with full sync at GX2DrawDone off remains a less-smooth fallback. Small first-use
hitches can remain, and neither a completely stutter-free cold cache nor a full
campaign replay is claimed. The
per-game GX2 full-sync option is available in Edit Game Profile > Graphics; its
OpenGL setting is included in the consolidated source.

## Verification Record

The 2026-10-25 shutdown follow-up is also in the consolidated source. Every idle CPU
core now observes the scheduler stop flag. On Windows, the DSU socket has a bounded
blocking receive; idle timeouts do not alter packet decoding, controller state or
motion processing. A lost shutdown wake-up datagram can no longer leave the reader
thread blocked indefinitely. No shutdown watchdog or new force-exit path was added.

The shutdown audit is `artifacts/shutdown-exit-20261025-dsu/`. Verification covers the
previously lingering NBA 2K13 X-button and Stop Game paths, fresh-process game-list
replacement, idle-window close, OpenGL Mii Maker close, 22 regression programs and
51 existing restart-handshake checks. The scheduler test compiles the exact production
idle function, including 1,000 repeated stop cases; the DSU tests verify packet delivery
after timeouts and shutdown with no wake-up packet. The 166 prior accepted source
hashes remain unchanged. This is targeted lifecycle coverage, not an exhaustive retest.

The later accepted native HOME/manual, persistent Start in Wii U Menu, controller
allocation, battery, Wiimote/MotionPlus/Nunchuk, DolphinBar speaker compatibility,
per-game GX2 full-sync and native amiibo-registration work is also in this source.
HOME/manual still use supervised helper processes; a single-guest-context native
system-applet implementation is not implied. NBA intro playback is privately
user-verified and now released, with rebuilt-scene replay pending; Hobbit's underlying renderer
cause remains unresolved despite the accepted workaround.

The final promotion audit is `artifacts/source-promotion-final-four-20261025/` in this
repository. It records the pre-merge source hashes, reviewed file deltas, rollback
sources, normal build log, regression results and cumulative accepted-source manifest.
Deployment hashes are also recorded in the task's final-four promotion report.
This is source/build verification plus the user's accepted game checks, not a new
full-campaign retest of every title.

## Wii U and 3DS Link

The reviewed same-PC/LAN link is self-contained in `src/wiiu_3ds_link`, with native
UDS integration and Tools menu enable/status controls. It is disabled by default.
The matching Azahar source is
the sibling `azahar source` folder. Both GitHub Builds release folders are rebuilt
from their respective updated source trees for this LAN promotion.

User-verified native flows are Mii Maker transfer, Sonic Lost World RC transfer,
Sonic Boom progress unlocks, Smash controller/custom-fighter transfer and regenerated
portraits, plus MH3U save transfer in both directions and a representative local
multiplayer session. LAN/IP/passphrase and a full Smash controller match were
verified in prior private two-PC builds. The rebuilt release has transport and
room-isolation checks, not a new hardware gameplay pass. Internet gameplay,
real-console interoperability, MH3U guild-card/DLC completeness and
four-player/stress coverage are not established.

Mii wrapping requires the user's own 16-byte `mii-wrap-key.bin` in Cemu's active
configuration directory. It is local runtime data, not included in source or the
executable. Automatic key extraction is not implemented. System titles, game data,
updates, saves and accounts remain external. No save validation is bypassed, and
MH3U's blank/correctly linked destination requirement still applies.

The verified Smash fix reads GPU portrait data back before the identified CPU copy.
The unexercised surface-copy/aperture experiments and temporary payload traces are
excluded. The original Matt Mii Maker face correction is now in source and
privately user-verified; its rebuilt original failing scene still needs checking.

## Regression Coverage

`test-accepted-fixes.ps1` now also runs the self-contained Mii/Hobbit/NBA checks and
retained game-policy programs. On 2026-11-01, 106 existing, nine new and 17 retained
programs passed after promotion: 132 of 133 attempted programs. The unchanged DSU
loopback delivery test timed out on its first packet, as before promotion; a
networking pass or cause is not established. No test was weakened to pass. Use
`-ContinueOnTestFailure` to collect the entire suite after a failed assertion;
the command still returns failure and records the failed program.
The old full-engine Sochi shader replay needs its proper engine/shader fixture,
not the standalone header harness. Wipeout 3's old timing-header prototype was not
an accepted patch; the archived ready record instead requires a listening check.

Source/test association coverage for 97 historically accepted games is recorded
in the task's `mii-hobbit-nba-promotion-20261101/per-game-audit`. This is not a new
in-game replay. The original failing scenes will be checked separately, starting
with rebuilt Mii Maker, Hobbit and NBA, with user navigation where needed.
