# Astro Bot Rescue Mission on Meta Quest 3 — what is in this folder

This folder holds a work-in-progress port of the `shadps4-arm64` PS4 emulator core to immersive VR
on Meta Quest, built around one game: ASTRO BOT Rescue Mission (CUSA12392).

| Folder / file | What it is |
| --- | --- |
| `shadps4-arm64-main/` | The emulator core (fork of shadPS4 with a FEX x86-64 CPU backend), with the PSVR emulation and the sound engine added here |
| `quest-host/` | New Quest app: OpenXR presenter + launcher for the core (`com.astrobotquest.vrhost`) |
| `tools/` | Portable toolchain (clang, CMake, Ninja, Windows SDK, Debian arm64 sysroot) and helper scripts |
| `games/CUSA12392/` | The game, extracted from the PKG |
| `build/win-x64/` | Windows x64 build of the core, used to develop and test on this PC |
| `build/arm64/` | The core cross-compiled for the headset (aarch64 Linux/glibc) |
| `build/quest/astro-vr-host.apk` | The app to install on the headset |
| `pc-vr/`, `Play Astro Bot VR.bat` | The other way to play: the emulator runs on this PC and the headset shows it through Virtual Desktop. Everything about it is in `README-PC-VR.md` |

**App 0.10 (2026-10-03 afternoon): the light.** Everything lit by the game's light probes
other than the first was lit from the wrong faces: a level keeps its probes in one cube-map
array, and AMD hardware takes the layer of a cube array as face + 8 x cube while the
emulator's array has six faces to a cube; the emulator passed the number on unchanged. In
level 1-4 the ground, lit by the third probe, read layers past the last and came out black;
everything else lit by a later probe (rock in shade, the controller, Astro) came out darker
or in the wrong colour - in every level, not only 1-4. Fixed in the shader translator
(`FixCubeCoords`); the canyon of 1-4 now shows its red earth and green plants.

**App 0.11 (2026-10-03): the first release on GitHub.** On the headset it does what 0.10
does; it is built from the published source, which by then also had the PC's field-of-view
changes (`README-PC-VR.md`), which the headset app does not use. From 0.11 on the app is
signed with the project's own key: over a build signed with the Android debug key (0.10 and
before) it only installs after that one is uninstalled, which deletes its saves - copy the
`savedata` folder off the headset first.

**App 0.12 (2026-10-03)**: the same app again. What changed is the PC's launcher, which finds
the game in its `games` folder, unpacks a `.pkg` by itself and asks where the game is when it
finds none (`README-PC-VR.md`). The folder it unpacks is also what the headset needs.

**App 0.13 (2026-10-03)**: the same app once more. What changed is on the PC: the sound no
longer stays silent after Virtual Desktop took its sound device away and brought it back
(`README-PC-VR.md`).

**App 0.14 (2026-10-04): startup field-of-view menu.** Each normal launch now shows a popup in the
headset before starting the game. Choose 50–120% with Left / Right on the gamepad's D-pad or
left stick, then press X / A to play. The last choice is remembered; `fov=` in `vrhost.txt`,
when present, supplies the initial choice instead. The popup explains how a narrower render
projection improves pixels per degree at the same resolution and shows the approximate
horizontal angle and average density gain. Below 100%, each eye gets a feathered squircle
(pillowed rectangle) border in the host's existing frame-copy pass. The compositor still
handles lens distortion. At 100% and above there is no added mask. Headless dry-start tests
use the configured FOV without waiting for input.

**App 0.15 (2026-10-04): FOV is relative to the user's headset.** The slider now runs from
50–100%, with 100% filling the headset's own field of view. The host reads its projection
from OpenXR before allowing Play, and passes those full-size optics to the emulator before
the game starts. The angle and PPD estimate use that detected FOV. Reduced settings retain
the soft squircle border; the initial default is still 85%.

**App 0.16 (2026-10-04): align the reduced-FOV apertures with the eyes.** The squircle now
uses the submitted frame's projection to locate straight ahead in each eye texture, instead
of assuming both optical axes sit at the texture midpoint. The detected Quest projection is
asymmetric, so the old masks pointed outward and curved inward over different parts of the
binocular scene. The corrected masks follow the optical axes while preserving the game's
stereo projections and the headset-relative slider. Invalid eye-separation measurements are
also ignored. Validation includes asymmetric shader checks at 50%, 85% and 95%, and a
captured 50% stereo scene whose distant features match the reported projection to within
one pixel.

**App 0.17 (2026-10-04): restore matching binocular coverage at reduced FOV.** Centering the
masks in 0.16 did not fix the unequal inward and outward render bounds introduced in 0.15.
The Quest host now opts into a symmetric render envelope enclosing the detected headset
projection, then scales that envelope for the chosen FOV. Both eyes have the same angular
aperture and squircle boundary. At 100% the envelope covers both full native frusta; OpenXR
clips the excess to the actual headset view. The popup reports visible coverage after that
clipping and the pixel-density gain at the center. Raw headset detection remains unchanged.
Regression checks compare the same angular directions in both eyes, including directions
that were visible in one eye and black in the other at 50% in 0.15 and 0.16.

**App 0.18 (2026-10-06): what the reports on GitHub brought.** The first release since 0.13:
it has the field-of-view menu and the fixes of 0.14 to 0.17 above, and

- **the game's version 1.04 plays.** An updated copy of the game stopped at "Adjust your
  position until you fit roughly inside the silhouette", over a green picture: from its
  first update on, the game asks the tracker to find the controller anew there and waits to
  see the controller's status go from calibrating back to tracking, which the emulator's
  tracker never said. It does now. The updated executable is laid out differently as well,
  and is known now next to the disc's: the game's own speed and the choice of its picture's
  size apply to both (core.log says `CUSA12392 in a build known from inside: ...`). The game
  with its update can be one folder, or the update in a folder of its own beside the game's:
  `/data/local/tmp/astro/games/CUSA12392-UPDATE` (see "The game's versions" in
  `README-PC-VR.md`). Tried on the headset without wearing it, into a level.
- A microphone the app was once refused: the app asks again at its next start; if the
  headset no longer shows the question, allow it in the headset's Settings, under the app
  permissions for the microphone, for "Astro VR Host". core.log says what the game hears
  ("Microphone: the loudest of the last 10 seconds was ...").
- **more controllers than the DualSense.** The app was made for a DualSense and took
  everything else for a lesser one. Now (all of it under "Controllers" below):
  - **the headset's own controllers play the game**, for whoever has no gamepad. The app
    reads them through the headset's runtime, with where they are and how they point; the
    right one is the controller in the game.
  - with a gamepad and the headset's controllers both at hand, **the game is played with the
    one used last**.
  - **the start-up menu takes whatever is in the player's hands**: a stick to a side chooses,
    ✕ / A or a trigger starts the game.
  - **a gamepad without motion sensors is turned and tilted in the game by the hands that
    hold it.** Before, it followed the hands to where they were and kept pointing straight
    ahead, which left the game's gadgets nothing to aim with.
  - **what the game wants done on the touchpad is on buttons**, on the headset's
    controllers and on every gamepad that has no touchpad: the right trigger (R2) presses
    it, the right grip (R1) swipes forward, the left trigger (L2) pulls back and lets go,
    which is the catapult at the end of every level. A card in the headset says so when the
    game starts and whenever OPTIONS is pressed. (The first worn try of this version ended
    at that catapult: the stick alone did not shoot it. The game looks at the pad once for
    every frame it draws, thirty times a second here, and a stick is at its end before it
    has looked twice: the pull began, for the game, where it ended.)
  - **the right stick as the touchpad's finger** is one the game can follow now: it stays
    where it comes down until the game has seen it there, moves no faster than a finger
    does, and lifts where the stick was let go. A button that presses the touchpad touches
    it as well.
  - smaller things for gamepads that are not PlayStation's: a right stick that reports on
    other axes, triggers that are only buttons, a Back button the system would take for its
    own "back" and close the app with.

  Played in the headset: the headset's own controllers, from the start-up menu into a level
  (seen all of the time, the game played with them). The buttons for the touchpad came after
  that session, which ended at the catapult; they have been played with on the PC, where
  they are the same code, and the catapult goes off there. On the headset they were checked
  without wearing it: what the game is given of a pull, a swipe and a press, read by read
  (see "Testing on the headset without wearing it"). **Not tried: a gamepad other than a
  DualSense**, and with it hands that turn a gamepad without motion sensors.

What else is new is for the PC (`README-PC-VR.md`): SteamVR and other PC headsets, a view
for spectators on the monitor, a gamepad in the game that can be moved where nothing tracks
it, an easier catapult with a stick, blowing with buttons.

**App 0.19 (2026-10-06): what players wrote elsewhere** (the project's thread on Reddit, the
comments under the videos about it).

- **The game speaks the headset's language.** It was in English whatever the headset was set
  to: the emulator's console was set to English and nothing offered another. The app now
  tells the emulator the language the headset is set to, and the game takes it as it takes a
  PlayStation's, if it has it (28 languages; English otherwise). `language=` in the settings
  names another. Tried on the PC, where the same emulator shows the game's first screens in
  French, Japanese and Arabic; on the headset, without wearing it.
- **Turning the view by steps**, for whoever sits where they cannot turn round (the game has
  things behind the player, and expects them to look): the left grip held (L1 on a gamepad;
  the right grip with `pad_hand=left`), each flick of the right stick to a side turns the
  view 30 degrees that way. The turn is about the head: the player stays where they are in
  the game and faces another way, and the controller comes round with them. The game is
  told nothing; to it, the player has turned. A reset of the view faces them straight ahead
  again. `turn=45` for another step, `turn=0` for none. The card that names the buttons has
  a line for it. Tried on the PC against a simulated headset (the view turns, the head stays
  where it is); **not worn**.

What else is new is for the PC: a launcher that no longer asks for a runtime that is
installed, and a gamepad and the headset's controllers that take turns there too.

**App 0.20 (2026-10-06): two things in world 2**, both in the emulator and so the same on the
PC.

- **An invisible wall at the end of level 2-1** (issue #16): the collisions of the last two
  islands were most of a block above what was drawn. See "A time step that changes, and the
  collisions the game moves" under "Speed".
- **Squares in the lava's glow around the octopus** at the end of world 2. See "One surface
  as two targets of a draw" under "The picture".

Both were found and tried on the PC, in the headset through Virtual Desktop. The Quest app
has the same emulator code; it had not been run on a headset when this went out.

**App 0.21 (2026-10-13): "Detail further away" in the start-up menu.** The menu has a second line
(Up / Down choose the line, Left / Right change it): *Lighter: faster*, the default, has the
game's models change to their simpler forms at a third of the distances the game has for that;
*As on the console* leaves them as they are. In level 3-2 the lighter setting halves the
vertices the GPU draws (3.7 instead of 7.4 million a frame) and takes a sixth off the GPU's
time for a frame; side by side the pictures are hard to tell apart. It also has the sun's
shadows drawn into maps of 1024x1024 instead of 2048x2048 (softer shadow edges). The choice is remembered,
`detail=` in `vrhost.txt` sets it. See "What a frame costs the headset's GPU" under "Speed".
A third line, *Foveated rendering* (Off / Low / Medium / High / Very High, Medium the
default), has the edges of the picture shaded in blocks of 2x2 pixels where the lenses show
less; with both at their defaults a frame of 3-2 takes 14.3 instead of 20.1 ms of the GPU. A
fourth line, *Display refresh rate*, has 90 Hz (the default) or 120 Hz; the display switches as
soon as it is changed. (The game runs at 30 frames a second at either: every frame is shown
three times at 90 Hz, four times at 120, where the compositor's work leaves the game a little
less of the GPU.)

Also in 0.21: the frame rate changes less often (a faster pace is only tried after
five seconds in which everything a frame takes fits it with a quarter to spare, and a failed
try makes the next wait longer; see "The size of the picture" under "Speed"), and the thread
that hands the game's drawing to the GPU takes a fifth less time a frame in 3-2 and a quarter
less in the world select (see "What the GPU thread does for a draw").

**Also in 0.21: more of the GPU left for the picture's size** (the night of 2026-10-09).
The first worn session with the above played the first level at a steady 30 frames a second,
but at 816x870 an eye throughout, the smallest size, with the GPU busy 76 to 82% of the time in
the level's ordinary views and 95 to 99% in its heaviest (where frames fell to 25 a second).
Measured since in that level, in the same views and with the same settings, the headset not
worn (`SHADPS4_DBG_AB`, the two ways taking turns in one run; sandbox GPU times at 690 MHz):

- **the game's constants as uniform buffers.** What a draw reads and does not write, up to 16
  KB (the constants of its shaders, mostly), reaches the shaders as uniform buffers instead of
  storage buffers. Adreno's driver loads uniform buffers into the shader's constant registers
  before the draw's work starts; storage buffers it reads from memory over and over, and the
  work waits for that. A frame of the first level takes 14% less of the GPU (15.8 instead of
  18.4 ms), one of 3-2 13% less, and the thread that hands the drawing over 5% less. On by
  default with the headset's driver; `SHADPS4_UBO=0` turns it off, `=1` on with other drivers.
- **the size chosen by what a size really costs.** 960x1080 costs the GPU only 10% more than
  816x870 in the first level (21% in 3-2); the emulator counted it at 35% more and wanted a
  quarter of a frame's time to spare before it went up, so the first level stayed at 816x870.
  It now starts from 21%, wants an eighth to spare, and learns from every change of size it
  makes what the step really costs. 30 frames a second at 960x1080 come before 40 at 816x870.
  See "The size of the picture" under "Speed".
- **90 Hz by default.** At 30 frames a second the headset's compositor makes three pictures of
  each frame at 90 Hz, four at 120: one less at about 1 ms of the GPU each (less for the game
  than the 4 ms 120 Hz takes from every frame of 30). 120 Hz stays a choice in the start-up
  menu (at 30 frames a second as well since the morning of 2026-10-09).
- **the sun's shadow maps at 1024x1024** with *Lighter* detail: a frame of the first level
  takes 4% less (14.6 instead of 15.2 ms); shadows' edges are a little softer. (Leaving the
  shadow maps' 18 filtering taps out altogether would only save 3%, and the depth pre-pass
  limited to 10 m and models simplified at a fifth of the game's distances instead of a third
  changed next to nothing: none of those were kept.)
