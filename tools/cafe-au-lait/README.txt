CAFE AU LAIT 0.2.2 - FULL MLC AND PRETENDO SETUP
Updated 2026-11-06. This section supersedes the v0.1 history below.

SLC PERMISSION CORRECTION - 2026-11-06
The repaired artwork package launched, but SLC mount returned -196634:
FS_ERROR_PERMISSION_ERROR (-0x3001A). V0.2.2 changes LOCAL_MOUNT (0) to
GLOBAL_MOUNT (2), matching libmocha/FTPiiU. Still uses our own unlocked FSA
client and private /vol/storage_cal_slc01 target. Existing mounts are never
replaced or unmounted. Normal owned-only cleanup remains; file opens stay
mode "r". No write/format/install/raw-device API is added. Global refers to
filesystem namespace visibility, not permission to upload/write files.
The icon and both splash images are byte-identical to the repaired package.
With Wii U OFF, replace the app folder from this ZIP. Keep the existing PC
program and retry a NEW full transfer. This mount change needs console testing.

ARTWORK PACKAGE CORRECTION - 2026-11-06
The first French bowl package omitted the 26-byte TGA 2.0 footer from all
three embedded images. Images now include the standard footer and dimensions;
iconTex.tga is 65580 bytes, not 65554. WUHB name, hash lookups and embedded
RPX bytes are verified. This packaging repair does not alter the SLC code.
With Wii U OFF, replace SD:/wiiu/apps/cafe-au-lait/ from this ZIP and restart.
Check that Cafe au Lait and the bowl icon appear, then open/B-exit before a
new transfer. Actual console recognition remains to be tested by the user.

HISTORICAL V0.2.1 SLC ACCESS - SUPERSEDED BY V0.2.2 ABOVE
The previous app assumed native-library SLC aliases were already accessible.
If those reads fail, v0.2.1 uses a normal FSA filesystem mount of /dev/slc01
at /vol/storage_cal_slc01 on its own client, using LOCAL rather than GLOBAL
mount flags. Only the six approved libraries are read; every file open uses
mode "r". No full SLC export, raw-device IO, formatting or file writes exist.
Only an owned mount is unmounted, after handles close and before client
deletion. Existing system/FTPiiU mounts are never unmounted or replaced.
Failure messages now name the library/path and numeric FSA status.
Replace the SD app folder and retry a NEW transfer. Keep the same PC program.
Build, ownership/cancellation mocks, source safety and package checks pass;
real-console SLC access/full transfer/Menu/Pretendo still require user testing.

SD ARTWORK UPDATE - 2026-11-05
User-selected French bowl artwork is embedded in the Aroma WUHB icon and
TV/GamePad launch screens. icon.png is also supplied for Homebrew Launcher.
The 2026-11-05 branding-only update left v0.2 code unchanged; the current
v0.2.1 package additionally includes the SLC access correction above.
Extract the refreshed ZIP to the SD root and replace only the app folder.

NEW FULL TRANSFER REQUIRED
1. With the console off, extract Cafe-au-Lait-Wii-U-private.zip to the SD root.
   Replace only SD:/wiiu/apps/cafe-au-lait/ with the new app files.
2. Launch Cafe au Lait through your existing Aroma setup. Select your linked
   PNID; enable Save password in its console account settings beforehand.
3. Open Cafe au Lait.exe on the PC. Choose storage folder if needed; space is
   checked for BOTH the received full dump and a separate private Cemu import.
4. Enter the Wii U IP, approve trusted-LAN use, compare the code and press A
   on the Wii U. Let the FULL transfer complete. B cancels safely.
5. Click Create private Cemu setup. Every manifest path/hash/directory is
   verified, then all files are placed automatically in a NEW private profile.
6. Exit Cafe au Lait and turn the Wii U off. Open private Cemu and test native
   Menu/Pretendo. Do not use the same PNID online on both at the same time.

