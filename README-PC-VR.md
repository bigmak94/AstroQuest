# Astro Bot Rescue Mission on PC VR

## Valve Index through SteamVR

Use the same Windows PC build and **Play Astro Bot VR.bat** with a Valve Index. You do
not need Virtual Desktop. Headset playback with a PC-connected DualSense was reported
working on 2026-10-05. The Index-controller fallback and Quest/VDXR regression still need
physical headset tests.

What is said here of an Index holds for the other headsets SteamVR drives, and for those
with an OpenXR runtime of their own: players have reported a Bigscreen Beyond (SteamVR) and
a Pimax Dream Air (Pimax's runtime, and SteamVR) working. The launcher uses whatever
runtime is the PC's active one.

1. Start SteamVR with the Index and base stations connected. In **Settings > OpenXR**,
   select **Set SteamVR as OpenXR Runtime**. The launcher uses the active runtime without
   changing it. An `XR_RUNTIME_JSON` environment variable overrides that selection; remove
   a Virtual Desktop or simulator override before launching with SteamVR.
2. Set the Index refresh rate to **120 Hz** in SteamVR's Video settings. Keep `fps=60`
   in `pc-vr\settings.txt` for the console's frame rate. At 90 Hz the default runs at 45
   frames a second, at 80 Hz at 40, and at 144 Hz at 48.
3. Connect the **DualSense to the PC** by USB or Bluetooth. Buttons, touchpad, gyro,
   rumble and light bar use the existing gamepad path. If you add a Steam shortcut, disable
   **Steam Input for that shortcut** so Steam does not replace it with a virtual Xbox pad.
4. Select the Index speakers and microphone in SteamVR's Audio settings, or make them
   Windows' default output and input devices. The game needs the microphone for blowing.
5. Put your own game dump in `games`, or choose it when the launcher asks. Start
   **Play Astro Bot VR.bat**, choose the graphics settings, and press Play. Hold OPTIONS
   for a second or press the PS button to recenter after sitting down.

The Index tracks your head in six degrees of freedom, but does not track bare hands or the
DualSense's position. Without a tracked position, the virtual gamepad rests in front of you
and follows your seating position; the DualSense gyro controls its rotation. Moving the
physical gamepad alone does not move its virtual position. The initial controller-alignment
screen uses the existing untracked-gamepad fallback. Set `hands=0` if parked VR controllers
cause an incorrect gamepad position. **You can put that gamepad where you want it**: see
"The gamepad in the game, where nothing tracks it" below.

Without a gamepad connected, Index controllers can use the existing VR-controller fallback:
right A is cross, right B is square, left A is circle, left B is triangle, and pressing the
left trackpad is OPTIONS. Sticks, triggers, grips, rumble and recentering follow the Touch
layout below. The connected DualSense takes priority over VR controllers.

The renderer covers the Index's canted eye views with parallel projections, as required by
the emulated PSVR game. SteamVR handles the final reprojection into the headset's eye views.
The saved headset FOV is reused only when the OpenXR runtime (including its version), headset
name and vendor match. With no confirmed headset identity or matching cache, the title uses
the PSVR default until the next game start; legacy caches without an identity are ignored.

## Quest through Virtual Desktop

The second way to play (the first, the app that runs on the headset itself, is in
`README-QUEST-VR.md`): the emulator runs on this PC, and the picture goes to the Quest 3 the
way any PC VR game does, through Virtual Desktop. The PC has far more to give than the
headset: the game runs at the console's own 60 frames a second with the console's
multisampling, and draws far larger than the console ever did: 2880x3072 an eye by default,
four times the pixels of its largest size (1440x1536, what a PlayStation 4 Pro uses), chosen
in a small window at every start.

## Playing

1. **The controller goes to the PC**, not to the headset: a USB-C cable, or Bluetooth (hold
   Create and the PS button until the light bar flashes, then add the device in Windows'
   Bluetooth settings). A DualSense remembers one partner: after playing on the headset's own
   app it has to be paired with the PC again, and the other way round. Connected to the PC it
   gives everything the game uses: buttons, sticks, touchpad, motion sensors, rumble, light bar.
   **Paired with the headset instead, it does not work properly**: Virtual Desktop then hands
   the PC a copy of it, a plain DualShock 4 without motion sensors and without touchpad - the
   controller in the game neither turns nor follows you, and the end of a level (swiping Astro
   out of the controller) cannot be done. That is what happened in the first session on
   2026-10-02 (the log says "PS4 Controller"; the console window now warns about it).
   **The headset's own controllers play as well**: with no gamepad on the PC, and with one
   whenever they were used after it (see below).
2. **Virtual Desktop Streamer** has to be running on the PC (the launcher starts it if it is
   not), with its own OpenXR runtime, VDXR, as the PC's OpenXR runtime (Streamer window,
   Options). It is, on this PC.
3. In the headset, start **Virtual Desktop** and connect to the PC. In its Streaming settings
   the frame rate to pick is **120**: the game then draws 60 frames a second and each is
   shown for two refreshes, as on a PlayStation VR. (At 90 it draws 45, at 72 36: see "Speed".)