- **edges smoothed only in the middle of the picture** where foveated rendering is on: the
  smoothing of the scene's edges (FXAA, four times a frame) stops where the picture is shaded
  in blocks anyway; 0.45 ms (3%) less a frame of the first level.
- **the eyes' copy into the picture handed over.** That picture (2880x1536) was cleared before
  the two eyes were drawn over all of it, 17.7 MB written for nothing every frame; it no
  longer is. And the eyes are read as the bytes they hold (sRGB images seen through views of
  the plain format): the sharpening no longer decodes every pixel it reads only to encode it
  again. Together 1.2 ms (5%) less of the GPU a frame, measured in the self-test, where the
  host's copy and a compositor stand-in run as in a session. The app's own copy into the
  compositor's image tells the driver that the image's old contents are not needed, and is
  timed on the GPU: `host.log`'s pacing lines say what it took.
- draws that write nothing (the game's tests of what is in view; about a hundred a frame in
  the first level) are left out: 1.6% less GPU time in 3-2.
- in the levels whose sun is black (FOREST CASTLE, INDY CAVE 2, the graveyard, the spider
  boss, and two challenges whose sun is at most a hundredth of a lit one's) its shadow maps
  are left out: a shadow pass of a tenth of a frame that shaded nothing (the game's own files
  for the inside of the whale do the same). Not where a level shares its file with one that
  has a sun (CANNON MANIA and CAVE & CANNONS, the whale's levels, LAVA RUN).

All together, in the first level's ordinary view at 816x870 (the headset not worn, GPU at
690 MHz): 18.3 ms of the GPU a frame before, 14.7 now (a fifth less), and 960x1080 costs 16.2;
in a session the eyes' copy and one picture of the compositor fewer a frame come on top
(about 2 ms). In the level bench (the self-test with a compositor stand-in at 90 Hz, the
level's heaviest view), the emulator now holds 30 frames a second at 1200x1280 with the GPU
busy 79% of the time; at a session's slower GPU clock that is 960x1080 to 1200x1280. What a
worn session makes of it is still to be seen.

Looked into and left: the game's anisotropic filtering (it asks for 2x and 4x only); pushing
a draw's descriptors only where they change (0.6 ms in a frame cut down to one triangle a
draw, and it would undo what the uniform buffers gain); drawing both eyes at once, see
"Drawing both eyes at once" under "Speed".

**Also in 0.21: 30 frames a second throughout, and only the picture's size changes** (after
the session of the morning of 2026-10-09, which played the world select, the first level and
3-2 with the build above). Frames dropped in the menus before the first level, and 3-2 slowed
down in its middle. The session's log shows why:

- the emulator changed the picture's size 43 times in nine and a half minutes (816x870 to
  1440x1536 and back). 15 of its 23 steps up were taken back within half a minute, and every
  one of its eleven trips above 960x1080 ended with frames of 35 to 45 ms: those are the
  slowdowns of 3-2 (816x870 held the game at full speed there);
- it tried 45 frames a second for 104 seconds in all, at the edge of what fitted, and in the
  menus at 1440x1536, which the scene behind them could not keep up with once it turned heavy;
- the menus' first scenes loaded while every shader they use was compiled anew: the first
  session after a new build compiles each of them once.

A replay of that log through a model of the emulator's rules (every set of rules run 40 times,
with the GPU's work changing from moment to moment as it did in the session) chose the rules now
in place:

- **always 30 frames a second**: frames are given three refreshes at 90 Hz and four at 120, in
  the menus and in the levels alike; only the picture's size follows what the GPU has time
  for. `pace=` in `vrhost.txt` still sets a number of refreshes instead.
- **1200x1280 at most** (`largest_resolution=` sets another): the larger sizes fitted the GPU's
  time only in lighter moments, and were taken back seconds later. (First 960x1080; the session
  of midday 2026-10-09 played the first level at that size with the GPU busy only 55 to 65% of
  the time, room the cap left unused: 1200x1280 since.)
- **a larger size only after four seconds in a row** in which it would have fitted with a fifth
  of a frame's time to spare (a frame that came late starts the count again), and not within
  ten seconds of any change. One half-second that fitted was enough before, and the views of
  these levels change by 12 to 20% within seconds. What a step was seen to cost goes to the
  log only: counted, it had drifted to steps that looked almost free, which made steps up
  eager.