FULL SCOPE AND PLACEMENT
Every file and directory under mounted /vol/storage_mlc01 is copied, including
all internal games, updates/DLC, saves, accounts, system titles/resources,
tickets, configuration and empty folders. NO system-title allowlist filters
the full export/import. Console originals and main Cemu remain unchanged.
External USB and disc data are separate volumes, not internal MLC. This is a
logical file copy, not a raw NAND image, firmware install or filesystem freeze.
Six selected native RPLs are read from existing system aliases or the owned
private-target GLOBAL SLC filesystem mount. No raw SLC access is introduced.

<storage>/NetworkDumps/<session>/ holds the verified source snapshot.
<storage>/PrivateImports/<session>/ is a NEW private Cemu setup:
- otp.bin and seeprom.bin go in its active user-data folder beside settings.
- COMPLETE MLC content goes under mlc01 with every original account ID/path.
- Account data stays at mlc01/usr/save/system/act/<original-id>/account.dat.
- Certificates/keys stay at mlc01/sys/title/0005001b/10054000/content/.
- Selected cafeLibs and OTP/SEEPROM are also placed for HOME/manual helpers.
The selected original account is configured for built-in Pretendo. No account
is fabricated, relinked, renamed or merged into the main installation.

COMPLETENESS AND STATUS
Protocol v2 includes directory records, per-entry SHA256, inventory checksum,
entry/byte counts and a completion marker. The importer compares the manifest
with ALL received MLC paths, not a filtered subset. Old partial dumps remain
preserved but are rejected for the new workflow. A NEW SD app/transfer is needed.
Synthetic full transfer/import/rejection tests and PowerPC build pass; the
final 0.2.2 console binary/full real transfer/Menu/Pretendo login need user testing.
Earlier 0.1 launch/partial-transfer results do not verify this final binary.
No zero-crash/brick guarantee is claimed. Read SAFETY.txt.
Full data includes credentials and private accounts/saves. Never share it.
Trusted-LAN private transfer is plaintext; never forward/expose TCP27581.
No Nintendo system binaries, accounts, games or saves ship in the SD ZIP.
Streaming uses32KiB; maximum250000entries/64MiB metadata/480UTF8bytes per path,
depth64, file4294967295bytes, total1TiB. Unsupported entries/bounds stop visibly,
not silently skip files. No console file-write/install/raw-device APIs exist.
Normal filesystem mount/unmount is limited to the owned SLC access described above.

PRIMARY REQUIREMENTS
https://pretendo.network/docs/install/cemu
https://github.com/PretendoNetwork/website/blob/master/content/docs/install/cemu.md
https://cemu.cfw.guide/using-dumpling.html#copying-online-files-to-cemu

V0.1 HISTORICAL NOTES BELOW - SUPERSEDED, NOT CURRENT INSTRUCTIONS
The following records preserve earlier failures and investigations only.

STATUS
The Wii U app, Windows wizard and private Cemu Tools entry are built.
LATEST OUTCOME: the 19:11 private log loads native nn_sl/ErrEula and still
reports 1579999. This setup is NOT Menu-ready. The earlier 925 matching system
files covered only the exporter's allowlist, NOT the fuller system MLC.
An unrestricted system-only comparison found 1,093 main system-title files
outside the private copy, including Browser, Notifications and Download
Management. Some other main-only paths are incomplete/alternate/legacy titles
and must not be copied blindly. Which omission, if any, causes this startup
failure remains unproven. See tests/latest-mlc-coverage.json and the full
all-system-titles.csv/all-system-files.csv report. No files were added by
that audit, and no emulator or console was run by the agent.
User verified normal Wii U launch/B-exit and a completed 846-file LAN transfer.
Full private import now completes: 846 system/account files and 259 runtime
files copied and SHA256 verified. The wizard restores the completed setup on
restart. Native Menu/online checks still need user verification. Automated
negative tests use synthetic data only.
It is not certified crash-free. Read SAFETY.txt before the first hardware test.
The main Cemu release, original accounts, games and saves are unchanged.

PC IMPORT CORRECTION - NO REPEAT DUMP OR SD UPDATE NEEDED
The Error applet has no content assets in its own title.fst. The old importer
incorrectly required a content folder after a files-only transfer. The importer
now accepts an absent folder only when the bounded file table proves no content
files are required; declared files must still exist with matching sizes.
Required empty content folders are created only in the NEW private import.
Close/reopen Cafe au Lait.exe to load the PC correction. The wizard reuses the
latest COMPLETED NetworkDumps session and enables Create private Cemu setup.
No manual folder creation, received-dump changes or Wii U app rebuild is needed.