4. On the desktop you now see in the headset, start **`Play Astro Bot VR.bat`** (in this
   folder). It looks for the game in the `games` folder next to it, up to three folders
   down: an unpacked game (a folder with `eboot.bin` in it) or a `.pkg` package, which it
   offers to unpack there first (PkgTool does it, in about a minute; only a package made
   from a dump can be unpacked, not an encrypted one from the PlayStation Store). When it
   finds neither, a window asks where the game is, and what is chosen there is kept as
   `game=` in the settings. A small window comes up next: the **resolution** of each eye (a slider, from the
   console's 1440x1536 up to 3600x3840), the **most frames a second**, the **language of the
   game** (Windows' own, or one of the game's 28), and the **field of
   view**; Play starts the game with them (they are kept in `pc-vr\settings.txt`; untick "Show
   this window at every start" to go without it). A console window then says what is found
   and what happens; the game's window opens behind it and shows both eyes' pictures side by
   side, and after a few seconds the headset switches from the desktop to the game.
5. Pick up the controller. Where it is in the game comes from your hands: put the headset's
   own controllers aside (hand tracking on in the headset) and the headset sees your hands
   around the gamepad. Virtual Desktop has to pass hand tracking on to the PC (its setting for
   forwarding tracking data); where it does not, the emulator takes the places Virtual
   Desktop gives the Touch controllers it makes up from the hands while the real ones lie
   unused. With neither, the controller floats at a fixed spot in front of you and only turns
   with the gamepad's motion sensors. The console window says which it is ("The gamepad is
   placed in the game by where the hands are, which ...").

The other order works too: start the game at the PC and put the headset on afterwards. The
game waits up to a minute for the headset (`wait` in the settings) and starts in it; after
that it starts on the monitor and moves to the headset whenever Virtual Desktop connects.

For people watching on the monitor, choose **Desktop view > Single eye (spectator)** in the
launcher. It shows the complete left-eye picture with its proportions kept, leaving black
bars where needed. The headset still gets both eyes
at full resolution; the desktop reuses an existing eye image rather than rendering another
camera. It follows the player's head, not a separate spectator camera. **Stereo (both eyes)**
restores the original desktop view and remains the default. This setting is for the PC build;
the standalone Quest app and external VR-host transport keep their stereo output.

In the game:

- The first screen asks to move the controller into a floating outline. Hold the controller
  up in front of you, where the outline is. (A gamepad without hand tracking is assumed to be
  there and the screen passes on its own.)
- The world map is selected by **looking** at a planet and pressing ✕.
- **Resetting the view**: hold OPTIONS for a second (as on a PlayStation VR), or press the PS
  button. Where your head is then is where you sit as far as the game goes, and the way you
  face is straight ahead. The emulator does it by itself when ✕ is pressed for the first time
  after the headset came up, and when the headset's own "reset view" is used (hold the Meta
  button).
- **Turning round without turning yourself**, for whoever sits where they cannot: hold
  **L1** (with the headset's controllers: the left grip) and flick the right stick to a
  side. The view turns 30 degrees that way with each flick, about your head: you stay where
  you are in the game and face another way, and a gamepad that nothing tracks comes round
  with you. The game has no use for L1 while it is played. Resetting the view faces you
  straight ahead again. (`turn=45` in the settings for another step, `turn=0` for none.)
- **A controller without a touchpad** (an Xbox pad, the headset's own controllers) has
  buttons for what the game wants done on one:
  - **right trigger (R2): the pad pressed**, for as long as the trigger is pulled. That is
    how the water cannon and the machine gun fire.
  - **right shoulder button (R1; the right grip of VR controllers): a swipe forward**, once
    for each press. That shoots the hook, throws the stars, opens the chests.
  - **left trigger (L2): a pull back** that is held while the trigger is, and let go when it
    is. That is the catapult at the end of every level: point the controller at the goal,
    pull, let go. It also pulls on the hook's rope.

  The game has no use for these buttons while it is played, and is still told of them (a few
  of its menus have). Besides them, the right stick is a finger on the pad, for anything
  else: it comes down behind the pad's middle, is dragged the way the stick is pushed, and
  lifts where the stick was let go; the Back / View button presses the pad. The right stick
  does that with a DualSense as well while nothing touches its pad. (A stick that drifts,
  never resting at its centre, is left out: it would hold a finger on the pad for good.)
- **Where the game has you blow** into the controller's microphone: blow at the headset's
  microphone, or hold the **PS button and □** together, which blows for as long as they are
  held. The console window says how loud the game heard the last ten seconds whenever it
  heard something ("Microphone: the loudest ... was -18 dB"): the game takes -21 dB for
  blowing at half strength and -9 dB for all of it. If blowing gives too little, `mic_gain`
  in the settings makes the microphone louder for the game.
- **Taking the headset off pauses the game**, the way the console does it: the picture goes
  black and the game waits; it goes on where it was when the headset is back on. The same
  happens while Virtual Desktop shows the PC's desktop instead of the game, and when the
  connection to the headset breaks: the game waits, and goes on when it is shown again.
  (If it ever waits although the headset is on and shows the game: `pause=0`.)
- To quit, close the game's window (or the console window).

### The gamepad in the game, where nothing tracks it

Without tracked hands (Virtual Desktop without hand tracking passed on, SteamVR, Pimax...),
the gamepad in the game hangs before you: 17 cm below your eyes and half a metre ahead,
which is where the game looks for it when it starts. That is in the way of the view for
some, and too high or too low for what the controller has to be held to elsewhere. **Hold
the PS button** and

| press | to |
| --- | --- |
| D-pad up, down, left, right | move the gamepad that way, 2 cm a press |
| L1, R1 | bring it nearer, push it farther |
| △ | switch between your place for it and the standard one |

The first of these puts the gamepad at your own place (30 cm below the eyes and 45 cm ahead
until you move it) and every press is kept for the next time (`pc-vr\user\vr_controller.json`).
Every start begins at the standard place, because the game's first screen needs the
controller there: PS + △ brings it to yours afterwards. The PS button pressed and let go by
itself still resets the view. With tracked hands, the hands say where the gamepad is as
long as they are seen; your own place counts while they are not.

### With the headset's own controllers

The two Touch controllers are the gamepad while no gamepad is connected to the PC, and,
while one is, from the moment they are used:

| Touch controller | DualShock 4 |
| --- | --- |
| left stick | left stick (move) |
| A (right hand) | ✕ (jump, confirm) |
| B (right hand) | □ (punch) |
| X (left hand) | ○ (back) |
| Y (left hand) | △ |
| right trigger | the touchpad pressed, for as long as it is pulled (water, guns); also R2 |
| right grip | a swipe forward on the touchpad, once (hook, stars, chests); also R1 |
| left trigger | a pull back on the touchpad, let go when the trigger is (the catapult at the end of a level); also L2 |
| right stick | a finger on the touchpad, dragged the way the stick is pushed |
| right stick pressed in | the touchpad pressed |
| X and Y together (left hand) | blowing into the microphone, for as long as they are held |
| left grip | L1; held, the right stick flicked to a side turns the view a step |
| menu button (left hand) | OPTIONS |
| left stick pressed in | L3 |
| both sticks pressed in | resets the view |

**The right controller is the controller in the game**: where it is and where it points is
where the game's DualShock is, tracked fully (a gamepad only gives its turning, and its
place as far as hand tracking sees the hands around it). At the first screen, hold the right
controller into the outline; gadgets shoot where it points. `controller_hand=left` in the
settings makes it the left one (the right grip then turns the view). The game's rumble goes
to both controllers.

**With a gamepad on the PC as well, whichever was used last plays.** A button of the
headset's controllers that plays (A, B, X, Y, a stick pushed or pressed in; not a trigger, a
grip or the menu button by itself, which a hand also does to a controller it only rests on)
takes the game over once the gamepad has said nothing for a quarter of a second, and the
gamepad takes it back with its first button, stick or touch. The console window says each
change. Until 0.18 a gamepad that was merely connected, lying on a desk, kept the headset's
controllers from doing anything. One case stays as it was: a gamepad that a streaming
program makes of the headset's controllers themselves (Virtual Desktop's gamepad emulation)
says what they say as they say it, and is then the one that plays.

## The game's versions

The emulator knows two builds of the game's executable from inside: the one on the disc
(1.00) and the last update (1.04). It tells them apart by what the executable holds when it
is loaded, not by what a package's name or `param.sfo` says, and the console window names
the one it found (`CUSA12392 in a build known from inside: ...`). Any other build is left to
itself: it plays at the console's sizes, and in slow motion where frames take long.