In the replay those rules change the size about five times in the session's nine and a half
minutes instead of 43, and hardly ever take a step back. In the level bench (the first level's
heaviest view, with a stand-in for the compositor at 90 Hz) the size did not change once in 320
seconds: 960x1080 at 30 frames a second throughout. What these rules leave of the slowdowns:
the moments the processor needs longer than a frame for the heaviest views (the thread that
hands the game's drawing to the GPU takes 20 ms and more a frame there), and the shaders of a
new build compiled in its first session. The GPU's driver keeps what it compiled in the app's
cache: in the sandbox, the menus and the first level played with that cache emptied had 16
seconds below 27 frames a second and one stop of 1.4 seconds; played again with it filled, 8
such seconds (the game's own loading, the same in both runs) and no stop.

Also in 0.21:

- **Foveated rendering: Very High**, a fifth choice in the start-up menu: nine tenths of the
  picture in blocks of 2x2 pixels, its outer three fifths in 4x4. It takes the GPU 5.5% less
  than High in 3-2 and 3% less in the first level, see "Foveated rendering" under "Speed"; the
  outer part of the picture is visibly coarse.
- **the start-up menu stands still in the room** instead of following the head: 1.5 m in front
  of where the head faced when it appeared, a hand's breadth below the eyes. The headset's own
  reset of the view brings it back in front, and so does putting the headset back on. (The
  card on the controls that OPTIONS brings up, and the notice of a game that stopped
  drawing, still follow the head: they are to be read wherever the player looks.)
- the log says how long the driver took over every new pipeline ("Graphics pipeline ... made
  in ... ms").
- **black squares at the end of the first level**: not seen here at first. Ruled out then: a
  NaN let through by max, min or clamp (the headset's GPU drops NaN in those as the
  PlayStation 4's does: tested on the device). The pictures taken while sizes changed every
  two seconds showed none, but they were small, and the scan of them only looked for black in
  lit surroundings, where the squares later turned out to be on the dark sky. Found and fixed
  since: see "no more black squares in the right eye" below. The GPU of the
  PlayStation 4 has legacy multiply-adds where zero times anything is zero; the emulator now
  does them that way too, although no shader of the menus or of the first level uses them.

**Also in 0.21: FSR 1 for the game's enlargement of its picture, on by default** (after the
session of midday 2026-10-09: "the resolution is still very low; could some kind of DLSS
upscale the picture?", then "make FSR 1 nearly free"). DLSS itself needs an NVIDIA GPU, and the
upscalers that work over time (DLSS, FSR 2, Snapdragon GSR 2) need motion vectors for every
pixel, which this game never draws. What fits is a spatial one: the game draws its scene at
816x870 to 1440x1536 an eye and enlarges it to 1440x1536 itself, bilinearly, in its final pass
(the one that also sets its tone and levels). With *FSR 1* on the fifth line of the start-up
menu, *Picture enlarged with* (the default), that pass reads its scene with AMD's FSR 1 edge
adaptive filter (EASU) instead: the emulator compiles the game's own final pixel shader with
the filter written into it in place of its one bilinear read (`TitleUpscale`,
`spirv_scene_easu.cpp`), so the rest of the pass is the game's and no picture is made on the
way. Only in the middle of the view, where it is shaded finely (beyond it EASU makes the blocks
of the coarse shading stand out), and wherever the scene is enlarged at all (816x870, 960x1080
and 1200x1280); the eye copy sharpens that part half as much. The same pass also makes the
picture for the TV, from a 640x360 scene, which nothing shows while the headset is in use: that
one is left as the game makes it (since 2026-10-11). The filter's arithmetic is done
in 16 bits, as in FSR 1's own version for that, and where the four texels nearest a pixel are
all but the same, the game's bilinear read stays (the filter could only return something
between them too). On the PC, the first level at 960x1080: edges, leaves and outlines crisper;
no detail is added.

What it takes the headset's GPU (the first level, 30 frames a second, the two ways taking turns
in one run): **about 0.65 ms a frame**, at 960x1080 and at 1200x1280 alike, where the first
version (the filter made a picture of its own, which the game's pass then read) took 3.6 ms.
That first version cost as much as the next larger scene size, so with it on the scene was
drawn smaller more often (the session of 2026-10-09 18:19 stayed at 816x870); now the next size
costs four times what FSR does (1200x1280 takes 2.7 ms more than 960x1080 there). Of the 0.65
ms, 0.2 is the filter's code sitting in the shader (it looks at every pixel whether to filter
there), the rest the filter itself. (The measuring runs never had the TV's picture made; in the
session of 2026-10-11 16:36 it was, and the filter on it would have added about 0.1 ms by its
size.) `fsr=0` in `vrhost.txt` has the game enlarge its picture
itself, as the menu's *The game's own* does.

**Also in 0.21: no more black squares in the right eye** (issue #19; the user, 2026-10-13:
"blocks of pixel just appears black for a couple of seconds then switch back to normal", right
after *Continue* and in levels). The game draws both eyes into the same depth buffer, the second
right after the first; the emulator cleared it for the right eye without a barrier after the left
eye's pass, as the buffer stayed in its layout. The headset's GPU driver, drawing straight to
memory, then let what the left eye's pass still had in its cache land on the cleared buffer
later, block by block, and where it did the right eye's sky failed the depth test: black
squares of 16x16 of the scene's pixels, for a second or two after the picture changed size.
A pass that clears a target now waits for the passes before it (`Scheduler::BeginRendering`);
the GPU takes no measurable time more for it (the first level, the two ways taking turns in
one run). Found with `SHADPS4_DBG_EYE_SCAN` in a worn session, then made to happen at will in
the sandbox by changing the size every four seconds: 8 to 12 pictures with squares a run
before, none after. Also: the card on the controls no longer comes up as the game starts,
only when OPTIONS (the menu button) is pressed.

**App 0.21.1 (2026-10-13): a crash at the last boss.** The emulator stopped there (`core.log`:
"Trying to unregister an already unregistered image"), right after the picture's size had
changed three times within a second. Its texture cache keeps a list of the images by when they
were last used, from which it lets go of the oldest when memory runs short. An image already
let go of is looked at once more by whoever still holds it, to learn that it has to be found
again, and the cache's stand-ins for missing textures were never in the list: both counted as
used, and that put a free entry back into the list, whose image the cache later let go of a
second time (or the entry, handed to a new image meanwhile, was in the list twice: probably the
one heap corruption seen under quick changes of size on 2026-10-09). Only images in the cache
count as used now, and the cache never lets go of an image twice. The same in the PC's build.

**App 0.8 (2026-10-03)**, after the fourth session (levels 1-1 to 1-3 played, the game stopped
on entering 1-4 twice):

- **The crash at level 1-4 ("Cliffhanger")**: the level is the first to draw with a geometry
  shader fed with a triangle fan, a kind of primitive the emulator's shader compiler had no
  case for; it stopped on purpose there (`spirv_emit_context.cpp: NumVertices: Unreachable
  code!`, the emulator ends with signal 5). Fans, polygons, line loops and the strips with
  adjacency are handled now; 1-4 loads and plays on the PC and in the headset's sandbox with
  the player's own save. The controller's vibration just before is the game's: what the game
  asks of the motors lasts a second at most once the emulator is gone.
- **Saves**: the game could miss its save at start (the adventure began anew; it happened on
  the PC every time, here only by luck of timing): the emulator handed the game the player's
  login before the game had started the user service. Fixed; see `README-PC-VR.md`.
- **Head-butting**: the headset's position reaches the game (every ten seconds core.log says
  where it has the head: it follows the player's), and from 0.8 the emulator also works out
  how fast the head moves where the headset's system does not say, since the game only takes
  a push of the head for one above some speed.
- **`fov=`**: a narrower field of view than the PlayStation VR's for a sharper picture (see
  the settings below). In 0.8 it defaulted to the console's own, so nothing changed unless
  set; **app 0.9 (same day) makes 85% the default**: 18% more pixels to the degree.

## How it fits together

```
Quest 3
└─ Astro VR Host (Android app, OpenXR)
   ├─ shows the stereo frames as a projection layer; the compositor reprojects them to the
   │  current head pose every refresh, like PSVR's own reprojection did
   ├─ sends the head pose (and lens distance) to the core every display refresh
   ├─ makes the DualShock 4 the game expects of what the player holds: a gamepad paired with
   │  the headset (of a DualSense everything: buttons, sticks, triggers, touchpad and motion
   │  sensors in; rumble and light bar colour out) or the headset's own controllers
   ├─ places that controller in the game: one of the headset's own is where it is; a gamepad
   │  is where the hands are that the headset sees holding it (otherwise at a fixed spot in
   │  front of the player), turned by its motion sensors or, where it has none, by the hands
   ├─ plays the sound (one audio stream per port the game uses)
   └─ starts the emulator core as a child process ──► shadps4 (aarch64 Linux, glibc)
                                                       ├─ FEX runs the game's x86-64 code
                                                       ├─ Turnip (Mesa) Vulkan driver on the Adreno GPU
                                                       ├─ emulated PSVR: HMD, tracker, camera,
                                                       │  reprojection, social screen
                                                       └─ sound: native Ngs2 engine, 3D audio,
                                                          7.1 rendered for the ear speakers
```

Frames travel from the core to the app in shared GPU buffers (AHardwareBuffer / dma-buf), poses and
controller data over unix sockets, audio over a third socket.

## Building

Everything builds on this Windows PC from Git Bash, no admin rights, WSL or Android Studio project:

```sh
source tools/env-win.sh && cmake --build build/win-x64 --target shadps4   # PC test build
bash tools/build-arm64.sh                                                  # core for the headset
bash quest-host/build.sh                                                   # APK
```

`quest-host/build.sh` takes the Linux runtime the core needs (glibc, its loader, the Turnip driver)
out of `bachatas4-0.2.4-release.apk`, which therefore has to stay in this folder.

## Installing and running

```sh
adb install -r build/quest/astro-vr-host.apk
adb push games/CUSA12392 /data/local/tmp/astro/games/        # 13 GB, a few minutes
```

To play with a gamepad, pair it with the headset (Settings ▸ Bluetooth; on a DualSense, hold
Create + PS until the light bar flashes); the headset's own controllers need nothing. Then start
**Astro VR Host** from the app library (Unknown Sources). A panel in front of you reports what
the app is doing, and what the game will be played with, until the game's first frame arrives.
It stands still in the room where you faced when it appeared, and does not follow your head;
the headset's own reset of the view brings it back in front of you, and so does putting the
headset back on.
With a gamepad, put the Touch controllers aside: with them out of the way the headset tracks
your hands, which is what places the gamepad in the game. See "Controllers" below for what
plays how.

In the game:

- The first screen asks to move the controller into a floating outline. Hold the DualSense up in
  front of you, where the outline is. (If the headset does not see your hands, the controller is
  assumed to be there and the screen passes on its own.)
- The world map is selected by **looking** at a planet and pressing ✕.
- Touchpad: the DualSense's own. Without one, the right stick is the finger and Create / Back /
  View the press (see "Controllers").
- **Resetting the view**: hold OPTIONS for a second (as on a PlayStation VR), or press the PS
  button; with the headset's own controllers, hold the left one's menu button for a second or
  press both sticks in. Where your head is then is where you sit as far as the game goes, and
  the way you face is straight ahead; the controller's heading is taken anew as well. The app
  does it by itself when ✕ is pressed for the first time, and when the headset's own "reset
  view" is used (hold the Meta button). Do it again whenever you have settled differently and
  things are too close, too far or off to a side: the game takes its bearings only when it is
  told to (see "Where the player sits").
- Taking the headset off pauses the game; putting it back on resumes it.
- If the headset's boundary lights up, you are near its edge: besides being in the way, the
  boundary costs the GPU time the game needs. A boundary with more room around the seat avoids
  it.

Optional settings go in `/sdcard/Android/data/com.astrobotquest.vrhost/files/vrhost.txt`, one
`key=value` per line:

| Setting | Meaning |
| --- | --- |
| `refresh_rate=90` | display refresh rate to ask for (72, 80, 90, 120; default 90, or what the start-up menu's *Display refresh rate* was set to last). The game runs at 30 frames a second at 90 and 120 Hz (every frame shown three or four times), at 36 at 72 Hz and 40 at 80, see "Speed". Seeds the start-up menu |
| `pace=2` | how many refreshes of the display every frame of the game is given. `2` is the game's own way, as up to version 0.6; a larger number holds it to that. Default: as many as make 30 frames a second (3 at 90 Hz, 4 at 120), and only the picture's size changes with what the GPU has time for, see "Speed" |
| `resolution=960` | the size the game draws its scene at, per eye: `816` (x870), `960` (x1080), `1200` (x1280), `1440` (x1536), or `game` for the game's own choice. Default: the emulator chooses, as large as the GPU manages at 30 frames a second up to `largest_resolution=`, see "Speed" |
| `fsr=0` | the game enlarges its picture itself, bilinearly, as on the console. Default: AMD's FSR 1 in the middle of the view, wherever the game draws it smaller than 1440x1536 (crisper edges, about 0.65 ms of the GPU a frame). Seeds the start-up menu's *Picture enlarged with* |
| `largest_resolution=1440` | the largest size the emulator chooses: `816`, `960`, `1200` (the default) or `1440`. A larger one makes the picture sharper where a scene is light, and changes of size (each costs a few late frames) more frequent; `960` changes it least |
| `antialias=0` | leave edges as jagged as the game draws them without multisampling. Default: the emulator smooths them, see "The picture" |
| `real_time=0` | let the game count time in frames as on the console: slow motion whenever a frame takes longer than 1/60 s. Default: the emulator has it run by the clock, see "Speed" |
| `hands=0` | do not use hand tracking; a gamepad stays at its fixed spot |
| `own_controllers=0` | never play with the headset's own controllers, whoever holds them |
| `pad_hand=left` | with the headset's own controllers, the left one is the controller in the game instead of the right one |
| `language=fr-FR` | the language the game is played in, as a language tag (`fr-FR`, `de-DE`, `pt-BR`, `ja-JP`...). Default: the one the headset is set to. The game has 28; it is in English for any other |
| `turn=45` | how many degrees the view turns for each flick of the right stick with the left grip (or L1) held (default 30; `0`: never) |
| `pad_tilt=-15` | for a gamepad without motion sensors, which the hands holding it turn and tilt in the game: how many degrees higher (lower, if negative) its front points than the hands do (-60 to 60). For a gamepad that points too high or too low in the game |
| `sharpen=0` | no sharpening of the picture. Default 1: the emulator sharpens every frame of the game once. 2: the headset's Super Resolution filter on every refresh instead (finer; about a twentieth of the GPU's time). 3: that filter whenever the system finds the GPU has time for it (in a level: hardly ever). 4: the headset's plainer filter. See "The picture" |
| `cubic=1` | have the headset enlarge the picture with a cubic filter instead of a linear one (sharper, for some GPU time on every refresh) |
| `stick_touchpad=0` | right stick no longer doubles as a finger on the touchpad |
| `motion=0`, `rumble=0` | ignore the gamepad's motion sensors (the hands holding it then say how it is turned, as for a gamepad that has none) / no vibration |
| `msaa=4` | let the game multisample as on the console (default 1 = off, much faster) |
| `stats=1` | keep a small panel in view with the game's frame rate, the display's refresh rate and whether the hands holding the controller are seen |
| `predict_ms=25` | how far beyond the next refresh the head pose given to the game is predicted (0–80). More if the picture's edges show when turning the head, less if the world wobbles |
| `dynamic_resolution=0` | do not ask the system what size it recommends for the picture (which may cost the GPU its fastest clock). `2`: also show the picture at that size, as up to version 0.6. Default: ask, and show the picture at full size, see "The picture" |
| `cpu_boost=0` | do not ask for the processor's "boost" level (its fastest clock, which the system grants for the first 45 seconds and up to a fifth of the time after) |
| `fov=100` | Initial choice in the startup FOV menu, from 50–100% of the user's headset projection tangents. The last menu choice is used when this setting is absent; the first-launch default is 85. `100` fills the runtime-reported headset FOV and adds no mask. Lower values render the same pixels over fewer degrees, with a soft squircle border around each eye. The menu's angles and average PPD gain are calculated from this headset's detected optics; the chosen FOV is logged in host.log and core.log. |
| `foveation=0` | Initial choice for "Foveated rendering" in the start-up menu: 0 every pixel shaded on its own, 1 (Low), 2 (Medium, the default), 3 (High) and 4 (Very High) more and more of the picture's edges shaded in blocks of 2x2 pixels (3: the far corners in 4x4, 4: the outer three fifths), see "Foveated rendering" under "Speed". The last menu choice is used when this setting is absent |
| `detail=console` | Initial choice for "Detail further away" in the start-up menu: the game draws everything as on the console. The default, `lighter`, has its models change to their simpler forms at a third of the game's distances (`SHADPS4_TITLE_LOD=0.333`), a sixth less GPU time in level 3-2, and the sun's shadows drawn into maps of 1024x1024 instead of 2048x2048 (`SHADPS4_TITLE_SHADOW_MAP=1024`). The last menu choice is used when this setting is absent |
| `mic=0` | the game does not get to hear the headset's microphone (see "Microphone") |
| `mic_gain=2` | make what the game hears that many times louder (0.1–30) |
| `arg=...`, `env=NAME=value` | extra emulator argument / environment variable |

What a session leaves behind, in the same folder as `vrhost.txt`:

| File | Content |
| --- | --- |
| `host.log` | the app's own log: settings, controller found and what it offers, the OpenXR session, and every ten seconds a line with the game's frame rate against the display's, the processor time and memory the emulator used and the cores it ran on, a line on where the head is in the headset's own space and how far it moved, a line on what was seen of the hands and where the controller was taken to be, every reset of the view and what asked for it, the VR runtime's own counters ("runtime:": GPU load, the compositor's time, frames it had to repeat, load per processor core), and how loud the microphone was |
| `core.log` | the emulator's output, with a few lines every five to ten seconds on the frame rate and what a frame's time goes to, on how each sound port keeps up and on what the game's microphone port heard; "The title's clock" (how long frames take, the time step the game is given, the speed it ran at, the size it draws at, how many refreshes a frame is given and how busy the GPU was), "The scene is drawn at" and "Frames are given ... refreshes" (every change of size and of pace, and why), "The title has the player's seat at" (where the game takes the player to sit and where the head is from there: near zero when the view is right), "Seat recentred" (every reset of the view), "The headset refreshes ... after the host's display" (how the emulated headset keeps step with the real one) |
| `host.prev.log`, `core.prev.log` | the same of the start before the last one |