UTF-8 RUNTIME CORRECTION
The initial PC import failed on a Unicode/RTL Arabic translation filename.
Windows PowerShell 5.1 now reads JSON/XML explicitly as UTF-8; package manifest
output includes a UTF-8 BOM. All runtime paths/hashes are checked BEFORE any
new account-data copy. Tests cover BOM-less UTF-8, BOM UTF-8, exact RTL filenames
and Unicode installation paths. A full real-data import has been completed
without launching Cemu or changing the received source/main installation.
Close/reopen Cafe au Lait.exe and click Open imported private Cemu; another
transfer or import is not required. The older failed partial copy stays marked
incomplete and is never selected. Do not use it or merge it into main.

MENU STARTUP DEPENDENCY FOLLOW-UP
The first imported Menu run stopped at its background with ErrEula157-9999.
The Menu executable matches the working main dump, but the minimal exporter
omitted Health and Safety Information (00050010/1004e100 for USA), which the
Menu can launch. The exporter now includes JPN/USA/EUR Health and Safety, and
fresh imports require that dependency rather than claiming Menu readiness.
For THIS private check only, 112 Health and Safety files were copied from the
user's existing owned local system dump into the PRIVATE MLC, with SHA256
verification. Main/recovery sources, accounts, saves and received dump unchanged.
Provenance is recorded in the private SYSTEM-DEPENDENCY-SUPPLEMENT.json.
The Health-only retest FAILED; that supplement did not establish the cause.
The latest trace shows unsupported nn_sl database initialization and ErrEula
drawing calls. The fonts, Menu save databases and native applet asset packs
were already present and readable. The private parent and HOME/manual helpers
were missing the native libraries used by the user's working main setup.
Exactly six owned RPLs have now been placed in their active data locations:
erreula, nn_sl, nsyskbd, snd_user, snduser2 and swkbd. All 30 parent/helper
copies match the working reference hashes; no other OS RPLs were injected.
The seven system application identities, 813 imported system-title files,
four fonts, Mii resources, keyboard/ErrEula packs and EULA/error messages were
checked. Main, original libraries, saves/accounts and received dump unchanged.
NATIVE-RUNTIME-AUDIT.json records the checks, not a successful Menu launch.
This is NOT a newly completed console transfer or proof that157-9999 is fixed.
Close/reopen the PC wizard and Open imported private Cemu. No repeat dump or
SD update is needed for THIS repaired setup. Limited private file/OS/API logs
remain enabled; the failed trace is in menu-diagnostics/before-native-runtime.log.
No emulator launch, desktop control or new game capture was performed by agent.
Fresh exports/imports now require those six libraries as well as Health and
Safety. The console exporter reads only their fixed paths under existing
mounted SLC aliases; no mounting, raw storage or writes were introduced.
Those additional console reads are built/audited but NOT hardware verified.
No owned system binaries are included in the distributable package.
Current automated checks:172 import,28 content-free title,131 transfer and
100041 native protocol/hash/fuzz checks. Subsequent log confirms native loading
but continued Menu failure; these passing checks do NOT prove dump completeness.

REQUIREMENTS
- Your own already-modded Wii U with compatible Aroma/Tiramisu/Mocha services.
- A working Wii U GamePad for the app's account selection/approval controls.
- Windows PC and Wii U on the SAME trusted private subnet, not guest Wi-Fi.
- Your existing console account/online setup, if online access is wanted.
This tool does not install CFW, create an account or configure a console PNID.

FIRST HARDWARE CHECK: LAUNCH AND EXIT ONLY
1. Close Wii U software normally. With the console off, add the contents of
   Cafe-au-Lait-Wii-U-private.zip to the SD root. It adds only the separate
   wiiu/apps/cafe-au-lait folder. Do not format, erase or replace other folders.
2. On Aroma, launch Cafe au Lait's WUHB. The RPX is included for an existing
   compatible Homebrew Launcher environment; use the appropriate ONE format.