- **A copy of the game that has the update in it** (one package, or one folder): put it in
  `games` as it is. Keep the whole folder together: an updated `eboot.bin` alone in an
  otherwise incomplete folder is not the game.
- **The game and its update as two packages**: the launcher unpacks the game's own package
  and leaves the update alone (an update holds only the files it changed, and is turned down
  if it is offered as the game). The game then plays as on its disc. To play it updated, the
  update gets a folder of its own next to the game's, named `CUSA12392-UPDATE`, which the
  emulator lays over the game's files as shadPS4 does; the game's own folder is not changed.
  With PkgTool, which is in the `pc-vr\pkgtool` folder, from the AstroQuest folder in
  PowerShell:

  ```powershell
  $tool = ".\pc-vr\pkgtool\PkgTool.exe"
  $update = "C:\Games\astrobot-update-1.04.pkg"
  & $tool pkg_extract --passcode ("0" * 32) $update ".\update-unpacked"
  Move-Item ".\update-unpacked\uroot" ".\games\CUSA12392-UPDATE"
  # The update's description of itself is kept apart in the package: its number is on the
  # PARAM_SFO line of the list (10 in the 1.04 update tried here).
  & $tool pkg_listentries $update
  New-Item -ItemType Directory -Force ".\games\CUSA12392-UPDATE\sce_sys" | Out-Null
  & $tool pkg_extractentry --passcode ("0" * 32) $update 10 ".\games\CUSA12392-UPDATE\sce_sys\param.sfo"
  ```

  Start the game as always: the launcher still names `games\CUSA12392\eboot.bin`, and the
  emulator runs the update's.
- **Saves** are in the same place for both versions (`pc-vr\user\home\1000\savedata\CUSA12392`).
  A save made by 1.00 loads in 1.04; whether one written by 1.04 loads in 1.00 has not been
  tried, so copy that folder before changing versions. To go back to 1.00, move
  `CUSA12392-UPDATE` out of `games`.