`adb pull /sdcard/Android/data/com.astrobotquest.vrhost/files/host.log` fetches one; they are
what to look at (or send along) when something did not work. `adb logcat -s AstroVR` shows the
host's log live. The emulator's own folder (saves, `sys_modules`) is `.../files/data/shadPS4/`.

## Controllers

The game is played with one DualShock 4, which the app makes of what the player holds.

**A DualSense** (or a DualShock 4, which the headset's system knows the same way; untested) has
everything the game uses: its touchpad, its motion sensors for how it is turned, rumble and
light bar. Where it is in the game is where the headset sees the hands that hold it.

**Other gamepads** (the headset's system has drivers and key layouts for Xbox controllers,
8BitDo, Logitech, Razer, PDP, Hori and others; nothing but a DualSense has been tried) lack
two things the game is built around, which are made up for:

- *the touchpad.* The game asks three things of it, and each is on a button the game has no
  use for while it is played (it is still told of them: a few of its menus have):
  - **R2, the right trigger: the pad pressed**, for as long as the trigger is pulled. The
    water cannon and the machine gun fire that way.
  - **R1: a swipe forward**, once for each press. That shoots the hook, throws the stars,
    opens the chests.
  - **L2, the left trigger: a pull back**, held while the trigger is and let go when it is.
    That is the catapult at the end of every level (point the controller at the goal: the
    game only shoots within 25 degrees of it), and it pulls on the hook's rope.

  A card in the headset names them whenever OPTIONS is pressed, which is the game's pause
  (not as the game starts, where it was in the way). Besides them the right stick, for which this game has
  no use either, moves a finger over the pad for anything else: the finger comes down behind
  the pad's middle, is dragged the way the stick is pushed, and lifts where the stick is let
  go (not back in the middle, where a stick flies to). The Create / Back / View button
  presses the pad. (`stick_touchpad=0` leaves the stick a stick.)

  Why buttons, and why the finger is slow: the game looks at the pad once for every frame it
  draws, thirty times a second in a level here, and makes a pull or a swipe of where it saw
  the finger first and last. A stick is at its end within a few hundredths of a second, and
  a finger that follows it there has not moved at all as far as the game can tell. So every
  finger the app moves stays where it lands until the game has seen it, goes no faster than
  a real one flicks, and lifts only when the game has seen it at its end.
- *the motion sensors*, by which the game knows how the controller is turned: its gadgets aim
  with it. The hands holding the gamepad stand in: the line from one palm to the other is the
  gamepad's sideways axis, which gives how it is turned and how far it is rolled, and the way
  the hands point is how far its front is tilted up or down. That is as steady as the
  headset's view of two hands around a gamepad, and off by however a gamepad's shape has the
  hands point: `pad_tilt=` moves it if it points too high or too low in the game. Motion
  sensors of gamepads that are not PlayStation's are not used even where the system offers
  them: which way is up for a motion sensor is each maker's own affair. (`motion=0` has a
  DualSense go by the hands as well. For a gamepad that has both, `host.log` says every ten
  seconds how far the two are apart: "hands against the gamepad's motion sensors".)

**The headset's own controllers**, for whoever has no gamepad. The app reads them through the
headset's runtime, which says where they are and whether anybody holds them (the gamepad the
system makes of them for other apps says neither, and is ignored):

| On the controllers | In the game |
| --- | --- |
| the right controller, where it is and the way it points (`pad_hand=left`: the left one) | the controller |
| left stick, pressed in | left stick, L3 |
| A, B (right) | ✕, □ |
| X, Y (left) | ○, △ |
| X and Y together | blowing into the microphone, for as long as they are held |
| right trigger | the touchpad pressed, while it is pulled (water, guns); also R2 |
| right grip | a swipe forward on the touchpad (hook, stars, chests); also R1 |
| left trigger | a pull back on the touchpad, let go when the trigger is (the catapult); also L2 |
| left grip | L1; held, the right stick flicked to a side turns the view a step (see "Where the player sits") |
| right stick | a finger on the touchpad, as on a gamepad without one |
| right stick pressed in | the touchpad pressed |
| menu button (left) | OPTIONS; held for a second, it resets the view |
| both sticks pressed in | resets the view |

(With `pad_hand=left` the trigger and the grip that press and swipe are the left ones, the
right trigger pulls, and the right grip turns the view.) Both controllers shake with the
game's rumble, the left one more with the heavy motor and the right one more with the light
one.

**With a gamepad and the headset's controllers both at hand, the game is played with the one
that was used last**: a button on the other takes over, and `host.log` says so. From a
gamepad, only what a hand cannot do without a controller in it takes over (a button under a
thumb, a stick): while a gamepad is held it is the hands the headset sees, and their pinching
must never count. The start-up menu takes either: a stick to a side chooses, ✕ / A or a
trigger starts the game, and what started it is not pressed in the game.

## Where the player sits

The game takes the place of the player's head once, from the first position the tracker gives
it a few seconds after it starts, and from then on places everything around that spot; it never
asks which way the player faces, because a PlayStation VR player faces the camera. It takes
stock again only when the console says the view was reset, or when OPTIONS is held. (Not when a
level starts: with the head half a metre forward of where it was at the start, the PC build
loads the first level and stays half a metre forward.) On the Quest the first position is
wherever the head was a few seconds after the app was started, in a space whose origin and
heading are the headset system's affair. In the second session that put the player ahead of
where the game meant them to be for the whole session: the hero stays 0.4 to 1.1 m in front of
that spot, so less than half a metre is enough to stand on top of him.

The emulator now keeps the player's seat itself (`src/core/vr/vr_runtime.cpp`): the position and
heading of the head at the moment the view was last reset. What the game is given is counted
from there, and with every reset it is sent what the console sends, so that it takes stock too.
The view is reset when the first pose arrives, when the game first asks for one, when ✕ is
pressed for the first time (the player has settled, controller in hand, looking at the title),
when OPTIONS is held for a second or the PS button is pressed, and when the headset's own reset
is used. `core.log` says each time where the seat is, and every ten seconds where the game takes
the player to sit and where the head is from there.

**Turning round without turning** (0.19) is the seat turned about the head, 30 degrees for
each step (`Runtime::TurnView`): the head keeps its place in the game's world and faces
another way, a controller the headset sees is placed anew from the turned seat, and one it
does not see keeps its place before the player. The game is not told: a player on a swivel
chair would look the same to it. The steps are asked for with a button the game has no use
for in play, held, and the right stick flicked to a side (the left grip, or L1; see
"Controllers"), and are undone by the next reset of the view. `core.log` says each one
("The view turns a step to the right: ...").

## Speed

**The game counts time in frames.** It moves its world on by a sixtieth of a second for every
frame it draws, however long the frame took. On the console that is the same thing; here it is
not, and in the first two sessions the whole game ran at little more than half its speed (30 to
35 frames a second). The emulator now gives it, frame by frame, the time its frames really take
(the engine keeps its step in three variables, written by the one function that sets its frame
rate; `src/core/known_title.cpp`). Slowed down to 43, 30 and 22 frames a second, the PC build
then follows the same course to the second as at 60, and a scripted walk through the first level
has the hero in the same places at the same moments. Below 30 frames a second the game slows
down rather than take steps it was never tried with. Up to 0.20 that limit was 20, and at 30
frames a second the frames after every hitch were given longer steps to catch up; with steps of
37 ms and more (35 still pass) something in the game comes out as no number at all. In level
3-2 that left the robot that hangs tied up there at no place from the moment the level loaded,
and its cry for help, which fades with distance, was heard at full strength wherever the player
was, for most of the level. `real_time=0` turns this off.

**A time step that changes, and the collisions the game moves.** One thing in the game did
depend on every frame's step being the same. The collisions of whatever moves (a platform, a
door, a part of a level that rises) are bodies the game sends to a place: the physics library
gives the body the speed that takes it there in one step, worked out with the time step of
the step it last took, and the next step moves it by that speed times the new time step.
With two different time steps the body lands off by its way times their difference. For what
moves all the time that is put right a frame later. For what is sent once it stays, and
levels do that: they sleep by sections, and a section that wakes sends all its bodies in one
frame from where they slept to where the level has moved since. The last two sections of
level 2-1 wake at the top of a tree that carried them 17 units up while they slept: their
collisions ended most of a block above what was drawn, an invisible wall before the last
enemy (issue #16). The function that takes the step is changed in the game's image, in both
versions of the game, to step with the time step the bodies were sent with
(`src/core/known_title_builds.h`, `PhysicsStepChanges`); bodies then land where they are sent
to the last digit, across hitches too.

**Two refreshes for a frame, or three.** The game draws a frame for every two refreshes of its
headset (60 frames for the 120 Hz of a PlayStation VR) when it manages. When a frame takes
longer it goes straight on to the next one, at whatever rate that makes, and that rate has
nothing to do with the display's: at 33 frames a second on a display of 72 Hz one frame is on
show for two refreshes, the next for three, as they happen to fall, and whatever moves in the
picture moves in jerks. That is what the first level was like up to version 0.6.

The emulated headset refreshes with the Quest's display. The app tells the emulator of every
refresh; the emulated headset keeps step with those and refreshes at the moment after them that
has the game's frames arrive midway between two looks of the app (a clock of its own would
drift against the display). From 0.7 on it refreshes more slowly than the display wherever the
game cannot keep the display's pace: one and a half times as slowly, so that the game's two
refreshes are three of the display's. From 0.7 the app asked for 90 Hz: 45 frames a second
where the game manages a frame in two refreshes (title, menus, world map, lighter scenes) and
30 where it takes three (the levels), and either way every frame is on show for exactly as long
as the next. In the self-test, with a display and a compositor of 90 Hz standing in, 292 to 300
of 300 frames stay for exactly three refreshes in the first level's ordinary views, and 87 to
96 of 100 in its heaviest. The start-up menu's *Display refresh rate* offers 120 Hz as well:
60 frames a second for two refreshes, 40 for three, 30 for four, so that levels which cannot
make the 22 ms of 45 frames a second get 40 wherever a frame takes no more than 25 ms instead
of falling to 30. But the compositor's work at every refresh, about 1 ms of the GPU at a field
of view of 80% (0.6 to 0.8 at 100%), is then done four times for each frame of 30 instead of
three, and what it takes the game does not have: 90 Hz is the default again since the night
of 2026-10-09, when the emulator also stopped trading the picture's size for 40 frames a second
(see "The size of the picture" below). Head movement is smooth at the display's rate regardless: the
compositor turns the last frame to where the head points by now.

Frames are never given more refreshes than still makes 30 frames a second: a game that cannot
do that either is left to draw as fast as it can. At `refresh_rate=72` and `80` there is
therefore only the one pace (36 and 40 frames a second), and the levels run beside the display
as they did; at `120` there are three (60, 40 and 30). `pace=` holds the game to one number of
refreshes throughout.

(What does not work is holding a finished frame back until its refresh: the game does not wait
for its frames to be shown, so they pile up behind the one that is held - a tenth of a second
late on the PC, seconds on the headset. The first attempt did that.)

