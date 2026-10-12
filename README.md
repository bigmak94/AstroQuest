# AstroQuest

**ASTRO BOT Rescue Mission (PS4 / PlayStation VR) in virtual reality on Meta Quest 3**, played
from your own copy of the game through a PS4 emulator. Ways to play:

- **On the headset alone**: an app for the Quest 3 runs the emulator on the headset itself. No
  PC is needed once it is installed. Please note: while the standalone Meta Quest 3 build is
  fully functional and playable, the headset’s mobile GPU is pushed to its limits. As a result,
  native standalone visuals run at lower resolutions and visual fidelity compared to the original
  PS4 hardware or the PC VR mode.
- **On a Windows PC, shown in the Quest through Virtual Desktop**: the PC runs the game at the
  console's 60 frames a second and at up to six times its resolution.
- **On a Windows PC with a Valve Index through SteamVR**: the same PC build uses SteamVR's
  OpenXR runtime and a DualSense connected to the PC. Headset playback has been reported working; see
  [the Index setup](README-PC-VR.md#valve-index-through-steamvr), including controller
  positional-tracking limits. Other PC headsets go the same way, through SteamVR or an OpenXR
  runtime of their own: a Bigscreen Beyond and a Pimax Dream Air have been reported working.

The emulator is [shadPS4](https://github.com/shadps4-emu/shadPS4) (its ARM64 build,
[zenithblue-oss/shadps4-arm64](https://github.com/zenithblue-oss/shadps4-arm64), on the
headset), to which this project adds what the game needs from a PlayStation VR: the headset,
its tracking and reprojection, the camera's view of the controller (taken from the Quest's
hand tracking), the 3D sound engine, the microphone, and fixes for the game's timing and
resolution under emulation.

> **No game files are included or distributed.** You need your own copy of ASTRO BOT Rescue
> Mission, dumped from your own PlayStation 4.

| | PlayStation 4 + PS VR | Quest 3 on its own | PC + Virtual Desktop |
| --- | --- | --- | --- |
| Frames a second | 60 | 30 in levels, 45 in lighter scenes | 60 (more if you ask for it) |
| Picture per eye | 960x1080 to 1440x1536 | 816x870 to 1440x1536 | 1440x1536 up to 3600x3840 |
| Edges | multisampled | smoothed by a filter | multisampled |

## Status

Work in progress, built and tested by one person on one Quest 3 and one PC. World 1 & 2has been 
played through on the Quest 3 on its own; the PC path has been played
over Virtual Desktop and fixed since. Expect rough edges, and please report what you find.

## What you need

- **A Meta Quest 3, or a Valve Index connected to a Windows PC running SteamVR.**
  (The Quest 3S has the same chip and should work, but nobody has tried.)
- **Something to play with.** A PS5 DualSense controller is best: it stands in for the PS4
  controller the game expects with everything that one has (buttons, sticks, touchpad, motion
  sensors, rumble and light bar; a DualShock 4 should do the same, untested). Without one,
  **the Quest's own Touch controllers play the game**, on the headset alone as on the PC.
  Other gamepads play too (Xbox, 8BitDo and the like): what they lack of a PS4 controller,
  the touchpad and the motion sensors, is made up for, see "Playing" below.
- **ASTRO BOT Rescue Mission, European release CUSA12392, version 1.00 or 1.04** (the game
  as on its disc, or with its last update), dumped from your own console and game: either as
  the game's folder (the one with `eboot.bin`, `sce_sys`, `sce_module` in it, about 13 GB) or
  as the `.pkg` package made from the dump, which the PC launcher unpacks by itself. (A
  package downloaded from the PlayStation Store is encrypted and cannot be used; a package
  that is only the game's update is not the game: unpacked into a folder named
  `CUSA12392-UPDATE` next to the game's `CUSA12392`, it is played over it.) Other regions and
  the versions in between are untested, and the fixes for the game's timing and resolution
  only apply to these two.
- To install on the headset: a computer with
  [adb](https://developer.android.com/tools/releases/platform-tools) (or
  [SideQuest](https://sidequestvr.com/)) and the headset in
  [developer mode](https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/#enable-developer-mode)
  (set in the Meta Horizon app on your phone; on Windows, adb may also need Meta's ADB
  driver, described on the same page).
- To play on the PC: Windows 10 or 11 (64-bit), a graphics card with Vulkan 1.3, 16 GB of
  memory or more (the emulator asks Windows for about 14 GB), the
  [Microsoft Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe)
  (the launcher says so if it is missing), and [Virtual Desktop](https://www.vrdesktop.net/)
  (the app on the Quest, the Streamer on the PC).

## Installing: Quest 3 on its own

Download `AstroQuest-<version>-Quest3.apk` from the
[latest release](https://github.com/bigmak94/AstroQuest/releases/latest), then:

1. **Install the app.** Connect the headset to the computer with a USB cable, allow USB
   debugging in the headset, and run

   ```sh
   adb install -r AstroQuest-0.20-Quest3.apk
   ```

   (or drag the APK onto SideQuest).
2. **Copy the game to the headset**, the folder named `CUSA12392` (this takes a few minutes):

   ```sh
   adb push CUSA12392 /data/local/tmp/astro/games/
   ```

   The app also finds it in `/sdcard/Android/data/com.astrobotquest.vrhost/files/games/CUSA12392`,
   but that folder is deleted with the app if it is ever uninstalled.

   **If your game is a `.pkg` file**, unpack it on a PC first: the headset needs the game's
   folder. Two ways:

   - **The easy one, on Windows**: download `AstroQuest-<version>-PC-VR-Windows.zip` from the
     same release, unzip it, put your `.pkg` in its `games` folder and start
     `Play Astro Bot VR.bat`. It offers to unpack the package (about a minute, 13 GB); you can
     close it after that. `games\CUSA12392` is then the folder to copy to the headset. You do
     not need Virtual Desktop or a VR-ready PC for this.
   - **By hand, with the tool it uses**: [PkgTool](https://github.com/maxton/LibOrbisPkg/releases/tag/v0.2)
     (`PkgTool-0.2.231.zip`, free and open source; Windows, or the `PkgTool.Core` builds for
     Linux and macOS). `PkgTool pkg_extract --passcode 00000000000000000000000000000000 game.pkg out`
     puts the game in `out/uroot`: rename that folder `CUSA12392`. Then add the game's
     description, which the package keeps apart: `PkgTool pkg_listentries game.pkg` shows the
     number of `PARAM_SFO`, and `PkgTool pkg_extractentry game.pkg <number> CUSA12392/sce_sys/param.sfo`
     writes it. Without `param.sfo` the emulator does not recognise the game.

   Either way, only a package made from a dump of the game can be unpacked; one downloaded
   from the PlayStation Store is encrypted.
3. **If you play with a gamepad, pair it with the headset**: Settings > Bluetooth > Pair (on
   a DualSense, hold Create and the PS button until the light bar flashes). The Touch
   controllers need nothing.
4. **Start "Astro VR Host"** from the App Library, under *Unknown Sources*. The first start
   takes a few seconds longer (it unpacks the emulator) and asks for the microphone: the game
   listens to it for blowing at things, as it did with PlayStation VR's.

Playing:

- The app begins with a menu for the field of view: a stick to a side chooses, ✕ / A or a
  trigger starts the game.
- **With a gamepad**: put the Touch controllers aside and hold it. The headset tracks your
  hands around it, and that is where the controller is in the game. A gamepad without motion
  sensors, which is every one that is not a PlayStation's, is turned and tilted in the game
  by your hands, as far as the headset sees them.
- **With the Touch controllers**: the right one is the controller in the game, where it is
  and the way it points. Left stick to move; A is ✕, B is □, X is ○, Y is △; the left
  controller's menu button is OPTIONS. X and Y together blow, where the game wants you to
  blow into the microphone.
- **The touchpad, on the Touch controllers and on a gamepad that has none**, is on buttons
  (a card in the headset says so when the game starts, and again whenever you press
  OPTIONS):
  - **right trigger (R2): press it and hold**: the water cannon, the machine gun
  - **right grip (R1): swipe forward**: the hook, the throwing stars, the chests
  - **left trigger (L2): pull back, let go to shoot**: the catapult at the end of every
    level (point the controller at the goal)
  - the right stick is a finger on it, for anything else.
- With both at hand, the game is played with the one you used last: a button on the other
  changes over.
- **To turn round without turning yourself** (on a seat that does not turn): hold the left
  grip, or L1 on a gamepad, and flick the right stick to a side. Each flick turns the view 30
  degrees that way; resetting the view faces you straight ahead again.
- **The game speaks the headset's language**, of those it has (28; English otherwise).
- The first screen asks to move the controller into a floating outline: hold it up in front
  of you where the outline is.
- On the world map, **look** at a planet and press ✕.
- **Reset the view** by holding OPTIONS for a second (or pressing the PS button; with the
  Touch controllers, hold the left menu button for a second or press both sticks in)
  whenever you sit differently and things are too close, too far or off to one side.
- Taking the headset off pauses the game.
- Settings (refresh rate, resolution, field of view, sharpening, microphone gain...) go in
  `/sdcard/Android/data/com.astrobotquest.vrhost/files/vrhost.txt`; all of them are explained in
  [README-QUEST-VR.md](README-QUEST-VR.md).

Your saves are kept in `/sdcard/Android/data/com.astrobotquest.vrhost/files/data/shadPS4/home/1000/savedata`.
Updating the app with `adb install -r` keeps them; **uninstalling deletes them**, so copy that
folder off the headset first (`adb pull`).

## Installing: PC VR through Virtual Desktop

Download `AstroQuest-<version>-PC-VR-Windows.zip` from the
[latest release](https://github.com/bigmak94/AstroQuest/releases/latest), then:

1. **Unzip it** into a folder with a short path, e.g. `C:\Games\AstroQuest`. Some of the
   game's files have long names, and the emulator cannot open a file whose full path is
   longer than Windows' 260 characters: keep the path of the folder that holds `games` under
   about 110 characters.
2. **Put your copy of the game in its `games` folder**, anywhere in it: the game's folder
   (the one with `eboot.bin` in it) or its `.pkg` file. A package is unpacked the first time
   you start, which takes a minute or so and about 13 GB. You can also leave the game where
   it is: when the launcher finds none, a window asks where it is and remembers the answer.
3. **Set up Virtual Desktop**: install the Streamer on the PC and, in its Options, choose
   **VDXR** as the OpenXR runtime. In the headset, in Virtual Desktop's Streaming settings,
   set the frame rate to **120** (the game then runs at 60 frames a second, as on the console).
4. **Connect the DualSense to the PC**, by USB cable or by Bluetooth paired with the PC, not
   with the headset: paired with the headset, it reaches the PC without motion sensors or
   touchpad. The Touch controllers play as well: with no gamepad on the PC, and with one
   whenever they were the ones used last.
5. **Connect to the PC with Virtual Desktop**, then start **`Play Astro Bot VR.bat`** on the
   desktop you see in the headset. The first time, it offers to unpack the game if it is a
   package (or asks where the game is). Then a small window lets you choose each eye's resolution, the
   frame rate, the game's language (Windows' own unless you choose another) and the field of
   view; Play starts the game, and the headset switches to it after a few seconds.

For the controller to be placed by your hands, turn on hand tracking in the headset and let
Virtual Desktop forward tracking data to the PC. The Touch controller layout, all settings and
what to do when something does not work are in [README-PC-VR.md](README-PC-VR.md).

## Common questions

**How do I get the game onto my computer?** This project gives no information on how to get
games, and this repository does not include any game data.

**Which headsets?** On the headset alone: Quest 3 (the Quest 3S has the same chip and should
work; nobody has reported it yet). A Quest 2 or Pro has not been tried and is slower. On a
PC: any headset with an OpenXR runtime on Windows (or SteamVR on Linux, see below), which is every Quest through Virtual
Desktop or Steam Link, and PC headsets through SteamVR (Index, PS VR2 with its PC adapter,
Bigscreen Beyond, Pimax and others have been reported working).

**Do I need a PlayStation controller?** No. The headset's own controllers play, on the
headset alone and on the PC, and so do other gamepads; see "Playing". A DualSense is still
the closest to what the game was made for.

**Without Virtual Desktop?** On the PC, yes: with SteamVR as the OpenXR runtime
([README-PC-VR.md](README-PC-VR.md#valve-index-through-steamvr)). Start SteamVR first, then
`Play Astro Bot VR.bat`.

**The picture looks like it is "reprojected", whatever I set.** The game draws one frame
for every two refreshes of the headset, as it does on a PlayStation VR: 60 frames a second at
120 Hz, 45 at 90 Hz, 36 at 72 Hz, and your head's turning is filled in between. Set the
headset to 120 Hz for the console's own 60. On a fast PC, "Frames a second, at most" in the
launcher's window can be raised to the headset's rate.

**The game is in English.** From 0.19 on it takes the language of Windows or of the headset
(one of the game's 28; English otherwise); the PC launcher's window has a list to choose
another from.

**The launcher keeps asking for the Visual C++ runtime although it is installed.** Fixed in
0.19 (it looked in the wrong place when it was started from a 32-bit program), and its
question now has a "start the game all the same".

**Linux?** The emulator builds natively on Linux, with no release package yet; how to build
and start it, and the SteamVR setting it needs, are in
[README-PC-VR.md](README-PC-VR.md#linux-native-build). A player also reports the Windows
build running through Steam's Proton with WiVRn; their recipe is in
[README-PC-VR.md](README-PC-VR.md#linux-through-proton-as-reported).

## Building from source

Clone with the submodules:

```sh
git clone --recurse-submodules https://github.com/bigmak94/AstroQuest.git
```

Everything builds on Windows from Git Bash, without admin rights, with a portable toolchain
kept in `tools/` (not in the repository): LLVM/clang 21 (`tools/llvm`), CMake 4.4
(`tools/cmake`), Ninja (`tools/ninja`) and the MSVC CRT and Windows SDK fetched with
[xwin](https://github.com/Jake-Shadle/xwin) (`tools/winsdk`).

- **PC build**: `source tools/env-win.sh && cmake --build build/win-x64 --target shadps4`, then
  `bash tools/make-pc-vr.sh` puts it in `pc-vr/`.
- **Quest app**: `bash tools/build-arm64.sh` cross-compiles the emulator for the headset
  (Debian 13 arm64 sysroot made by `tools/mk-sysroot.mjs`, FEXCore built by the upstream
  `shadps4-arm64-main/runtime/scripts/build-fexcore-smoke-aarch64.sh`), then
  `bash quest-host/build.sh` builds the APK. That needs the Android SDK (build-tools 36.1,
  platform 36), an NDK, a JDK, the
  [Khronos OpenXR loader for Android 1.1.63](https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/1.1.63/)
  (the `.aar` unzipped into `tools/openxr`), and the
  [Bachata S4 v0.2.4](https://github.com/JICA98/Bachata-S4/releases/tag/v0.2.4) release APK
  (`bachatas4-0.2.4-release.apk`, SHA-256 `4077b0d0b80e354c71eff1ce2c003f122449ac266eb27a6ac26744d3f9809a08`)
  in the top folder, whose Linux runtime (glibc, its libraries and the Turnip Vulkan driver)
  the app carries.
- `bash tools/make-release.sh <version>` packs the release files into `build/release/`. It
  puts [PkgTool 0.2.231](https://github.com/maxton/LibOrbisPkg/releases/tag/v0.2), unzipped
  into `tools/pkgtool`, in the PC package: the launcher unpacks a game package with it.

How it all works, and the tools used to test it on the headset and with a simulated one, is
described in [README-QUEST-VR.md](README-QUEST-VR.md) and [README-PC-VR.md](README-PC-VR.md).

## Support the project

If AstroQuest gave you a good time in VR and you want to help it go further, you can support
its development here:

[![Support AstroQuest](https://img.shields.io/badge/Support-AstroQuest-635BFF?style=for-the-badge&logo=stripe&logoColor=white)](https://buy.stripe.com/6oU4gB5450PZ18h8aK1wY00)

**https://buy.stripe.com/6oU4gB5450PZ18h8aK1wY00**

## License

AstroQuest is free software, licensed under the
[GNU General Public License, version 2 or (at your option) any later version](LICENSE)
(GPL-2.0-or-later), the license of shadPS4 it is built on.

The third-party components it uses or ships keep their own licenses: among them FEX (MIT),
Mesa's Turnip Vulkan driver (MIT), the GNU C Library (LGPL-2.1-or-later), PkgTool (LGPL-3.0), the Khronos OpenXR
SDK (Apache-2.0) and the libraries under `shadps4-arm64-main/externals`. See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for what the release files contain and where
the source of each part is.

ASTRO BOT, PlayStation and PlayStation VR are trademarks of Sony Interactive Entertainment.
This project is not affiliated with, endorsed or sponsored by Sony Interactive Entertainment,
Team Asobi or Meta. It contains no game, firmware, keys or other copyrighted console files:
use it only with software you own and have dumped yourself.

## Thanks

- **The [shadPS4](https://github.com/shadps4-emu/shadPS4) team and contributors.** None of
  this would exist without their PlayStation 4 emulator: everything here is built on top of
  their years of work. Thank you!
- [zenithblue-oss/shadps4-arm64](https://github.com/zenithblue-oss/shadps4-arm64) and
  [Bachata S4](https://github.com/JICA98/Bachata-S4), for the ARM64 build of shadPS4, its FEX
  integration and the Linux runtime it runs in on Android.
- [FEX-Emu](https://github.com/FEX-Emu/FEX), which runs the game's x86-64 code on the
  headset's ARM processor.
- [Mesa](https://www.mesa3d.org/) and its Turnip driver for Adreno GPUs (the build used here
  comes from [Vauzi-17/mesa-tu8](https://github.com/Vauzi-17/mesa-tu8)).
- [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg), whose PkgTool unpacks game packages
  for the PC launcher.
- Everyone who reported what went wrong, and those who sent the fix along:
  [ODevStudio](https://github.com/ODevStudio) for SteamVR and Valve Index support and the
  desktop's spectator views, [Clodo76](https://github.com/Clodo76) for finding the game's
  1.04 executable from inside, [evertec82](https://github.com/evertec82) for what stopped the
  game under SteamVR.
- [The Khronos Group](https://www.khronos.org/) for OpenXR and Vulkan,
  [vgmstream](https://github.com/vgmstream/vgmstream) for documenting Sony's audio formats,
  and [Virtual Desktop](https://www.vrdesktop.net/) for the PC streaming path.