Tried here: both versions on the PC and, without wearing it, on the Quest 3 (title, the
controller's screen, world map, into a level; 1.04 at 2880x3072 an eye on the PC).
ODevStudio played 1.04 through its first level on a Valve Index with a fix of their own for
the same thing (pull request #15), whose write-up of the folders this section follows. 1.00
is the version played the most.

## Desktop Spectator View

The launcher offers **Stereo**, **Single eye** and **Combined eyes**. Single eye shows the
complete left-eye picture. Combined eyes keep that eye as the main picture and add the
other eye's non-overlapping peripheral strip. This is an experimental composite, not a
separate spectator camera: nearby objects can disagree at the join because the eyes are
in different places. Combined eyes preserve the source eye's proportions and widen the
desktop canvas by the extra peripheral coverage. With symmetric horizontal projections,
Combined eyes have the same aspect ratio as Single eye and add no peripheral view.
These desktop choices leave the headset's stereo picture unchanged.

By default, the desktop preserves the complete image with black bars as needed, without
stretching or cropping. **Crop top/bottom to fill** scales Single eye or Combined eyes to
the desktop width and crops the top and bottom equally on a wide monitor. It does not
stretch the image, add scene coverage or change the headset. Narrow windows can still
have bars above and below; the sides are never cropped. Stereo ignores this option.
Fullscreen follows shadPS4's native `GPU.full_screen` setting in
`pc-vr\user\config.json`; the launcher does not override it or save a separate preference.

Sony describes the original PSVR's standard TV social screen as an undistorted, cropped
right-eye image ([official FAQ](https://blog.playstation.com/archive/2016/10/03/playstation-vr-the-ultimate-faq/)).
Games could also supply a separate TV image; Astro Bot-specific use of that path has not
been confirmed here. Our default spectator option preserves the complete image; cropping
is optional.

## Settings

`pc-vr\settings.txt`, one `key=value` a line (the file explains each):

| Setting | Meaning |
| --- | --- |
| `resolution=2880` | the width of each eye's picture: `1440` (the console's largest), `1800`, `2160`, `2520`, `2880` (default), `3240`, `3600`; or `game` for the console's sizes chosen by the game. The game draws a step smaller by itself while the graphics card falls behind |
| `dynamic=0` | hold the game to the chosen size even where the graphics card falls behind |
| `fps=60` | the most frames a second: `120`, `90`, `72`, `60` (default), `45`, `40`, `36`, `30`; what the headset's refresh rate allows (see "Speed") |
| `fov=100` | how much of the headset's field of view the game draws, 70 to 100 percent: 100 fills all of it; less puts the same pixels over fewer degrees, sharper, with a dark border |
| `fov_of=psvr` | `fov` is a percent of a PlayStation VR's field of view (100 by 103 degrees an eye, what the game was made for) instead of the headset's own (`fov_of=headset`, the default) |
| `menu=0` | no window with the main settings at the start |
| `desktop_view=spectator` | one complete eye on the monitor; `combined` adds the other eye's peripheral strip; default `stereo` keeps both eyes side by side. Does not change the headset view |
| `desktop_crop=1` | scale Single eye or Combined eyes to the desktop width, cropping the top/bottom if needed; default `0` preserves the complete image. Stereo and headset output are unchanged |
| `sharpen=0.3` | sharpening of the picture on its way out, 0 to 1. Virtual Desktop has its own on top |
| `msaa=4` | the most samples a pixel gets; default as the console draws it. With `1` the emulator smooths edges itself (unless `antialias=0`) |
| `hands=0` | do not use hand tracking to place the controller |
| `predict_ms=20` | how far beyond the next picture the head position given to the game is predicted (0 to 80 ms) |
| `stick_touchpad=0` | the right stick no longer doubles as a finger on the touchpad |
| `mic_gain=3` | what the microphone hears, that many times louder for the game (0.1 to 30): for when blowing does too little |
| `controllers=0` | the headset's own controllers never stand in for a gamepad |
| `controller_hand=left` | which Touch controller is the controller in the game (default `right`) |
| `language=fr-FR` | the language the game is played in, as Windows names languages (`fr-FR`, `de-DE`, `pt-BR`, `ja-JP`...); default `windows`: the one Windows is shown in. The game has 28; it is in English for any other. The launcher's window has the list |
| `turn=45` | how many degrees the view turns for each flick of the right stick with L1 held (default 30; `0`: never) |
| `pause=0` | the game is never made to wait when the headset is off the head or shows something else |
| `surround=0` | fold the game's surround sound down instead of rendering it for the headset's speakers |
| `real_time=0` | let the game count time in frames, as on the console |
| `pace=1` | the older way of saying `fps`: refreshes of the headset a frame is given (takes the place of `fps` when set) |
| `wait=60` | seconds to wait for a headset before starting on the monitor (`0`: start at once, move over when it connects) |
| `headset=0` | do not look for a headset, play on the monitor |
| `game=...` | where the game is, if not in the `games` folder: its `eboot.bin`, its folder, or its `.pkg` (which is unpacked into `games`). The launcher's "Where is the game?" window writes this line itself |
| `env=NAME=value` | extra environment variable for the emulator |

The emulator's own settings (window size, input bindings, log) are in `pc-vr\user\config.json`
and `pc-vr\user\input_config\`. Saves are in `pc-vr\user\home\1000\savedata\CUSA12392` and are
the same format as on the headset: `tools/quest-save-to-pc.sh` copies the save of the
headset's app over (headset on the cable; nothing on the headset is changed, and a save the
PC already has is kept next to the new one).

**Until 2026-10-03 the PC build never read its save**: every start was a new adventure. The
game's thread that looks for system events started before the one that starts the user
service, the emulator handed it the player's login anyway (a console refuses until the service
is started), the game counted the player twice, took them for one who had come back, and kept
the empty save it had in memory instead of reading theirs. Fixed in the emulator; checked with
the headset's save (World 1, levels 1 to 3 done): the title offers CONTINUE and the world map
has the 14 rescued bots.

## What you get

| | PlayStation 4 | Quest 3 on its own (app 0.7) | PC through Virtual Desktop |
| --- | --- | --- | --- |
| Frames a second | 60 | 30 in the levels, 45 in light scenes | 60 (at 120 Hz), up to 120 by the setting |
| Scene size per eye | 960x1080 to 1440x1536 | 816x870 to 1200x1280 | up to 2880x3072 (default), 3600x3840 at most |
| Edges | multisampled | smoothed by a filter | multisampled |
| Head movement | 120 Hz, reprojected | 90 Hz, reprojected | the stream's rate, reprojected in the headset |
| Field of view per eye | PlayStation VR's, 100 by 103 degrees | PlayStation VR's (85% of it from app 0.9 on) | the headset's own, all of it (the simulated Quest 3: 94 by 99 degrees) |

The game is told the field of view of the headset being worn, as the emulator finds it when
the session starts, and draws exactly that: the picture reaches every edge of the view (with a
PlayStation VR's, as on the console, a Quest 3 would show a narrow dark margin far out to the
sides, while degrees beyond its view on the nose side, top and bottom were drawn for nothing).
What the PC cannot change: the picture reaches the headset as a video stream, with the
softness and the delay that brings (Virtual Desktop's own settings decide those: bitrate,
codec, sharpening).

Measured on this PC (Ryzen 7 9800X3D, RTX 5070 Ti) with a simulated Quest 3 at 120 Hz
standing in for the headset (see "Testing without the headset"): title, prologue, world map
and the first level, its heaviest view included (1800 draws and 8 million vertices a frame),
at 60.0 frames a second with every frame shown for exactly two refreshes, the GPU busy 5 to
15% of the time at 1440x1536. Level 1-4 at 2160x2304 an eye: 60 frames a second, the GPU busy
20%; at 2880x3072: 60, busy 29%.

**How the larger sizes come about.** The game has a list of sizes it draws its scene at
(816x870 to 1440x1536 an eye) and makes the pictures it hands to the headset 1440x1536; it
sets aside 200 MB for its render targets and 872 MB of graphics memory for everything. With
`resolution` above 1440 the emulator writes larger sizes into the game as it loads (every
size of the list grown by the same factor, the pictures for the headset too, the pools and
the memory they come from), and gives the emulated console that much more memory: the game
then draws everything at that size itself - a sharper picture, not an enlarged one.

## Speed

The emulated PlayStation VR refreshes when the real headset's picture is due (the runtime
tells: once for every picture it wants), and the game draws one frame for every two
refreshes, as it does on the console. With Virtual Desktop at 120 Hz that is the console's 60
frames a second; at 90 Hz it is 45 and at 72 Hz 36, each frame shown for exactly two
refreshes, and the game is given the time its frames really take, so it runs at its proper
speed whatever the rate (see "Speed" in `README-QUEST-VR.md`). Where the runtime only asks
for 60 pictures a second or fewer (a 60 Hz stream, or Virtual Desktop's Synchronous Spacewarp
making up every other picture itself), the game is given one refresh for a frame instead of
two, which keeps it at 60.

**More than 60 frames a second** (`fps` above 60): the game may draw a frame for every picture of
the headset. A frame takes this PC 9 to 11 ms whatever the scene (it is the emulator's
processor work that takes the time; the graphics card is busy 13% of it), so 72, 80 and
90 Hz are within reach and 120 is not: there the emulator tries once, finds that frames do
not fit, and stays with every other picture (it tries again every ten minutes; a try is a
few seconds of uneven frames). With the simulated headset at 90 Hz the walk through the
first level held 90 frames a second throughout. The same scripted walk, played at 60 and at
90 frames a second (`tools/pc-rate-compare.sh`), does the same things at the same moments:
the game's own time step follows the frame rate. What does not are the few things the game
counts in frames: an animated sign in the level runs half as fast again at 90. That, and
that nothing but the first level was compared, is why it is a setting and not the way
things are: 120 Hz with the game's own 60 frames is what the game was made for.

**Hitches**: in play the longest wait for a frame is 20 to 25 ms (a frame is 16.7); where
scenes change (the title, the map, a level loading) single waits of 50 to 235 ms happen, six
in the five and a half minutes from the start into the first level. The head's movement
does not wait for them: the picture that is there keeps being turned by the compositor.

## How it fits together

```
PC
└─ shadps4.exe (the emulator, one process)
   ├─ the game's x86-64 code, run directly; PSVR, tracker, camera, sound emulated as on the Quest
   ├─ Vulkan on the PC's GPU; every frame the two eyes' pictures are put side by side
   │   ├─ at full size into an image of the OpenXR host            (the headset's picture)
   │   └─ scaled into the window                                   (what the monitor shows)
   └─ OpenXR host (thread "shadPS4:XrHost", src/core/vr/openxr_host.cpp)
       ├─ session with the PC's OpenXR runtime on the emulator's own Vulkan device
       ├─ every picture the runtime asks for: head pose, hands and controllers in, the newest
       │   frame out as a projection layer with the pose it was drawn for and the PSVR's
       │   field of view
       └─ the runtime's compositor turns it to where the head is by the time it is shown
            └─ Virtual Desktop Streamer ── video ──► Quest 3 (Virtual Desktop app)
```

The OpenXR host is to the PC what the Quest app is to the headset, without the process
boundary: no sockets and no shared buffers, the frame is one GPU copy away. The rest of the
emulator does not know which of the two it talks to.

Things that had to be right, for whoever works on this again:

- **The Vulkan device has to suit the runtime.** The runtime names the instance and device
  extensions it needs and the graphics card the headset hangs off; the emulator adds them
  when it makes its device (`vk_platform.cpp`, `vk_instance.cpp`). A headset that connects
  after that still works: the extensions the common Windows runtimes need are enabled
  whenever a runtime is installed.
- **The queue is shared.** Runtimes submit to the emulator's Vulkan queue when frames begin
  and end, when images are acquired and released, and (Virtual Desktop's) when a swapchain's
  images are first listed. All of those calls are made under the lock the emulator's own
  submissions take (`Scheduler::submit_mutex`).
- **sRGB.** A runtime takes an 8-bit image that is not of an sRGB format for linear light and
  shows it too bright. The headset's picture is drawn into an sRGB image (the eye pass
  converts back to linear light at its end, `linear_out` in `post_process.frag`) and copied
  to the runtime's sRGB image unchanged.
- **Between two frames of the game** the last picture is submitted again with its pose; the
  runtime keeps using the image released last.
- **The headset coming off** shows as the session leaving the "focused" state. The emulator
  then tells the game what the console tells it (`sceSystemServiceGetStatus`: the system's
  own screen is over the game), which is what makes this game pause. (The headset's own
  "worn" flag, `hmuMount`, does nothing to it.) While it waits, the game hands over a black
  picture of one pixel an eye; the host shows nothing then instead of asking the runtime for
  images of that size.
- **A runtime that asks for no pictures** shows something else in the headset: the game is
  told to wait, as when the headset is off.
- **A headset that goes away** is not announced by Virtual Desktop's runtime (its source: a
  lost headset only makes it stop asking for pictures, for as long as the session lives;
  but it also asks for none while it shows the desktop, which passes by itself, and a new
  session pulls the headset back into VR). So the host makes a new session when the headset
  was put on and no picture has been asked for since, nor is for 3 seconds; when the
  headset is off and nothing was asked for for 30 seconds (either less and less often, up
  to every 5 minutes, while it changes nothing); or when the session's calls only fail for
  2 seconds. A runtime that does announce a loss is believed at once. The game is paused
  from the loss until a new session is asked for pictures with the headset on the head.
- **The headset's controllers** are read through one OpenXR action set bound to the Touch
  profile (which is also what Virtual Desktop's runtime offers for every controller), made
  with each session, and handed on as the first player's gamepad the way a scripted or
  remote gamepad is (`GameController::ApplyRemoteState`); the aim pose of one of them is the
  tracked controller (`Runtime::UpdatePad`).
- **Sound** goes to Virtual Desktop's own sound device ("... (Virtual Desktop Audio)")
  whenever that is in the system, which it is while Virtual Desktop streams to a headset,
  whichever device Windows prefers; else to the device the runtime names for the headset;
  else to the one Windows plays on. The game's 7.1 mix is rendered for two speakers at the
  ears as on the Quest. The microphone goes the same way.
  The sound follows its device: when that goes away the sound moves to Windows' default
  device, and back when it is there again (see "Fixed in 0.13"). Which device is the
  headset's is asked of the runtime again whenever a sound device comes or goes: Virtual
  Desktop has its own only while it streams to the headset, and names the PC's until then.
  Every port plays on a device of its own, never on "the default device" as such (see
  "Fixed and added in 0.19").

## Fixed in 0.20 (2026-10-06): two things in world 2

- **An invisible wall at the end of level 2-1** (issue #16): the hero stood in the air above
  the last mound and could not get to the last enemy; the island below had the same. The
  collisions of those two islands were most of a block above what was drawn. The game moves a
  collision by a speed worked out with one frame's time step and applied for the next
  frame's, which is the same thing only where every frame's step is the same, as on the
  console. Its physics now take each step with the time step the collisions were moved for
  (`README-QUEST-VR.md`, "Speed"). Played to the end of 2-1 on the PC.
- **Squares in the lava's glow around the octopus** at the end of world 2, lit and dark,
  for as long as he moved. The game has one buffer bound as two colour targets of the draws
  that glow, which Vulkan leaves undefined; such a draw is now made once for each of the two
  (`README-QUEST-VR.md`, "The picture"). Played on the PC in the headset.

## Fixed and added in 0.19 (2026-10-06), from what players wrote elsewhere

Reports from the project's thread on Reddit and from the comments under the videos about it.

- **The launcher asked for the Visual C++ runtime again and again**, however often it was
  installed; one player had to cut the check out of the launcher to play. Started from a
  32-bit program (a file manager, a game launcher), `Play Astro Bot VR.bat` got a 32-bit
  PowerShell, to which Windows shows the 32-bit system folder under the name of the 64-bit
  one: and one of the runtime's files, `vcruntime140_1.dll`, exists in 64 bits only. The
  launcher now hands over to the 64-bit PowerShell first (which also puts the OpenXR
  runtime's registry entry where it looks), names the files it misses, and its question has
  a third answer: start the game all the same.
- **The game was in English whatever the console's language should have been.** The
  emulator's console was set to English and nothing offered another. The launcher now says
  which language Windows is shown in, and its window has the game's 28 to choose from
  (`language=`); the game takes it as it takes a PlayStation's.
- **The headset's controllers did nothing while a gamepad was connected to the PC**, even
  one that lay unused. Whichever was used last plays now: see "With the headset's own
  controllers".
- **Turning the view by steps**, asked for by a player who sits where they cannot turn
  round: L1 (the left grip) held, the right stick flicked to a side. See "In the game".
- **The game stopped with "Unhandled Exception code 0xc0000005" some time after the headset
  was connected, and until then its sound came from the PC's speakers**, when the game was
  started before Virtual Desktop streamed to the headset. Two things, one leading to the
  other:
  - Virtual Desktop's sound device only exists while it streams. Asked for the headset's
    sound device before that, its runtime names what Windows plays on, the PC's speakers,
    and the emulator asked once, at the start. It then held on to the speakers by name, and
    took the sound back to them a few seconds after Windows had moved it to the headset. The
    runtime is asked again now whenever a sound device comes or goes, and the sound goes
    where it then says. And since what that runtime names is the device Windows prefers,
    which need not be Virtual Desktop's even while that one is there (Windows keeps to the
    device that was chosen last by hand): Virtual Desktop's own device is taken for the
    headset's whenever it is in the system, whatever Windows prefers and whatever the
    runtime in use (SteamVR names none).
  - That taking back is what stopped the game. Ports opened on "the default device" are
    moved by SDL, the sound library, when Windows changes its default; SDL 3.5.0 puts each
    moved port at the front of the new device's list without telling the port behind it
    (`SDL_DefaultAudioDeviceChanged` leaves that one's back link as it was), and closing
    one of them afterwards leaves the list pointing at freed memory, which the device's
    playback thread reads next (`SDL_GetAudioStreamDataAdjustGain`). The emulator's ports
    no longer play on "the default device": each is opened on the device Windows plays on
    at that moment, as the device it is, and moved by the emulator itself, three seconds
    after Windows chose another.

  Tried by making another device Windows' default while the game plays and the first one
  again after (`tools/pc-audio-default-test.sh`): the build before stops, this one plays on,
  on the right device each time; by taking the default device out of Windows and putting
  it back (`tools/pc-audio-device-test.sh`, levels measured on both devices); and against a
  simulated headset whose runtime names another device after one went away. Virtual
  Desktop's own device coming and going needs the headset: not tried yet.
- **A question in a box behind the game's window at the very first start** ("Save
  Migration": whether to move saves over from where an older shadPS4 kept them), which held
  the game up until somebody saw and answered it. The packages avoided it by bringing the
  folder it is about; started any other way, it came. It is only asked now where there is
  something to move.

## Linux, native build

The emulator builds and runs natively on Linux, with the same OpenXR host as on Windows.
There is no release package and no launcher for it: build it and set the launcher's
environment variables yourself. Tried on x86-64 with a Valve Index through SteamVR on an
NVIDIA card.

1. Build it, with clang, CMake and Ninja, from a clone made with its submodules (see
   "Building from source" in README.md); SDL needs the usual X11, Wayland, ALSA and
   PulseAudio development packages:

   ```sh
   cd shadps4-arm64-main
   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
   cmake --build build
   ```

2. **Turn on SteamVR's asynchronous reprojection.** SteamVR on Linux leaves it off, and
   without it the frames the game draws for every other refresh are shown again as drawn,
   not turned to where the head is: the picture judders whenever the head moves (the
   window on the desktop looks right all the same). With SteamVR closed, add
   `"enableLinuxVulkanAsync" : true` to the `"steamvr"` section of
   `~/.local/share/Steam/config/steamvr.vrsettings` (under `~/.var/app/com.valvesoftware.Steam/`
   for Steam from Flatpak). Its log (`logs/vrcompositor.txt` next to `config`) then says
   `Enabling async support!`. Set the refresh rate as for the Index above: 120 Hz for the
   game's 60 frames a second.
3. Make SteamVR the OpenXR runtime (SteamVR's Settings > OpenXR), and the headset's
   speakers and microphone the system's default devices: the emulator plays to the default
   output.
4. Start SteamVR, then the emulator. It keeps its settings and saves in a `user` folder in
   the folder it is started from if there is one, otherwise in `$XDG_DATA_HOME/shadPS4`
   (`~/.local/share/shadPS4`):

   ```sh
   SHADPS4_OPENXR=1 SHADPS4_XR_WAIT=60 SHADPS4_TITLE_EYE_WIDTH=2880 SHADPS4_VR_FPS_CAP=60 \
   SHADPS4_VR_FOV=100 SHADPS4_VR_FOV_OF=headset SHADPS4_VR_SHARPEN=0.3 \
   /path/to/shadps4-arm64-main/build/shadps4 -g /path/to/games/CUSA12392/eboot.bin
   ```

   The settings of "Settings" above are these variables; `pc-vr/launch.ps1` shows which
   variable each one sets.

## Linux, through Proton, as reported

The native build above is the more direct way; none of this has been tried here. A player
(klejmanm, on Reddit) reports the Windows build running on Arch and Mint with Radeon RX
6000/7000 cards, shown in a Quest through WiVRn, when it is started by Steam and not by a
script of one's own (Wine's OpenXR layer got in the way otherwise):

1. Steam > Games > Add a Non-Steam Game: `pc-vr/shadps4.exe`, "Start In" the `pc-vr`
   folder. In its Properties > Compatibility, force GE-Proton or Proton Experimental.
2. Properties > General > Launch Options, on one line, with your own paths. The launcher
   is not used this way, so what it would set is given here:

   ```
   PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 PRESSURE_VESSEL_FILESYSTEMS_RW=/var/lib/flatpak/app/io.github.wivrn.wivrn SHADPS4_OPENXR=1 SHADPS4_XR_WAIT=60 SHADPS4_TITLE_EYE_WIDTH=2880 SHADPS4_VR_FPS_CAP=60 SHADPS4_VR_FOV=100 SHADPS4_VR_FOV_OF=headset SHADPS4_VR_SHARPEN=0.3 SHADPS4_CONSOLE_LANGUAGE=en-US WINE_DISABLE_DNS=1 %command% -g "/path/to/games/CUSA12392/eboot.bin"
   ```

Before 0.19 the first start also needed the "Save Migration" box answered on the desktop,
or the headset's session timed out; that box no longer comes.

## Fixed and added on 2026-10-06, from the reports on GitHub

- **The game's version 1.04 plays** (issues #1, #3, #5, #7, #10). An updated copy of the game
  stopped at "Adjust your position until you fit roughly inside the silhouette. The controller
  with the RED light bar should also be in view", over a green picture, with no controller to
  be seen. From its first update on, the game asks the tracker to find the controller anew on
  that screen and waits to see the controller's status go from calibrating back to tracking;
  the emulator's tracker never said either. It now does what a console's does. The updated
  executable is also laid out differently, so the emulator did not know it and gave it
  neither the larger pictures nor the game's own speed ("resolution stuck at the lowest"):
  both builds are known now, 1.00 and 1.04, told apart by what the executable holds when it
  is loaded (the console window says "CUSA12392 in a build known from inside: ..."). The
  addresses for 1.04 are Clodo76's. Any other version still plays as before: at the
  console's sizes, and in slow motion where frames take long.
- **A package that is only the game's update** was unpacked as if it were the game. The
  launcher now takes the game's own package first and says what an update alone is. To play
  the game with its update, use a copy that has the update in it, or put the update's files
  in a folder named `CUSA12392-UPDATE` next to the game's `CUSA12392`: the emulator takes
  what is there over the game's own files (shadPS4's way of keeping updates), and the
  game's folder stays as it was. On the headset the same goes for
  `/data/local/tmp/astro/games/CUSA12392-UPDATE`. Without that folder the game plays as on
  its disc, 1.00, which is the version tried the most here.
- **The game stopped as it started with code -1073741819** (#9) where Windows listed a
  gamepad that could not be opened: nobody was logged in then, and the game takes its first
  player for granted.
- **With SteamVR, the game stopped as it started** with "Mapping cannot fit inside free
  region", code -2147483645 (#14): SteamVR's parts came to lie where the game maps its
  memory. The console's memory is now set aside before the headset is looked for (found by
  evertec82). And a session that is asked for no pictures is only made anew for Virtual
  Desktop's runtime, which needs it; SteamVR's are left alone.
- **The catapult at the end of a level, and the rest of the touchpad, without a touchpad**
  (#2): see "In the game". The catapult took luck with a stick, for two reasons. The game
  shoots by where the finger is when it lifts, and a stick let go took the finger back to the
  middle first. And the game looks at the pad once for every frame it draws: a stick is at
  its end within a few hundredths of a second, so the pull began, for the game, where it
  ended, the more often the fewer frames it drew (by a model of the game's own rule, one
  fast pull in four failed at 60 frames a second and two in three at 30). The stick's finger
  now stays where it comes down until the game has seen it there and moves no faster than a
  finger does; and three buttons do the three things the game asks of the pad outright:
  press it, swipe forward, pull back and let go.
- **The right stick no longer holds a finger on a real touchpad** (#12): a stick that drifts
  touched the pad for good, and a real finger's swipes then counted for nothing. If swipes
  on a DualSense's touchpad still do nothing, the console window tells what Windows hands
  over ("Controller 1 connected: ... touchpad yes") and whether a finger was ever felt ("The
  controller's touchpad feels a finger"): a gamepad that reaches the emulator through
  something else than itself (Steam Input, DS4Windows, a headset's streaming app) may come
  without its touchpad.
- **The gamepad in the game can be moved** where nothing tracks it (#4), and **blowing** can
  be seen and replaced (#8): see above.
- SteamVR and Valve Index support and the desktop's spectator views are ODevStudio's (pull
  requests #6 and #11).

## Fixed in 0.13 (2026-10-03)

- **No sound until the emulator was started again.** Virtual Desktop takes its playback
  device ("Virtual Desktop Audio") out of Windows whenever the headset is not being streamed
  to, and puts it back afterwards. A stream opened on a device that goes away plays into
  nothing from then on, without an error, and the device that comes back is a new one to the
  sound library. The emulator now looks once a second for the device its sound is meant for:
  three seconds after that went away the sound moves to Windows' default device, and three
  seconds after it is back, to it again. Where the device is Windows' default itself, the
  sound is opened on "the default", which the sound library moves by itself. (The waiting is
  there because opening a device while Windows changes its default can hang the sound for
  good.) The log says which devices come and go ("Audio output added", "removed", "went
  away", "is there"). Tested by taking a playback device out of Windows and putting it back
  while the game plays, and measuring what each device plays: `tools/pc-audio-device-test.sh`.
  (`tools/pc-audio-default-test.sh` changes which device Windows plays on instead, as a
  program that streams to a headset does when the headset connects.)
  Not changed: the microphone.
- An emulator crash now leaves its call stack in the log (module and place in it).

## Fixed on 2026-10-03 (afternoon)

- **The game stopped at start with the DualSense paired to the PC** ("Unhandled Exception
  code 0xc0000005 at 0x800c47a21"): two gamepads were there, the DualSense and the Xbox 360
  controller Virtual Desktop makes of the headset's controllers. Each logged in a player of
  its own, the game registered both with the PlayStation VR tracker, and the emulator's
  tracker refused the second; the game takes that as fatal. Now, with a headset, there is one
  player: the best gamepad (a DualSense before anything else) is theirs and the others are
  left alone (the console window says "... is not used"); `env=SHADPS4_VR_ONE_PLAYER=0` in the
  settings logs in a player for every gamepad again. The emulated tracker also takes up to
  four controllers now, as the console's does.
- **Everything in shade too dark** (most visible in level 1-4, where the ground was black):
  objects lit by any light probe but a level's first were lit from the wrong cube faces. See
  `README-QUEST-VR.md`, "App 0.10".
- **Field of view: 100% is now all that the headset being worn shows**, not a PlayStation
  VR's view. The emulator reads the headset's view from the OpenXR runtime when the session
  starts (the console window says "The headset shows ... degrees an eye") and tells the game
  that, scaled by the `fov` percent. The game asks once, as it starts; should the headset not
  have said by then, the emulator waits up to 10 seconds for it, and after that takes what the
  headset showed the last time (kept in `pc-vr\user\vr_headset_fov.json`). `fov_of=psvr` in the settings goes back to the
  PlayStation VR's view.

## When something does not work

The console window shows the emulator's lines about the headset as they come; the whole log
is `pc-vr\user\log\shad_log.txt` (the start before it: `shad_log.prev.txt`). What to look for:

| Line | Meaning |
| --- | --- |
| `Headset found through VirtualDesktopXR ...` | the runtime has a headset: Virtual Desktop is connected |
| `Waiting up to 60 s for a headset ...` | the runtime has none yet: connect Virtual Desktop |
| `... has no headset connected yet` / `No headset yet` | still none: the game is on the monitor and moves over when there is |
| `Headset session: focused` | pictures are shown and the headset is on the head |
| `The headset is off the head, or shows something else: the game waits` | the game is told to pause, until `The headset is on the head and shows the game` |
| `Frames are shown through 3 images of 2880x1536` | the picture's size and format in the runtime |
| `Headset: the title delivered 60.0 frames a second (20 ms the longest wait for one, 0 waits of more than 50 ms), 60.0 were shown, the headset took 120.0 pictures a second` | every ten seconds: the game's frame rate against the stream's, and how badly it hitched |
| `pictures were shown N degrees from where they were drawn for` | how much the compositor had to turn pictures: a few degrees while the head turns, next to nothing at rest |
| `Hands: both seen N% of the time ... holding the controller N%` | what hand tracking gave |
| `Controller 1 connected: ... (motion sensors yes, touchpad yes, light yes)` | what Windows handed over of the gamepad |
| `Controller ... cannot be opened and is not used: ...` | Windows lists a gamepad that something else holds, or that is just going away |
| `The controller's touchpad feels a finger` | the first touch of the gamepad's own touchpad arrived |
| `CUSA12392 in a build known from inside: 1.00, as on the disc` (or `1.04, the last update`) | the game's executable is one the emulator has its speed and picture fixes for |
| `This build of CUSA12392 is none of those known from inside ...` | another version of the game: it plays at the console's sizes, and in slow motion where frames take long |
| `Microphone: the loudest of the last 10 seconds was -18 dB ...` | what the game heard; it takes -21 dB for blowing at half strength, -9 dB for all of it |
| `Microphone: nothing but silence has come from it so far ...` | no sound at all reaches the emulator: see the line for what to look at |
| `The controller, while nothing sees where it is, is held to be at ...` | the gamepad's place was moved or switched (PS + D-pad, PS + triangle) |
| `No gamepad is connected to the PC: the headset's controllers stand in for it` | the Touch controllers are the gamepad |
| `Controllers: standing in for the gamepad; the right one tracked N% of the time ...` | every ten seconds: where the tracked controller is and what is pressed |
| `The headset is gone: the game waits until it is back` / `... starting over with it` | the session was lost; a new one is made when the headset answers again |
| `The headset is driven by another graphics card ...` | the PC has two; set `Vulkan/gpu_id` in `config.json` or start with the headset connected |
| `Windows has ... of memory left to hand out, and the emulator asks for about 14 GB` | (the launcher, before the game starts) the emulator has Windows set the console's whole memory aside in one request (`address_space.cpp`), 14 GB in all at 2880x3072. Windows can usually still find it by enlarging its page file, but may refuse while it does, and the emulator then stops as it starts (`The emulator ended with code -2147483645`). Close other programs, or just start again |

No picture in the headset although the session is focused: look for `failed:` lines (frame
calls the runtime refused are counted and logged).

## Testing without the headset

The launcher checks run with `powershell -NoProfile -ExecutionPolicy Bypass -File tools/tests/launcher-test.ps1`.
To check the OpenXR quaternion conversion, parallel and opposite-canted stereo bounds, and
headset FOV cache validation, run these commands from the repository root in a developer shell
with `clang-cl` and the Windows SDK available (the `build` directory must exist):

```powershell
clang-cl /std:c++latest /EHsc /Ishadps4-arm64-main/src /Ishadps4-arm64-main/externals/openxr-sdk/include tools/tests/openxr_view_test.cpp /Fobuild/openxr_view_test.obj /Febuild/openxr_view_test.exe
./build/openxr_view_test.exe
clang-cl /std:c++latest /EHsc /Ishadps4-arm64-main/src /Ishadps4-arm64-main/externals/json/include tools/tests/headset_fov_cache_test.cpp /Fobuild/headset_fov_cache_test.obj /Febuild/headset_fov_cache_test.exe
./build/headset_fov_cache_test.exe
```

To check desktop composition, in the same shell:

```powershell
clang-cl /std:c++latest /EHsc /Ishadps4-arm64-main/src tools/tests/spectator_view_test.cpp /Fobuild/spectator_view_test.obj /Febuild/spectator_view_test.exe
./build/spectator_view_test.exe
```

Virtual Desktop's runtime has no headset to offer unless one is connected, so the OpenXR path
is tested against the **Meta XR Simulator** (installed on this PC), which is a full OpenXR
runtime with a simulated Quest 3 and its controllers. `XR_RUNTIME_JSON` makes one process use
it; the system's runtime stays Virtual Desktop's.

| Tool | What it does |
| --- | --- |
| `tools/pc-xrsim-test.sh <name> <seconds> [NAME=value ...]` | runs the PC build against the simulator, log in `build/dev/xrsim/<name>/`. `XRSIM_HZ=120` sets the simulated display's rate; `SHADPS4_INPUT_SCRIPT=` as for `run-win.sh` |
| `tools/window-shot.ps1 [<title> <png>]` | lists windows, or saves a picture of one (the simulator's shows what its headset shows; the game's shows both eyes) |
| `tools/xrsim-keys.ps1 <key>:<ms> ...` | works the simulator's window: `B` is A and X, `N` is B and Y, `Y G H J` the sticks, `I` the sticks pressed in, `Comma` the menu button (hold keys for a second: short presses are not always seen); `Look:<dx>,<dy>` turns the simulated head, `Click:<x>,<y>` clicks |
| `tools/pc-rate-compare.sh <name> "<NAME=value ...>"` | the same scripted level walk at 60 frames a second and with the given settings, a picture every two seconds from each |
| `tools/xr-probe-win` (`build/xr-probe-win/xr_probe_win.exe`) | what a runtime offers: extensions, system, Vulkan requirements |
| `tools/tests/launcher-test.ps1` | tries the launcher's search for the game (unpacked games and packages, names with brackets, leftovers of an unpacking) on made-up folders |
| `tools/ui-drive.ps1 -Steps "text\|picture.png\|button\|seconds", ...` | works the launcher's own windows and message boxes from outside: waits for one that shows a text, saves a picture of it, presses a button. With a release unzipped somewhere and a package put in its `games` folder, that is the whole first start, from the question about unpacking to Play |

Settings for tests: `SHADPS4_XR_HEAD=0` (the head is a script's to move, the host only
shows), `SHADPS4_XR_FREEZE_AFTER=<s>` (no new pictures after that), `SHADPS4_XR_WAIT=<s>`,
`SHADPS4_XR_HIDE_FOR=<s>` (the headset only turns up that long after the start),
`SHADPS4_XR_LOSE_AFTER=<s>` (the session is taken for lost once),
`SHADPS4_XR_UNWANTED=<from>,<to>` (no pictures are asked for between those seconds of the
session), `SHADPS4_VR_FASTEST_PACE=1` (what `pace=1` sets), `SHADPS4_XR_CONTROLLERS=`
`0`/`1` (off by default under an input script), `SHADPS4_XR_PAD_OFFSET=<x>,<y>,<z>` (where
the gamepad is taken to be from the controller, in metres right, up and towards the player),
`SHADPS4_OPENXR=0`; in input scripts `worn=0` / `worn=1` take the headset off and put it on.

What the simulator runs showed: the session comes up and is focused; pictures arrive at
2880x1536 in an sRGB image and look right in the simulated headset (upright, colours and
brightness as on the monitor, the narrow margin at the outer edge where the PSVR's field of
view ends; since the game draws the headset's own field of view, the simulator's 54/40/50/49
degrees an eye, the layer it is handed has exactly that one); at 72 Hz the game locks to 36.0 frames a second and at 120 Hz to 60.0, following
a change of rate while running; pictures are shown where the head is at rest (0.0 degrees
off), also after the head was turned and after the seat was reset there; the first press of
✕ resets the seat; closing the window ends the session cleanly; the game's window minimised
changes nothing in the headset. A headset that only turns up 50 seconds into the game is
picked up and shown to; a session lost in the middle of the game pauses it, and the new
session shows it again at 60.0 frames a second; a runtime that asks for no pictures for a
while (as Virtual Desktop's does while it shows the desktop) pauses the game for as long,
without a new session. The simulated Touch
controllers start the game (A), move the stick and the touchpad finger, reset the view (both
sticks), and the right one, held into the outline of the first screen, passes it: the game
draws its DualShock where that controller is.

What only a session with the headset on can show: Virtual Desktop's own runtime accepting all
of it (its source says it will: it passes pose and field of view of the layer on, offers the
image format that is asked for, takes the extensions it is given and maps every controller
to the Touch profile), the stream itself, hand tracking through Virtual Desktop, which sound
devices it names, and what it does when the headset is taken off or disconnected (the host
is written to its source, not to its behaviour).

## Building

```sh
source tools/env-win.sh && cmake --build build/win-x64 --target shadps4   # the emulator
bash tools/make-pc-vr.sh                                                   # copy it to pc-vr/
```

The OpenXR loader (Khronos OpenXR-SDK 1.1.63, `externals/openxr-sdk`) is built with the
emulator; `ENABLE_OPENXR` (on for Windows and Linux) switches the whole of it.