**The size of the picture.** The game draws its scene at one of four sizes (816x870, 960x1080,
1200x1280 or 1440x1536 an eye; a base PlayStation 4 moves between the last three, and only goes
down to the first in a few levels), enlarges it to 1440x1536 itself and hands that to the
headset. It chooses the size by how long it finds the GPU to take over a frame, and emulated,
the clock it reads for that says how long the emulator took to pass the drawing on, not how
long the GPU needs for it. The emulator therefore chooses in its place, pace and size together
(`Governor` in `src/core/known_title.cpp`). It looks eight times a refresh whether the GPU
still has work of the game's before it, and at the GPU's clock: a GPU with little to do slows
its clock down and is then busy for longer with the same work, so its work is counted at the
fastest clock it was seen to run at. It goes by that, by how long frames take, and by how much
of the processor the thread that translates the game's drawing uses a frame:

- frames that fit the refreshes they are given take exactly that long, on average; frames that
  take 3% longer do not fit;
- in the Quest app the pace is held to 30 frames a second throughout (three refreshes at 90 Hz,
  four at 120; `SHADPS4_VR_STEADY_FPS=30`), and only the size changes. Where the pace is left to
  the emulator (`SHADPS4_VR_STEADY_FPS` not set, as on the PC), it is the fastest at which
  960x1080, the console's own smallest, fits with a quarter to spare: the GPU's work (counted
  as if a smaller picture saved little), the translating thread's time, and the game's own work
  where a try at that pace showed it to be too much (until the scene is clearly lighter, or ten
  minutes later). It is only tried after five seconds in which all of that held. Only at the
  slowest pace (30 frames a second) will 816x870 do: 960x1080 at 30 frames a second comes
  before 816x870 at 40, and 816x870 before fewer than 30;
- the size is the largest that fits the pace with a tenth to spare, up to the largest allowed
  (`SHADPS4_VR_LARGEST_SIZE`; the Quest app allows 1200x1280 unless `largest_resolution=` says
  otherwise). A step up to a larger one is taken only after four seconds in a row in which it
  would have fitted with a fifth to spare (a frame that came late, or one that held everything
  up, starts the count again), not within ten seconds of any change of size or pace, and not
  in the 15 seconds after the pace got faster. The sizes are counted as costing a little more
  than half as much as 816x870 plus their pixels (960x1080 1.21 times 816x870, 1200x1280 1.52,
  1440x1536 1.95). What every step between two sizes really cost (the GPU's work a frame in the
  windows before against two windows from half a second after) goes to the log; it does not
  change what the sizes are counted as, since while the player moves a step shows as much of
  the view changing as of the size (counted, the steps had drifted to looking almost free in
  the session of the morning of 2026-10-09, which made steps up eager). Measured with the
  headset's GPU at a fixed pace and the sizes taking turns in one run, with the models lighter
  and the edges shaded coarsely: in 3-2, 960x1080 cost 1.21 times what 816x870 did and
  1200x1280 1.20 times what 960x1080 did; in the first level, with 1600 draws a frame, 960x1080
  cost only 1.10 times 816x870. Up to the night of 2026-10-09 the sizes were counted at 1.35,
  1.87 and 2.59 and a step up needed a quarter to spare, which held the first level of the
  session that night at 816x870 all through, with the GPU busy 76 to 82% of the time in its
  ordinary views;
- when frames stop fitting and the GPU is what holds them up (busy 85% of the time or more), a
  step up in size from the last half minute is taken back; otherwise it goes straight to the
  size that the GPU's time says will fit, and (where the pace is the emulator's to choose) to
  the next pace only where no size does; when it is the game's own work that takes long, to
  the next pace;
- a faster pace that had to be taken back makes the next try wait 20 seconds, then twice as long
  each time (up to 2 minutes); a size that had to be taken back counts as dearer (6% at a time,
  20% at most, and as before after two seconds of loading in which nothing was drawn), and the
  next step up waits 15 seconds, then twice as long each time.

In the sandbox at 120 Hz, from the title into 3-2 (290 seconds): before, the frame rate went
from 40 to 60 and back to 40 within seven seconds, and the size changed every few seconds at
the start; with these rules it changed pace five times, never on a try that failed, and held
40 frames a second at 960x1080 for the last two and a half minutes. It went to 30 only in the
heaviest view, where the translating thread needed 23 ms a frame.

Up to 0.6 the size went back and forth between 816x870 and 960x1080 all through the first
level (fourteen times in six minutes of the third session): at 72 Hz the smaller one used 78 to
94% of the GPU's time and the larger one did not fit. With three refreshes of 90 Hz for a frame
there is room. In the self-test's first level (GPU at 690 MHz, compositor stand-in at 90 Hz):
1200x1280 at 30 frames a second with the GPU busy 78% of the time in its ordinary views, 816x870
at 30 with the GPU busy 97 to 99% in its heaviest (the whole level in view), 1440x1536 at 45 in
the world map. A session has a slower GPU clock (599 MHz): expect the heaviest views to fall a
little short of 30 there, and the ordinary ones at 960x1080 to 1200x1280. `resolution=` holds
the game to one size, `resolution=game` leaves the choice to the game.

Measured on a Quest 3 in the first level, without the headset worn (`tools/quest-sandbox-test.sh`):
about 30 frames a second before the changes below; after them 59 at the start of the level (860
draws per frame) and 40 to 42 looking down the whole level (1800 draws), where GPU and processor
are both at their limit. A real session differs: the emulator has three processor cores of its
own there (3 to 5, at 1.92 GHz), the GPU runs at 599 MHz at most (690 in the sandbox), and the
compositor takes 0.7 to 1 ms of it at every refresh. The second session (app 0.5, at 120 Hz):
40 to 56 frames a second in the prologue, 21 to 40 in the first level, with the GPU 97 to 99%
busy and the emulator using two of its three cores. The third (app 0.6, at 72 Hz): 33 to 36 in
most of the level and 24 to 32 at its start, at the game's proper speed. The self-test can
stand in for that: `HOST_COMPOSITOR=90` among its settings takes the GPU away from the emulator
the way the compositor does, which makes the GPU the limit there too (its frame rates times
0.87 for the slower clock are about what a session gets), and `tools/quest-level-bench.sh` runs
it into the first level that way.

Three programs share the GPU in a session: the emulator, this app (which copies each finished
picture to where the compositor takes it from) and the compositor. Left to itself the driver
treats the first two as equals, and the app's millisecond of copying then waits behind everything
the emulator has queued for the pictures after it: in that first session up to 54 ms, with the
app's own frame loop down to 42 rounds a second. The emulator's GPU work therefore runs at the
lowest of the kernel driver's priority levels (`KGSL_COMPAT_PRIORITY=12`, set by the app; the
Mesa driver has no way to ask for that itself, the library that sits between it and the kernel
does it). In the self-test, with the GPU kept busy by 4x multisampling, the copy went from 15 to
38 ms on average (76 at worst) to 1.5 ms (9 at worst), and the game got 15% more frames out.

What made the difference, in the core (`shadps4-arm64-main`):

- The frame's GPU commands are handed to the driver in parts while the frame is still being
  translated, not all at once at its end, so that the GPU works at the same time as the thread
  that feeds it (`SHADPS4_FLUSH_DRAWS`, 64 draws by default on the headset build).
- Images are created with the list of formats they will be viewed in. Without it Turnip has to
  store every colour image uncompressed and untiled.
- A render pass is no longer ended by a change that does not need it (a target that has just
  been cleared, depth writes being switched off and on): 113 passes per frame became 52.
- Per-draw work of the GPU thread that only repeated itself is remembered (fetch shader layout,
  texture descriptions and lookups, image layout transitions), and a pipeline that is already
  bound is not bound again (which made the driver resend its state to the GPU).
- A finished frame is flipped when the GPU is done with it, not at the next refresh.
- The emulator has more busy threads than the headset has cores for it. The sound thread and the
  two threads every frame passes through get a higher priority than the game's helpers
  (`SHADPS4_THREAD_NICE`, set by the app). (The VR runtime's own thread hints are not available
  for them: it only accepts threads of the app's process, and the emulator is a process of its
  own.)
- The GPU driver draws directly instead of in tiles (`TU_DEBUG=sysmem`, set by the app while
  multisampling is off).
- Since 0.21: the GPU driver's shader compiler puts no "preamble" before the game's
  shaders (`IR3_SHADER_DEBUG=nopreamble`, set by the app). A preamble is the part of a shader
  that is the same for a whole draw, worked out once at its start; with about 1700 mostly small
  draws a frame that costs the GPU more than it saves. 0.53 ms a frame less without (2.6%: the
  first level at 30 frames a second, four whole runs in the order with, without, without, with;
  20.05, 19.57, 19.54 and 20.12 ms). The driver keeps the shaders it compiled before apart from
  these, so the first start with it compiles each shader again as the game first uses it.

What is left is what the game draws. Looking down the first level that is about 8 million
vertices a frame (shadow maps a third, a depth pass for each eye a fifth, the two colour passes
the rest), in 1850 draws; `SHADPS4_FRAME_STATS=1` logs the count, `=2` lists it pass by pass.
With every draw cut down to one triangle (`SHADPS4_DBG_DRAW_VERTICES=3`, a measuring aid) the
GPU still has a third of its work: about 4 microseconds per draw for the state that changes
between draws, the other two thirds being the geometry and its pixels. The frame rate of that
view does not rise by it, though: there the game's own threads and the thread that translates
its draws (20 ms a frame) are the limit in the sandbox, where the emulator has three cores.

In a VR session the system holds the GPU at 545 MHz at most (level 4), and lets it go to 599
(level 5) only for apps that take part in "dynamic resolution": that ask it, from moment to
moment, at what size it wants their picture. The app asks (and up to 0.6 also showed the
picture at that size, see "The picture"); `dynamic_resolution=0` turns the asking off. Whether
the system grants the higher level shows in what `tools/quest-recorder.sh` keeps of a session
(`GPU=4/5`, `.../599MHz` in the `VrApi` lines, `Dynres boost enabled` among the reasons for the
GPU's maximum). In the second and third sessions it did: level 5, 599 MHz throughout.

The last level, 640 MHz, the system only gives to an app that also trades a processor level for
it, which is a line in the app's manifest and so a build of its own:
`quest-host/build.sh --trade-cpu-for-gpu`. The processor then tops out at 1.65 instead of
1.92 GHz, 14% less for 7% more GPU. In the second session the GPU was the one that ran out,
with the emulator using two of its three cores; but the thread that hands the game's drawing to
the GPU is one of the two, and it is the one a slower processor would hold up. Not tried in a
session; the default build does not trade.

The app also asks for the processor's "boost" level, which the system grants for the first 45
seconds and up to a fifth of the time after that (2.36 GHz): that is when the emulator
translates most of the game's code. `cpu_boost=0` turns it off.

Tried and dropped: FEX without x86 memory ordering (`SHADPS4_FEX_TSO=0`) - the game stops within
half a minute. The driver without its lowering of uniform buffers to constants
(`IR3_SHADER_DEBUG=nouboopt`) - the same GPU time. Not setting vertex layout, vertex and index
buffers and push constants again when they are what the command buffer already has (half of
those calls) - no difference to the GPU or to the thread that makes them; tried again on
2026-10-11 for push constants and descriptors within a render pass (three quarters of the
constants' pushes, none of the descriptors', which change with every draw), in turns in one run
on the headset: the GPU's time the same within the noise (0.1 ms). The driver's early depth test
(LRZ) switched off while
the game runs (`TU_DEBUG_FILE`) - the GPU stopped for good within three minutes.

The app has the core log, every five seconds, the frame rate, what a frame consists of and how
long it takes on each part of its way ("frame path", `SHADPS4_FRAME_STATS=1`). The tools under
"Testing on the headset" show where GPU and processor time go.

### What the GPU thread does for a draw

One thread of the emulator turns the game's drawing into Vulkan commands. In the world select
(about 1770 draws a frame) it needed 28 ms of the processor a frame in the sandbox, 15.7
microseconds a draw; in 3-2 (1016 draws) 14.6 ms. A sampled profile of it
(`ASTRO_THREAD_SAMPLES`) put a fifth of that into finding the pipeline for each draw, which
meant working out again, for every shader of every draw, what kinds of resources it is given
and comparing that with the variants made so far; a sixth into finding the images behind its
textures; a quarter into the GPU driver itself.

Most draws give their shaders resources of the same kinds as some draw before them, and bind
textures that a draw before them bound. Both are now remembered: a shader's variant by every
word of its resources' descriptions except where they are (`Recognize` in
`vk_pipeline_cache.cpp`, checked word for word), the pipeline the draw before got, and a
texture's image by its whole descriptor while no image has come or gone
(`TextureCache::FindTextureImage`, and the view an image was last asked for). Run with
`SHADPS4_DBG_CHECK_RECOGNIZE=1`, the PC build also takes the long way at every shortcut and
compares: through 3-2 and through the first level, 0 differences in 50 million shader lookups
and 70 million texture lookups. In the sandbox, the shortcuts switched off and on every ten
seconds of one run (`SHADPS4_DBG_AB`): the world select 27.8 instead of 23.7 ms a frame for
the pipelines alone, then 22.4 instead of 20.2 ms for the textures on top; in 3-2 11.8 instead
of 14.6 ms a frame for both.