3. At the account-selection screen press B. Confirm a normal return to your
   console's menu/launcher. Do this before attempting a transfer.
If startup or exit fails, stop and report exactly what appeared. Do not install
other firmware, retry through a crash loop or follow recovery steps from guesses.

TRANSFER AND AUTOMATIC PLACEMENT, AFTER LAUNCH/EXIT PASSES
1. Open the Wii U app again. Select the intended account with left/right,
   then A. Leave its displayed IP address on screen.
2. Run Cafe au Lait.exe in this folder. Alternatively use Start-Private-Cemu.cmd
   and Tools > Cafe au Lait... in the private Cemu game-list window.
3. Enter the Wii U IP. Leave region Auto unless explicitly selecting your own
   JPN/USA/EUR region. Check the trusted-LAN warning and click Connect to Wii U.
4. Compare the 16-character code on BOTH screens. Only if it matches your PC,
   press A on the Wii U to approve. B rejects/cancels; no export starts without A.
5. Once transfer and validation pass, click Create private Cemu setup. You can
   close/reopen the PC wizard to reuse its latest completed transfer. Files
   are automatically placed in a NEW PrivateImports session. Nothing is merged
   into the main Cemu installation during this test.
6. Click Open imported private Cemu to check Wii U Menu, Mii and System Settings.
   For the private keyboard GamePad: Enter=A, Escape=B, arrows=D-pad,
   Space=Plus, H=HOME. Automatic controller assignment is disabled in this new
   test profile; your regular controller profiles are not copied or changed.

WHERE THE IMPORT PUTS DATA
PrivateImports/<new-session>/otp.bin and seeprom.bin: beside private Cemu.
PrivateImports/<new-session>/mlc01/sys/title/: approved system apps/resources.
PrivateImports/<new-session>/mlc01/usr/save/: approved system-app/Mii state
and usr/save/system/act/<original-account-ID>/account.dat and related files.
The original account ID is retained. It is not renamed to 80000001.
Private settings and HOME/manual helpers point ONLY to that imported MLC.
The six selected cafeLibs RPLs are installed for both parent and helper data
paths. No parent portable directory is created or user-data routing changed.
No game content, game saves, original game paths or original controller
profiles are copied. No dump data needs manually moving to another folder.
This test opens a new system-only setup; it does not contain your game library.

ONLINE AND PRIVACY
The import preserves your own existing identity/account files. Select the
intended network in THIS private Cemu's account settings before online checks.
File validation is structural, not proof of successful service authentication.
It does not implement unsupported applets or make every Wii U service work.
NetworkDumps and PrivateImports contain console/account credentials. Keep them
private. Never upload, commit or share them; do not attach them to bug reports.
This user-approved PRIVATE test transfers plaintext like FTP. No encryption or
cryptographic peer authentication is claimed. The comparison code provides
physical approval, not protection from an active attacker on the LAN.
Do not forward TCP 27581, expose it online or use untrusted/public Wi-Fi.
No general FTP service, incoming uploads or remote path commands are provided.

FAILURE/CANCELLATION
Interrupted/corrupt transfers cannot receive a valid completion marker and
cannot be imported. Partial data stays in a new private PC folder only.
An interrupted import is unusable without IMPORT-COMPLETE.json.
No existing files are overwritten; there is no automatic merge or rollback
operation against the console or your main Cemu profile.
Do not move a completed private import without updating its private MLC path;
the launcher refuses an unexpected path instead of falling back to main data.

SCOPE AND BUILD EVIDENCE
This is new read-only network code, based on Dumpling's dump layout/scope,
using WUT/libmocha APIs also used by established Wii U homebrew. It is NOT an
unchanged Dumpling binary or an existing FTP server merely given a new name.
Read SAFETY.txt, THIRD-PARTY.txt and private-release.json for precise limits.
The private Cemu emulation source matches main; only its Tools entry and
private version label differ. Current helper-process HOME/manual is retained.
Builds/tests live in console-build, cemu-build and tests. No hardware test or
new in-game regression replay is claimed by their automated pass results.