The driver's own share (`SHADPS4_DBG_CALL_TIMES=1`, the world select): 6 ms a frame, of which
2.1 ms pushing the descriptors of every draw, 1.6 ms the draw calls themselves and 0.6 ms
binding pipelines. Recording the commands on a second thread could take that off this one, but
the two would have to hand work to each other some 100 times a frame on the three cores a VR
session gives the emulator, where the game's own threads are as busy; not done. Of the small
buffers a draw reads (constants, short vertex and index lists), copied for every draw: about
7000 a frame in 3-2, 7 MB, four fifths of them copied before in the same frame. They are 1 KB
on average, and since the game may change one between two draws, reusing a copy means reading
the memory again to make sure; that was not worth it.

Where the processor limits the frame rate now is mostly the game's own work: in 3-2 it hands a
frame's drawing over within 19 ms of starting it, against 12 ms for the thread above.

**Drawing both eyes at once** (the GPU drawing every object for the two eyes in one go, which
the headset's GPU can do) was measured before anything was built for it. The second eye's
scene is 42% of a frame's draws in 3-2. Left out altogether (`SHADPS4_DBG_SKIP_SECOND_EYE=1`, a
measuring aid; that eye then shows what the first saw), the GPU's work a frame drops by roughly
40% (counted at its clock), the translating thread's time by 16% (the aid leaves out only the
last part of translating a draw; all of it would be up to 40%), and the game's own processor
time not at all (19.2 ms either way). Drawing both at once would save less than that on the
GPU: every pixel of the second eye still has to be shaded, only the draws themselves and part of
the geometry would be shared. And the game draws the eyes one after the other into the same
images, with its effects in between; the emulator would have to hold the first eye's drawing
back until the second's has come, pair them draw by draw, keep a second layer of every image
the eyes are drawn into, and give each eye its own constants inside the same shaders. That is
a large piece of work for what is, where the game's own processor time decides the frame rate,
little; not done.

Measured again on the night of 2026-10-09 in the first level (1600 draws a frame, the models
lighter, 816x870, the GPU's time counted at 690 MHz): the second eye's scene costs 5.6 ms of a
15.5 ms frame; drawn with one triangle a draw (`SHADPS4_DBG_SKIP_SECOND_EYE=2`) it still costs
1.9 ms, which is what its 590 draws cost as such (about 3 microseconds of the GPU each). Drawing
both eyes at once would save about that, and some of the second eye's vertices (it has 2.2
million): 2.5 to 3 ms, a sixth to a fifth of a frame in this level, more than in 3-2. Against
that, the first eye's drawing would wait for the second's: the GPU thread has the second eye's
first draw 4 to 6 ms after the first eye's in this level (`SHADPS4_DBG_STEREO_TIMING=1`), time
in which the GPU could then only work on the shadows. A build of it, with the pairing of the
draws, a second layer of every image the eyes are drawn into and the shaders' constants for
each eye, is weeks of work; still not done. With every draw cut to one triangle a frame of
the first level takes 6.6 of its 15.2 ms: what a draw costs as such, for all of them, is the
largest part of what is left.

### What a frame costs the headset's GPU

Measured on 2026-10-08 in level 3-2 (INDY CAVE 1), where a player's session in app 0.20 ran
at 20 to 30 frames a second at the smallest size (816x870 an eye), with the GPU at the 599 MHz
a VR session allows and busy 99% of the time. In the sandbox, at the same size, the GPU at its
690 MHz and the game held to 45 frames a second, a frame took 20.1 ms of the GPU's time. In a
session that is about 23 ms at 599 MHz, and the compositor needs its share on top: more than
the 22.2 ms that 45 frames a second leave, so the game drops to 30.

`SHADPS4_GPU_PROFILE=<seconds>` (`src/video_core/renderer_vulkan/vk_gpu_profile.cpp`) times
every draw and dispatch of one frame on the GPU, one by one, and lists what the time went to,
pass by pass, by target and by shader. A frame of 3-2 is about 1000 draws and 7.4 million
vertices: two shadow maps (2048x2048, 1.9 million vertices), then for each eye a depth pass
(200 draws, 1 million vertices) and the colour pass (220 draws, 1.7 million vertices), then a
small amount of post-processing. The colour passes are two thirds of the GPU's time, the
depth passes a seventh, the shadow maps a tenth, everything after them a few percent.
Drawn with a quarter of the pixels (`SHADPS4_DBG_VIEWPORT_SCALE=0.5`) the frame takes 13.4
instead of 20.1 ms: shading pixels is a bit less than half of it, the geometry and the draws
themselves the rest. Particles and other effects are not where the time goes.

The game has its own settings for what it draws, in its scene parameter files (`.spx`, XML;
`src/core/known_title_scenes.h`): at which distances models change to their simpler forms,
the size of the sun's shadow maps, whether depth is drawn on its own before the colours. The
emulator can hand the game changed copies of them (`SHADPS4_TITLE_LOD=<share>`,
`SHADPS4_TITLE_SHADOW_MAP=<1024|1536>`, `SHADPS4_TITLE_DEPTH_PREPASS=0`; the copies are made
from the game's own files at run time, in the emulator's cache folder). Measured in 3-2 as
above, GPU time a frame:

| Setting | GPU time | |
| --- | --- | --- |
| the game's own | 20.1 ms | 7.4 million vertices a frame |
| models simpler at half the distances | 17.9 ms | 4.7 million |
| models simpler at a third of the distances | 17.0 ms | 3.7 million; the start-up menu's "Lighter" |
| shadow maps 1024 instead of 2048 | 19.5 ms | the shadows' cost is their geometry, not their size |
| no depth pass | 22.5 ms | slower: without it the colour pass shades what is hidden |

In the first level and in 3-2, pictures taken with the models at a third of the distances
differ from the game's own in fewer than one pixel in a hundred, along edges. What the world
select draws does not change (its own file has every model in full up to 1000).

**Foveated rendering** (`foveation=`, the start-up menu's third line; `SHADPS4_VR_FOVEATION`,
`src/video_core/renderer_vulkan/vk_foveation.cpp`). The edges of each eye's scene, where the
lenses show less, are shaded in blocks of 2x2 pixels instead of pixel by pixel. Shapes and
their edges are drawn as before, every pixel with its own depth; only the colour inside a
block is worked out once for all four. The GPU does that by a small map of how coarsely to
shade each part (`VK_KHR_fragment_shading_rate`, which the headset's driver offers in the
direct mode the game is drawn in: one texel of the map for 8x8 pixels), given to the render
passes that draw the scene for an eye (those at the size the scene is drawn at, not the
picture handed to the headset). The map is centred on straight ahead, which the symmetric
projection the app asks for puts in the middle of the picture. In 3-2 as above, with the
models at a third of the distances:

| Level | Shaded in blocks | GPU time |
| --- | --- | --- |
| Off | nothing | 17.0 ms |
| Low | the outer half (by area: the corners are a lot of it) | 15.1 ms |
| Medium, the default | seven tenths, from 62% of the way out | 14.3 ms |
| High | four fifths, the far corners in blocks of 4x4 | 12.8 ms |
| Very High | nine tenths, the outer three fifths in blocks of 4x4 | about 5% less than High |

Together with the lighter models that is 14.3 instead of 20.1 ms a frame at the default. With
the emulated headset at 120 Hz and the emulator choosing pace and size, 3-2 in the sandbox
settles at 40 frames a second at 1200x1280 that way, against 30 at 960x1080 with the game's
own settings. A session has less of the GPU (599 MHz, and the compositor's share): 40 frames
a second at a smaller size is what to expect there. Not yet tried worn. Up close, the blocks
show on fine, bright texture (the glitter of a floor) in the outer part of the picture; at
High the far corners are visibly coarse, and at Very High most of the picture's outer part.
Very High was measured against High in the same run, the two taking turns every ten seconds
(`SHADPS4_DBG_AB_ONLY=fov`, 30 frames a second): in 3-2 at 960x1080, 11.6 instead of 12.3 ms
of the GPU a frame (5.5% less); in the first level at 816x870, 14.7 instead of 15.2 (3%). Not
much more: most of what a frame costs the GPU there is not the shading of the picture's
pixels (the depth pre-pass, the shadow maps, and what every draw costs whatever its size).

### The headset's own performance features

What Meta offers apps for speed, and what each can do for a game that is drawn by an emulator
(checked against Meta's documentation; the pages are in `build/docs/`):

- **Application SpaceWarp** has the app draw at half the display rate and the compositor make up
  every other frame. For that the app hands over, with every frame, a buffer that says for each
  pixel how it moved since the frame before, and the depth buffer; whatever is drawn without
  such motion vectors "appears to stutter". A game engine writes them in a pass of its own, from
  each object's transform in this frame and the last. This game has no such pass (among its
  render targets in the first level there is no velocity buffer), so the emulator would have to
  work out, for each of the game's 1800 draws a frame, where the same draw put its vertices a
  frame earlier, and draw all of it once more. That is a project of its own with an uncertain
  end, and the extra pass would cost frame rate that is already short of the exact half rate
  SpaceWarp insists on (60 for 120 Hz, 40 for 80 Hz, 36 for 72 Hz). Not done.
  What the compositor does do with every frame, at the display's rate, is turn it to where the
  head points by now. That is the same reprojection PlayStation VR used to show this game's 60
  frames at 120 Hz.
- **Fixed foveated rendering**, as the system offers it, makes the GPU draw the edges of the
  image an app renders into at a lower resolution. The image this app renders into only
  receives a copy of the finished picture, which is no work to speak of; the picture itself is
  drawn by the emulator into the game's own targets. So the emulator does it there, itself:
  see "Foveated rendering" under "What a frame costs the headset's GPU" (`foveation=`). (The
  system's way of doing it, a fragment density map, the driver only does in its tiled mode,
  which halves this game's frame rate; shading rates by a map, which it uses instead, work in
  the direct mode the game is drawn in.)
- **Super Resolution** is the better of the two filters the compositor can enlarge the picture
  with. Left to the system (the default up to 0.6) it was off two thirds of the time in a
  level; `sharpen=2` now asks for it unconditionally, the default is the emulator's own
  sharpening (see "The picture").
- **GPU and processor levels**: see above.

## The picture

In the third session the game was up to speed and the view was right, and the picture "looked
way worse" than on the console. It did, for four reasons, of which the size of the scene was
only one:

- **No anti-aliasing.** The console draws this game with multisampling (four colour samples
  and eight depth samples a pixel). On the Quest that is switched off (`msaa=1`): two samples
  cost 9 to 28% of a frame's time, four 28 to 40%, which is more than there is. Every edge was
  therefore as jagged as its pixels, and the headset's display, which has about two and a half
  pixels across for each of the picture's, shows every step. From 0.7 on the emulator smooths
  edges in the finished scene instead: where the game "resolves" a multisampled picture that
  the emulator holds with one sample a pixel, there is nothing to average, and the copy that
  used to stand in for it is now a filter that finds edges by their contrast and blends each
  pixel on one with its neighbour across it, by how far along the edge it sits (FXAA; it works
  on the scene before tone mapping, so a glaring pixel does not decide everything around it;
  `src/video_core/host_shaders/resolve_aa.frag`). It costs about 3% of a frame's time (1 ms at
  816x870) and is not tied to this game. `antialias=0` turns it off.
- **The size**, see above: 816x870 for half of the level.
- **Shown smaller.** Up to 0.6 the app showed the picture at the size the system recommended
  from moment to moment, 15% smaller in each direction whenever the GPU was busy, which in a
  level is always. That saved nothing worth having (the picture is copied once per frame of
  the game) and blurred what the game drew. Now the app still asks for the recommendation,
  which is what the system wants of an app before it lets the GPU run at its faster clocks,
  and shows the picture at full size. Whether the system still grants the faster clock (599
  MHz, "level 5") that way is the first thing to look for after the next session: `GPU=4/5`
  and `599MHz` in the `VrApi` lines that `tools/quest-recorder.sh` keeps.
  `dynamic_resolution=2` goes back to showing the picture at the recommended size.
- **No sharpening.** The headset was asked for its Super Resolution filter "when the system
  finds the GPU has time", and the system found that in a third of the level. Now the emulator
  sharpens each picture itself as it copies it to where the app takes it from (contrast
  adaptive sharpening; `post_process.frag`), which costs too little to measure and happens
  once per frame of the game instead of at every refresh. `sharpen=2` asks for Super
  Resolution on every refresh instead, whatever the GPU is doing (it takes about 0.7 ms of
  every refresh, a twentieth of the GPU at 90 Hz); `sharpen=0` leaves the picture soft.

`build/quest/picture-compare.png` shows three parts of the first level's picture as 0.6 showed
them, as 0.7 does in the heaviest views and in ordinary ones, and as a PlayStation 4 draws
them at its smallest and largest size (made from full-size dumps of the PC build by
`tools/picture-compare.py`).

How this compares with the console: a base PlayStation 4 shows this game at 60 frames a second,
960x1080 to 1440x1536, multisampled. The Quest 3 running it through an emulator shows the
levels at 30 frames a second, each frame for the same time, at 960x1080 to 1200x1280 in most
views and 816x870 in the heaviest, with edges smoothed by a filter instead of by multisampling;
lighter scenes at 45 frames a second and up to 1440x1536. Half the console's frame rate is what
the headset's GPU has in it for this game (a frame of the first level is 4 to 8 million
vertices in 750 to 2500 draws, 20 to 30 ms of GPU time at the smallest size); the picture is
now close to the console's at its smaller sizes.

**One surface as two targets of a draw.** The glow of the lava around the octopus of world 2
had squares in it, lit and dark, for as long as he moved. The game draws the rings that glow
into a quarter-size buffer which it has bound twice for those draws: as target 0, where the
colour is blended in, and as target 1, where alpha is written without blending. The console's
GPU writes both to the same memory. Vulkan leaves undefined what an image holds that was two
attachments of one render pass, and what it held was the draw in some blocks of 4x8 texels
and not in others. The emulator now makes such a draw once for every target that shares a
surface, with the others left out (`Rasterizer::SharedTargetPasses`). A scripted run gets to
the same octopus through the challenge "10 - Second Date" (with a save that has world 2
done): in the world select the challenge belt is chosen by looking 90 degrees to the right,
the challenge is three steps of the stick to the right, and the gadget chest at its start
takes the controller about 0.3 m below and 0.55 m ahead of the head;
`tools/pc-dump-test.sh` then keeps every render target of a frame of the fight.

## Sound

The game does all its sound through Sony's Ngs2 library, which the emulator only had as an empty
stub. `src/core/libraries/ngs2/ngs2_engine.cpp` implements what the game uses natively: sampler
voices (ATRAC9, HE-VAG, PCM), submixers, reverb, chorus/delay/pitch racks, mastering, and the
game's own effect callbacks. The game's 7.1 mix is rendered for the headset's ear speakers by a
virtual surround stage (`audio/surround_virtualizer.cpp`); `env=SHADPS4_VIRTUAL_SURROUND=0`
replaces it by a plain fold-down. The second mix the game produces for the television is not played.

The app plays each port on its own audio stream. The stream's buffer (60 ms) is what stands
between a game that is late for a moment and a gap in the sound: the emulator delivers as fast
as the stream takes it, and after a delay hands over what it missed at once, which fills the
buffer again. A stream that takes nothing at all (output suspended or rerouted) is not waited
for: its sound is dropped until it plays again, so the game never waits for sound nobody hears.
`host.log` has a line per stream every ten seconds: how much sound was delivered, dropped and
played, and how often the buffer ran dry. (In the second session the stream of the game's 3D
sound ran dry some 130 times in the ten seconds the first level took to load, and hardly ever
otherwise: the thread the game mixes it on, and the emulator's threads that pass sound on, had
no more claim to a processor than the game's helpers, which are busiest while a level loads.
They are now among the threads `SHADPS4_THREAD_NICE` puts first.)

(In the first session there was no sound at all: Android only starts to play a stream once its
buffer has been filled completely, and the app held back everything beyond a "cushion" that was
smaller than that.)

To check sound without listening: `SHADPS4_AUDIOOUT_DUMP=<seconds>` writes what every port plays
to WAV files in the emulator's user folder (`tools/wavstat.py <file> --png out.png` summarises one);
`SHADPS4_AUDIOOUT_STATS=1` logs every ten seconds how much of real time each port played and what
held it up.

## Microphone

The game listens to the microphone PlayStation VR has in its visor: blowing at things is one of
the ways to play with its world. The app passes the headset's own microphone on for that, and
asks for the permission the first time it starts; without it (or with `mic=0`) the game hears
silence, as it did before.

All the game does with the sound is measure how loud it is: 256 samples at a time, and from
their root mean square `rms` (1 being full scale) a strength of blowing
`min(1, 0.5 * ln(1 + 3.5 * 20 * log10(1 + rms / 1.5)))`. That is 1 from -9 dB on, a half at
-21 dB and a quarter at -30 dB. `host.log` says every ten seconds how loud the loudest moment was
on that scale, which is what to set `mic_gain` by should blowing do too little (or talking too
much). In an input script `mic=0.35` blows for as long as the line lasts, and
`SHADPS4_AUDIOIN_STATS=1` makes the emulator log what the game's port heard.

## Other firmware libraries

`src/core/libraries/json/json.cpp` implements the firmware's JSON library (libSceJson2), which the
emulator only stubbed. The game hands it the animation sequences of characters and sets of option
flags; with the stub every such text read as empty. `tools/tests/json_test.cpp` exercises it the
way the game calls it, and `SHADPS4_JSON_TRACE=<count>` logs what is parsed.

## PC test build

`tools/run-win.sh <seconds>` runs the game in the Windows build with a simulated headset. Useful
environment variables: `SHADPS4_VR_DEMO=1` (sweeps the head), `SHADPS4_INPUT_SCRIPT=<file>` (scripted
buttons, sticks, touchpad, head, controller and hand positions, a reset of the view, see
`shadps4-arm64-main/src/input/scripted_input.h`; examples in `build/dev/`). `tools/contact.mjs`
turns the automatic screenshots into a contact sheet.

`tools/pc-view-test.sh <name> <seconds> <"x,y,z"> [input script]` runs it with the head resting
at a given place and keeps pictures and log in `build/dev/view/<name>/`. For telling what the
game does with time and with the head, on a machine that is fast enough for anything:

| Setting | Result |
| --- | --- |
| `SHADPS4_DBG_FRAME_DELAY_MS=<ms>` | holds the game up by that much every frame (22 gives 43 frames a second, 32 gives 30) |
| `SHADPS4_SHOT_SECONDS=<n>[,<from>]` | a picture every n seconds by the clock, so that runs at different frame rates can be compared moment by moment |
| `SHADPS4_FRAME_STATS=1 SHADPS4_FRAME_STATS_EVERY=1` | frame rate and draws per frame every second: what the game shows over time, as a fingerprint of where it is |
| `SHADPS4_VR_REFRESH_RATE=<Hz>` | how often the emulated headset refreshes (60 to 120); the game draws half as many frames |
| `SHADPS4_VR_PACE=<2..6>` | how many of those refreshes every frame is given: 3 at 90 Hz makes 30 frames a second on the dot (without it the emulator chooses, which on a PC is always 2) |
| `SHADPS4_TITLE_FSR=<0..2>` | the known title's final pass reads its scene with FSR 1 EASU instead of bilinearly (its pixel shader is compiled with the filter in it): 1 where it is enlarged by 1.3 or more, 2 wherever it is enlarged (what the app asks for), 0 (the core's default) never. `SHADPS4_DBG_AB_ONLY=fsr` measures it |
| `SHADPS4_TITLE_FSR_HALF=0`, `SHADPS4_TITLE_FSR_FLAT=0` | the filter in 32 bits instead of 16; the filter also where the four nearest texels are all but the same. `SHADPS4_DBG_AB_ONLY=fsrhalf,fsrflat` measures them (several names may be given) |
| `SHADPS4_VR_SHARPEN_FSR=<0..1>` | how much the eyes' pictures are sharpened in the part FSR 1 made (default: half of `SHADPS4_VR_SHARPEN`) |
| `SHADPS4_VR_LARGEST_SIZE=<3..6>` | where the emulator chooses the scene's size, none larger than that (3 = 816x870 ... 6 = 1440x1536; the Quest app sets 5 unless `largest_resolution=` says otherwise) |
| `SHADPS4_VR_STEADY_FPS=<fps>` | frames are given as many refreshes as make that many frames a second or more, throughout, and only the scene's size changes with what the GPU has time for (the Quest app sets 30 unless `pace=` is set) |
| `SHADPS4_TITLE_TIMESTEP=0`, `SHADPS4_TITLE_RESOLUTION=<3..6 or title>` | the game's time step left alone; its scene held to one size (3 = 816x870 ... 6 = 1440x1536) or left to the game |
| `SHADPS4_TITLE_PHYSICS_STEP=0`, `SHADPS4_TITLE_PHYSICS_WATCH=1` | the game's physics step left as the console has it (collisions that are moved then land off, see "Speed"); a line in the log every ten seconds on how far the bodies the game sent somewhere ended from there |
| `SHADPS4_MAX_MSAA=<1, 2, 4>` | the most samples a pixel gets; 1 is what the headset runs with, and has the emulator smooth edges itself unless `SHADPS4_RESOLVE_AA=0` |
| `SHADPS4_VR_SHARPEN=<0..1>` | how much the eyes' pictures are sharpened on their way out (the app sets 0.6) |

`tools/pc-dump-test.sh <name> <seconds> [NAME=value ...]` plays into the first level and keeps
everything the game has drawn at that moment, at full size, in `build/dev/dump/<name>/` (the
eyes' finished pictures are the `..._523ce0000_1440x1536_...` and `..._5245e0000_...` files,
the scene before the game enlarges it the `960x1080_R16G16B16A16Sfloat` ones, or whatever size
it was held to). `tools/crop.py` cuts the same part out of several of them and puts the cuts
side by side, `tools/picture-compare.py` makes the comparison in `build/quest/picture-compare.png`,
and `tools/aa_proto.py` is where edge smoothing, enlarging and sharpening were tried out on
such dumps before any of it was written for the GPU.

`build/dev/input-gametime.txt` and `input-playcheck.txt` are the scripts the time step was
checked with, `input-origin2.txt` the one that shows when the game takes stock of the head.

## Testing on the headset without wearing it

`tools/quest-sandbox-test.sh <seconds> [input script]` runs the core inside the app's sandbox with
scripted input and pulls log, screenshots and audio dumps to `build/quest/run/`;
`tools/quest-selftest.sh <seconds>` exercises the app's own launch, socket and frame-sharing code.
Neither needs the headset to be unlocked; only the immersive session itself does.

Settings of `quest-sandbox-test.sh` for looking into speed (all described at the top of the script):

| Setting | Result |
| --- | --- |
| `SHADPS4_FRAME_STATS=1` (as an argument) | frame rate, passes, draws and the frame path in `run/core.log`; `=2` also lists the render passes of a frame and which code ended each |
| always | `run/gpu.txt`: GPU load, GPU and processor clocks every five seconds |
| `ASTRO_GPUTRACE_AT=<s>` | the driver's own trace from that second on; `tools/gputrace.py run/gputrace.txt <fps>` sums it up per kind of render pass |
| `ASTRO_THREADS_AT=<s> ASTRO_THREAD_SAMPLES="1500 shadPS4:GpuComm"` | a sampling profile of one thread; `node tools/symbolize-stacks.mjs --thread shadPS4:GpuComm --profile` prints it |
| `ASTRO_AT="<s>:<command>;..."` | shell commands at given times, e.g. to switch driver options through `TU_DEBUG_FILE` |
| `SHADPS4_GPU_PROFILE=<s>` (as an argument) | every that many seconds, one frame timed on the GPU draw by draw (that frame is slower): pass by pass, by target and by shader, with what the emulator itself had the GPU do between the draws (uploads, copies, barriers, where passes ended) and the longest waits, in `run/core.log` and `run/profile.txt`; also on the PC build |
| always | `run/pacing.txt`: frame rate, frame path and every change of pace and size, from the whole log (`run/core.log` keeps only its first 6 MB and its end) |
| `SHADPS4_DBG_AB=<s>` (as an argument) | the changes that ask for it (`FrameStats::AbOff`) are made and left out by turns, that many seconds each; every "frame stats" line says which (`[A]` made, `[B]` left out). `SHADPS4_DBG_AB_ONLY=pipelines`, `textures` or `eye2` limits it to one. Compares under the same heat and in the same scene, which runs after one another do not: the headset gets slower as it warms up |
| `SHADPS4_DBG_CALL_TIMES=1` (as an argument) | how long the calls into the GPU driver that every draw makes take, kind by kind, a line a frame-stats window |
| `SHADPS4_DBG_CHECK_RECOGNIZE=1` | at every shortcut of the pipeline and texture lookups the long way is taken too, and a difference is logged as an error |
| `SHADPS4_DBG_SKIP_SECOND_EYE=1` (as an argument) | the second eye's scene is not drawn (that eye shows what the first saw): what it costs. `=2` draws it with one triangle a draw: what is left is what its draws cost as such, which drawing both eyes at once would save. With `SHADPS4_DBG_AB` as `eye2` |
| `SHADPS4_DBG_STEREO_TIMING=1` | every ten seconds, how long after the first eye's first scene draw the second eye's first one reaches the GPU thread, and when the first eye's picture was done in between: what drawing both eyes at once would hold the first eye back by |
| `SHADPS4_DBG_AB_ONLY=<name>` | the changes that can be left out by turns: `ubo` (the game's constants as uniform buffers), `skipempty` (draws that write nothing left out), `size` (with `SHADPS4_TITLE_RESOLUTION`: the "B" frames a size larger), `ppcopy` (the eyes' copy without a clear and through UNORM views: only in the self-test, the sandbox has no copy), `resolveaa` (resolves copied as they are instead of smoothed), `fxaafine` (smoothed all over instead of in the fovea's fine middle), `onetri` (with `SHADPS4_DBG_DRAW_VERTICES`), `pipelines`, `textures`, `eye2`, `clearbarrier` (the barrier before a pass that clears a target: without it, black squares in the right eye after a change of size; costs nothing measurable) |
| `SHADPS4_DBG_NO_SHADOW_TAPS=1` | pixel shaders read every depth-compared sample (the taps that filter a shadow's edge) as fully lit: what those taps cost. No shadows |
| `SHADPS4_MAX_ANISO=<n>` | no more than n samples along a texture seen at a slant; the log names the ratios the game asks for (2x and 4x in the first level) |
| `SHADPS4_TITLE_PREPASS_DISTANCE=<m>`, `SHADPS4_TITLE_PREPASS_RADIUS=<m>` | how far away models still get the depth pre-pass, and how small they may be (the headset's base scene file sets neither; the television's has 10 and 0) |
| `SHADPS4_UBO=0` / `=1` | the game's constants as storage buffers / as uniform buffers (on by default with the headset's driver only) |
| `SHADPS4_DBG_EYE_SCAN=<n>` | every n-th frame both eyes' pictures (and, in a session or the self-test, the picture handed to the app) are read back and looked over for black blocks in one eye only, or that were lit at the look before: each is logged ("Eye scan at ...") and pictures of such moments go to `eye_scan/` in the core's user folder, one set a minute to compare with. Works in a real session too, through `env=` in `vrhost.txt` |
| `SHADPS4_DBG_TRACE_HMD_FRAMES=<frame>,...` / `SHADPS4_DBG_DUMP_HMD_FRAMES=<frame>,...` | the draws after those frames in the log (targets, depth and stencil state, clears, copies) / every render target as a picture in `rt_dump-<frame>/` |
| `SHADPS4_DBG_TRACE_SIZE_CHANGE=<s>[,<frames>]` | with `SHADPS4_DBG_RESOLUTION_CYCLE`: the draws from so many frames after the first change of size that many seconds in |
| `SHADPS4_DBG_TRACE_IMAGES=1` | every image the texture cache takes on or lets go of, and every request answered with an image of another size |

The sandbox is not a VR session: the core may only use three processor cores there, and the GPU
clock follows the load instead of being held by the VR runtime.

Settings of `quest-selftest.sh` that make it more like one (given like the core's, as
`NAME=value` arguments):

| Setting | Result |
| --- | --- |
| `HOST_COMPOSITOR=<Hz>[,<n>[,<Mpx>]]` | a stand-in for the compositor: so many times a second a pass the size of the compositor's own (9 megapixels) and n - 1 small ones (n = 5), on a GPU context that comes before the emulator's. With it the GPU is the limit as in a session |
| `HOST_DISPLAY=<Hz>` | the test's loop goes round once per refresh of a display of that rate, tells the emulator of each as the app does, and counts for how many refreshes each frame of the game stayed the newest |
| `HOST_LOOK="<s>:<left>,<up>;..."` | where the head looks from which second on (degrees); `8:85,22;110:0,-12` finds the first level from the world map and then looks along it |
| `HOST_PAD="<s>:<kind>,<right>,<up>,<ahead>,<left>,<tilt>,<roll>;..."` | what the app says of the controller from which second on, the way a session does: `c` one of the headset's own controllers (it says everything of itself), `h` hands around a gamepad without motion sensors, `p` hands around one that has them (only where it is), `n` nothing seen; where it is in metres from the head, how it is turned in degrees. With `HOST_LOOK=0:0,-18` the controller is in the picture |
| `HOST_TURN="<s>:<steps>;..."` | the view is turned by so many steps to the right at that second (to the left if negative), as the left grip held and the right stick flicked ask in a session. With `HOST_LOOK=0:0,0` the pictures before and after show the same scene from two sides |
| `HOST_SHOTS=<s>` | a picture every so many seconds instead of every ten |
| `XDG_DATA_HOME=<folder>` | where the emulator keeps its data, saves among them. The self-test starts from nothing each time, so the game plays its prologue (four minutes); with a folder that holds a save it is in the first level after 75 seconds |
| `SHADPS4_VR_FOLLOW_DISPLAY=0` | the emulated headset keeps to its own clock although the host tells of its display's refreshes |

`quest-selftest.sh` gives the game's microphone a test signal (a second of noise every five);
`ASTRO_MIC=real` opens the headset's own instead, which stays silent while nobody wears it.
`ASTRO_FRESH=1` makes it unpack the runtime from the installed app first, as the app does on its
first start, instead of testing the core in `build/arm64`. (Both test scripts otherwise put that
build into the app's runtime folder and remove the folder's stamp, so that the app unpacks its
own again the next time it is started.)

`tools/quest-level-bench.sh <name> <seconds>` is the self-test set up for measuring: a display
and a compositor stand-in of 90 Hz (`BENCH_HZ=72` or `120` for others), a save that has the game
in the first level after 75 seconds (the folder `bench-data` in the app's files, a copy of the
sandbox tests' save), the head looking at the level sideways until second 110 and down its
whole length after that, which is its heaviest view. It keeps report, logs and the GPU's clock
in `build/quest/bench/<name>.*` and prints what `tools/quest-bench-read.sh <name>` prints:
frames a second by ten seconds, for how many refreshes frames were shown, every decision on
pace and size, and every ten seconds what frames took at what size with the GPU how busy.
(`tools/quest-gpu-watch.sh <seconds> <file>` on its own writes GPU load, clock and temperature
once a second.) The self-test does not take one of its own settings twice: give `HOST_DISPLAY`
and `HOST_COMPOSITOR` once.

`tools/quest-xrprobe.sh` goes as far into OpenXR as the runtime lets an app go with nobody
wearing the headset: it checks that the app finds the runtime and that the extensions the host
uses are there, and then runs the host itself for five seconds - session, swapchains, hand
trackers and the frame buffers shared with the emulator are created as in a real start, the
session (which stays idle) is asked for the refresh rates on offer, the performance levels and
the thread hints, and a test picture is copied into the swapchain's images and read back. On
Horizon OS 207 all of that succeeds (72, 80, 90 and 120 Hz on offer). What it cannot reach is the
session itself: frames on the display, head and hand tracking, the controller.

`tools/quest-drystart.sh <seconds> [settings file]` starts the app's own activity without showing
it: everything the app does when it is launched runs (settings, log files, OpenXR up to the idle
session, unpacking the runtime, the emulator with the game, sound), a controller that is not
there presses ✕ now and then (through the same code a real one's events go through), pulls
its right stick back once and presses R1 once, and the `host.log` and `core.log` the app
writes are pulled to `build/quest/drystart/` together with pictures of what its status panel
showed, of the start-up menu and of the controls cards.

`SHADPS4_PAD_TOUCH_TRACE=1` (as `env=` in a settings file for the dry start, or as a setting of
`quest-sandbox-test.sh`, whose input scripts can press the gesture buttons with
`gesture=press`, `gesture=swipe` and `gesture=pull` and move the stick's finger with
`finger=x,y`) has core.log say what the game was given of the touchpad at each of its reads,
for as long as a finger was on it: where the game saw a touch begin and end, and over how
many reads, is what it makes its swipes and pulls of.

`quest-host/build.sh --test-package` builds the same app under another name
(`com.astrobotquest.vrhost.test`, "Astro VR Host (test)", `build/quest/astro-vr-host-test.apk`).
It installs next to the app proper and touches neither it nor its saves: for trying a build on
a headset whose owner plays with the other one. `ASTRO_PACKAGE=com.astrobotquest.vrhost.test`
before `quest-xrprobe.sh`, `quest-drystart.sh`, `quest-selftest.sh` or `quest-sandbox-test.sh`
runs them in it; `adb uninstall com.astrobotquest.vrhost.test` removes it again.

## Watching a real session

A session is played without a cable, so what the app cannot see itself is written down on the
headset: `tools/quest-recorder.sh start` leaves a small recorder running there (until the
headset is restarted; starting it again is harmless). Whenever the emulator runs it keeps, in
`/data/local/tmp/astro/sessions/<start time>/`, the VR runtime's own statistics (`VrApi` lines
once a second: frames shown and stale, processor and GPU level and clock, GPU time of the app
and of the compositor; `crcs` lines: every change of clock level and the reason for it), GPU
load, clock and temperature every two seconds, and the emulator's busiest threads with the cores
they ran on every thirty.

`tools/quest-recorder.sh pull`, with the headset back on the cable, fetches all of it to
`build/quest/sessions/`, with the app's `host.log` and `core.log` next to the newest session.
`status` says whether the recorder runs and what it has.

What to read first after a session with 0.7: in `core.log`, "Frames are given ... refreshes"
and "The scene is drawn at" (how often pace and size changed, and between what), and the
ten-second lines "The title's clock" (frames at 33.3 ms with the GPU under 90% busy is the level
running as intended; frames longer than they are given with the GPU at 100% is the GPU short).
In the recorder's `VrApi` lines, `GPU=` and the clock after it (level 5, 599 MHz, is what the
app had so far while it showed the picture at the recommended size; it now only asks for that
size), `FPS=` against the display's 90, and `Stale`. In `host.log`, the "controller:" lines
(vendor 1356 is Sony) and "picture shown at" (1440x1536, and what the system recommended).

(`tools/quest-session-watch.sh` does the same from the PC, for a headset that stays connected.)

Should the game stop drawing, the app's panel says so after twenty seconds ("No new picture
from the game for ... seconds") instead of "Loading...", and the emulator writes what each of
its threads was doing into `core.log`; `node tools/symbolize-stacks.mjs <core.log>` turns that
into function names. The one such stop met so far is described in
`shadps4-arm64-main/src/video_core/renderer_vulkan/vk_master_semaphore.cpp`.
