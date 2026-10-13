// SPDX-License-Identifier: GPL-2.0-or-later
package com.astrobotquest.vrhost;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.hardware.input.InputManager;
import android.hardware.lights.Light;
import android.hardware.lights.LightState;
import android.hardware.lights.LightsManager;
import android.hardware.lights.LightsRequest;
import android.os.Build;
import android.os.Bundle;
import android.os.CombinedVibration;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.os.VibrationEffect;
import android.os.VibratorManager;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * Hosts the PS4 emulator core on a Meta Quest: installs its Linux runtime, starts it, feeds it a
 * gamepad paired with the headset as the DualShock 4 the game expects (of a DualSense
 * everything: buttons, sticks, touchpad, motion sensors in; rumble and light bar colour out;
 * of other gamepads what they have) and lets the native side show its frames in the headset.
 * The headset's own controllers play as well; those the native side reads itself, and it
 * decides which of the two the game is played with (PadRouter).
 *
 * Files the user provides live in the app's external folder
 * (/sdcard/Android/data/com.astrobotquest.vrhost/files):
 *   games/SERIAL/eboot.bin ...   the extracted game (or /data/local/tmp/astro/games/SERIAL)
 *   data/shadPS4/                 the emulator's own folder (sys_modules, saves, logs)
 *   vrhost.txt                    optional settings, see readSettings
 * and what the app leaves there for looking at afterwards:
 *   host.log, core.log            this app's log and the emulator's output of the last start
 *   host.prev.log, core.prev.log  the same of the start before
 */
public class MainActivity extends Activity
        implements InputManager.InputDeviceListener, SensorEventListener {
    private static final String TAG = "AstroVR";

    // PS4 pad button bits (ORBIS_PAD_BUTTON_*).
    private static final int L3 = 0x2, R3 = 0x4, OPTIONS = 0x8;
    private static final int UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80;
    private static final int L2 = 0x100, R2 = 0x200, L1 = 0x400, R1 = 0x800;
    private static final int TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000, SQUARE = 0x8000;
    private static final int TOUCHPAD = 0x100000;

    private static final int STATUS_WIDTH = 1024, STATUS_HEIGHT = 512;

    /** Second place games are looked for; every app may read what adb puts there. */
    private static final String SHELL_GAMES = "/data/local/tmp/astro/games";

    // Mirrors CoreProcess::State.
    private static final int CORE_IDLE = 0, CORE_STARTING = 1, CORE_RUNNING = 2, CORE_STOPPED = 3,
            CORE_FAILED = 4;

    /** How long a rumble command lasts if the game says nothing more, and how often it is renewed. */
    private static final int RUMBLE_MILLIS = 1000, RUMBLE_RENEW_MILLIS = 400;

    private static final int REQUEST_MICROPHONE = 1;

    static {
        System.loadLibrary("astrovr");
    }

    private native void nativeStartXr(float refreshRate, int eyeWidth, int eyeHeight,
            boolean trackHands, int sharpen, boolean cubic, boolean showStats, float predictMs,
            int dynamicResolution, boolean cpuBoost, boolean ownControllers, int padHand,
            float padTilt, boolean rumble, boolean stickTouchpad);

    private native boolean nativeStartCore(String loader, String runtimeRoot, String storageRoot,
            String game, String logFile, String[] extraArgs, String[] extraEnv);

    private native void nativeStop();

    private native int nativeCoreState();

    private native String nativeCoreMessage();

    private native long nativeFrameCount();

    private native void nativeSetStatusImage(ByteBuffer pixels);

    /**
     * What the gamepad says: sticks from -1 to 1, triggers from 0 to 1, the finger on its own
     * touchpad from 0 to 1 across and from its far edge.
     */
    private native void nativeInput(int buttons, float leftX, float leftY, float rightX,
            float rightY, float leftTrigger, float rightTrigger, boolean touchDown, float touchX,
            float touchY);

    private native void nativeMotion(float gx, float gy, float gz, float ax, float ay, float az);

    private native long nativePadFeedback();

    /**
     * A gamepad is there to play with (again, or another one), or none is any more.
     * touchpad: it has a touchpad of its own, and needs no buttons to stand in for one.
     */
    private native void nativeSetGamepad(boolean connected, boolean touchpad);

    /** How often OPTIONS was pressed in the game so far, on whichever controller. */
    private native int nativeOptionsPresses();

    /** Shows the panel over the game's picture, for what is to be read while playing. */
    private native void nativeShowPanel(boolean show);

    /** The start-up menu is over: what the player presses is the game's. */
    private native void nativeSetPlaying(boolean playing);

    /** What the game is played with, see the CONTROLLER_ constants. */
    private native int nativeController();

    /** The player chose with it in the start-up menu. */
    private native void nativeUseController(int which);

    /** What the headset's own controllers say to the start-up menu, see the MENU_ constants. */
    private native int nativeTouchMenu();

    private static final int CONTROLLER_WHICH = 3, CONTROLLER_GAMEPAD = 1, CONTROLLER_OWN = 2;
    /** Added to it while the headset's own controllers are in the player's hands. */
    private static final int CONTROLLER_OWN_HELD = 4;
    /** Added where what the game is played with has no touchpad: buttons do its gestures. */
    private static final int CONTROLLER_GESTURE_BUTTONS = 8;
    private static final int MENU_LEFT = 1, MENU_RIGHT = 2, MENU_CONFIRM = 4, MENU_UP = 8,
            MENU_DOWN = 16;

    private static final int VENDOR_SONY = 0x054c;
    private static final int VENDOR_META = 0x2833;

    /** What a reset of the view covers: the controller's heading, the player's seat. */
    private static final int RECENTER_PAD = 1, RECENTER_SEAT = 2;

    private native void nativeRecenter(int what);

    private native int nativeXrStatus();

    private native void nativeSetMicrophone(boolean enabled, float gain);

    private native void nativeSetFieldOfView(int percent);
    /** Has the display switch to that refresh rate now (the session runs while the menu is up). */
    private native void nativeSetRefreshRate(float rate);
    /** The refresh rates the display offers: bit 0 72 Hz, 1 80, 2 90, 3 120; 0 until known. */
    private native int nativeRefreshRates();

    /** Whether the display offers that rate (true while that is not known yet). */
    private boolean offersRate(int rate) {
        int offered = nativeRefreshRates();
        int bit = rate == 72 ? 1 : rate == 80 ? 2 : rate == 90 ? 4 : rate == 120 ? 8 : 0;
        return offered == 0 || (offered & bit) != 0;
    }

    private native float[] nativeHeadsetFov();

    private static native void nativeSetLogFile(String path);

    private static native void nativeLog(int priority, String message);

    private final Handler handler = new Handler(Looper.getMainLooper());
    private View inputView;
    private volatile String setupStatus = "Starting...";
    private volatile boolean setupFailed;
    private String shownStatus = "";

    // Controller state.
    private int keyButtons;
    private int hatButtons;
    private int triggerButtons;
    private float leftX, leftY, rightX, rightY, leftTrigger, rightTrigger;
    private boolean padTouchDown;
    private float padTouchX, padTouchY;
    private boolean padTouchClick;
    private final float[] acceleration = {0.0f, 9.81f, 0.0f};
    /** Whose sticks and triggers the fields below describe. */
    private int axesDevice = -1;
    private int rightXAxis = MotionEvent.AXIS_Z, rightYAxis = MotionEvent.AXIS_RZ;
    /** The gamepad says how far its triggers are pulled, and not only that they are. */
    private boolean analogTriggers = true;

    // The controller itself: what the game's feedback goes to.
    private InputDevice gamepad;
    private SensorManager controllerSensors;
    private VibratorManager controllerVibrators;
    private LightsManager.LightsSession lightSession;
    private Light lightBar;
    private long shownFeedback = -1;
    private long rumbleRenewAt;

    // Settings.
    private boolean stickTouchpad = true;
    private boolean motion = true;
    private boolean rumble = true;
    private boolean trackHands = true;
    private boolean ownControllers = true;
    private int padHand = 1;
    private float padTilt;
    // The language the game is played in: the headset's own, or the one the settings name.
    private String language = "";
    private int sharpen = 1;
    private boolean cubic;
    private boolean antialias = true;
    private int pace;
    private int msaa = 1;
    private boolean showStats;
    private float predictMs = 25;
    private boolean microphone = true;
    private int dynamicResolution = 1;
    private boolean cpuBoost = true;
    /**
     * How much of the user's headset projection the game draws, in percent: less than all
     * of it for more pixels to the degree, which is what the player asked for.
     */
    private int fieldOfView = 85;
    private boolean choosingFov;
    private int fovConfirmKey = KeyEvent.KEYCODE_UNKNOWN;
    private long nextFovMoveAt;
    private float[] headsetFov;
    private static final int FOV_MIN = 50, FOV_MAX = 100, FOV_STEP = 5;
    private long nextOwnFovMoveAt;
    /**
     * How much the game draws of what is further away: DETAIL_CONSOLE all of it, as on the
     * console; DETAIL_LIGHTER has its models change to their simpler forms at a third of the
     * distances the game has for that (SHADPS4_TITLE_LOD), and the sun's shadows drawn into
     * maps of 1024x1024 instead of 2048x2048 (SHADPS4_TITLE_SHADOW_MAP). The headset's GPU
     * then has noticeably less to do for a frame (a sixth less in level 3-2), and the
     * difference is hard to see from the player's seat: shadows' edges are a little softer.
     */
    private int detail = DETAIL_LIGHTER;
    private static final int DETAIL_CONSOLE = 0, DETAIL_LIGHTER = 1;
    /**
     * How coarsely the edges of the picture are shaded (SHADPS4_VR_FOVEATION), where the lenses
     * show less of it anyway: 0 every pixel on its own, 1 to 4 more and more of the edges in
     * blocks of 2x2 pixels (at 3 the far edges in 4x4, at 4 the outer part). Meta's own
     * names for it are used, and one beyond them.
     */
    private int foveation = FOVEATION_DEFAULT;
    private static final int FOVEATION_DEFAULT = 2, FOVEATION_MAX = 4;
    /**
     * How the game's picture is enlarged to the one it hands to the headset where it draws it
     * smaller (SHADPS4_TITLE_FSR): with AMD's FSR 1 in the middle of the view (the default),
     * which keeps edges crisper, or as the game does it itself (bilinear). FSR 1 runs inside the
     * game's own final pass and takes the GPU well under a millisecond a frame.
     */
    private boolean upscaleFsr = true;
    private static final String[] FOVEATION_NAMES = {"Off", "Low", "Medium", "High", "Very High"};
    /**
     * The line of the start-up menu that left and right change: view, detail, foveation,
     * enlargement, refresh rate.
     */
    private int menuRow;
    private static final int MENU_ROWS = 5;
    private long nextRowMoveAt;
    private long nextOwnRowMoveAt;
    /** The card that says which buttons do the touchpad's gestures: shown until then. */
    private long cardUntil;
    private boolean cardShowing;
    /** The presses of OPTIONS counted at the last look. */
    private int optionsSeen;
    private static final long CARD_ON_PAUSE_MS = 10000;
    /**
     * What confirms the menu on the headset's own controllers has been seen let go while the
     * menu showed: the trigger that started the app from the headset's library is not it.
     */
    private boolean ownConfirmArmed;
    /**
     * Set by the test that runs the activity without showing it (SandboxShell "drystart"):
     * nothing is asked of the user or of the system's window manager then.
     */
    private boolean dryRun;
    private int statusPictures;
    private float microphoneGain = 1.0f;
    private long quietFrames = -1;
    private long quietSince;
    private long statsFrames = -1;
    private long statsTime;
    private String statsRate = "...";
    /**
     * What the display is asked to refresh at, a line of the start-up menu. The game draws 30
     * frames a second either way: at 90 Hz (the default) the headset's compositor makes three
     * pictures of each, at 120 Hz four, which follow the head more smoothly but take the GPU a
     * little more from the game. vrhost.txt's refresh_rate seeds it.
     */
    private float refreshRate = REFRESH_DEFAULT;
    private static final int REFRESH_DEFAULT = 90, REFRESH_FASTER = 120;
    /** What SHADPS4_TITLE_RESOLUTION is set to, null to leave the choice to the emulator. */
    private String resolution;
    /**
     * The largest size the emulator chooses (SHADPS4_VR_LARGEST_SIZE): 1200x1280. The first
     * level leaves the GPU room for it most of the time; 1440x1536 fits only in light scenes,
     * and a heavy one that comes after them would be drawn too large for a second or so.
     */
    private String largestResolution = "5";
    private boolean realTime = true;
    private final List<String> extraArgs = new ArrayList<>();
    private final List<String> extraEnv = new ArrayList<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        dryRun = getIntent() != null && getIntent().getBooleanExtra("dry_run", false);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // Nothing is drawn here, the view only exists to receive controller events.
        inputView = new View(this);
        inputView.setFocusable(true);
        inputView.setFocusableInTouchMode(true);
        inputView.setOnCapturedPointerListener((view, event) -> handleTouchpad(event));
        setContentView(inputView);
        inputView.requestFocus();

        // Kept as files: by the time somebody asks what happened in a session, the system's
        // log has long moved on.
        File external = getExternalFilesDir(null);
        keepPrevious(new File(external, "host.log"), new File(external, "host.prev.log"));
        nativeSetLogFile(new File(external, "host.log").getAbsolutePath());
        logInfo("Astro VR Host " + versionName() + " on " + Build.MODEL + ", system "
                + Build.DISPLAY);

        // vrhost.txt takes precedence over the last choice, and seeds the menu each launch.
        if (!dryRun) {
            fieldOfView = Math.max(FOV_MIN, Math.min(FOV_MAX,
                    getPreferences(MODE_PRIVATE).getInt("fov", fieldOfView)));
            detail = getPreferences(MODE_PRIVATE).getInt("detail", detail) == DETAIL_CONSOLE
                    ? DETAIL_CONSOLE : DETAIL_LIGHTER;
            foveation = Math.max(0, Math.min(FOVEATION_MAX,
                    getPreferences(MODE_PRIVATE).getInt("foveation", foveation)));
            upscaleFsr = getPreferences(MODE_PRIVATE).getBoolean("fsr", upscaleFsr);
            refreshRate = getPreferences(MODE_PRIVATE).getInt("refresh", REFRESH_DEFAULT)
                    == REFRESH_FASTER ? REFRESH_FASTER : REFRESH_DEFAULT;
        }
        readSettings();
        choosingFov = !dryRun;
        nativeStartXr(refreshRate, 1440, 1536, trackHands, sharpen, cubic, showStats, predictMs,
                dynamicResolution, cpuBoost, ownControllers, padHand, padTilt, rumble,
                stickTouchpad);

        InputManager inputManager = getSystemService(InputManager.class);
        inputManager.registerInputDeviceListener(this, handler);
        findGamepad();

        if (dryRun) {
            // Nobody sees the menu in a test: it is drawn for the pictures the test keeps,
            // once the way it waits for the headset and once with a headset's view.
            drawFovMenu();
            headsetFov = new float[] {1.376f, 0.839f, 0.966f, 1.428f};
            drawFovMenu();
            headsetFov = null;
            // Likewise the cards that say which buttons do the touchpad's gestures.
            drawStatus("The touchpad, on this controller", controlsCard(true));
            drawStatus("The touchpad, on this controller", controlsCard(false));
            startGame();
        } else {
            handler.post(this::pollOwnControllers);
        }
        handler.post(this::refreshStatus);
        handler.post(this::applyFeedback);
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        getSystemService(InputManager.class).unregisterInputDeviceListener(this);
        detachGamepad();
        nativeStop();
        super.onDestroy();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
            int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_MICROPHONE) {
            // Nothing else to do: the native side keeps trying to open the microphone.
            logInfo("microphone " + (grantResults.length > 0
                    && grantResults[0] == PackageManager.PERMISSION_GRANTED
                            ? "allowed" : "not allowed"));
        }
    }

    /** Called by the native side when the headset session is over and will not come back. */
    private void onSessionEnded() {
        logInfo("the session is over: closing");
        if (!dryRun) {
            runOnUiThread(this::finish);
        }
    }

    private String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException e) {
            return "?";
        }
    }

    private static void keepPrevious(File current, File previous) {
        if (current.isFile()) {
            previous.delete();
            current.renameTo(previous);
        }
    }

    private static void logInfo(String message) {
        nativeLog(Log.INFO, message);
    }

    private static void logWarning(String message, Throwable cause) {
        nativeLog(Log.WARN, cause == null ? message : message + ": " + cause);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        logInfo(hasFocus ? "input focus gained" : "input focus lost");
        if (hasFocus && !choosingFov) {
            // Gives the controller's touchpad to the game instead of a mouse pointer.
            inputView.requestPointerCapture();
        }
    }

    // --- settings -------------------------------------------------------------------------------

    /**
     * Reads the optional vrhost.txt: one "key=value" per line.
     *   refresh_rate=90        display refresh rate to ask for (72, 80, 90, 120). The game
     *                          draws 30 frames a second at 90 Hz (the default) and at 120,
     *                          every frame shown for three or four refreshes; 36 at 72 Hz and
     *                          40 at 80. Seeds the startup menu
     *   pace=2                 leave it to the game how many refreshes a frame takes, as up to
     *                          version 0.6: it takes two, and one more whenever a frame is
     *                          late, so frames last unevenly where it only just keeps up. A
     *                          larger number holds every frame to that many refreshes. Without
     *                          this the game draws 30 frames a second throughout and the
     *                          emulator chooses the size of the picture, see resolution
     *   resolution=960         the size the game draws its scene at, per eye: 816 (x870), 960
     *                          (x1080), 1200 (x1280) or 1440 (x1536), or "game" for the game's
     *                          own choice. Without this the emulator chooses, by how busy the
     *                          GPU is: the largest size that fits a frame's time with room to
     *                          spare, up to largest_resolution
     *   largest_resolution=1440  the largest size the emulator chooses: 816, 960, 1200 (the
     *                          default) or 1440. A larger one makes the picture sharper where
     *                          a scene is light, and changes of size (each a few late frames)
     *                          more frequent; 960 changes it least
     *   antialias=0            leave edges as jagged as the game draws them here. The console
     *                          multisamples (see msaa), which this GPU cannot afford; by
     *                          default the emulator smooths edges in the finished scene
     *                          instead, for a fraction of the cost
     *   real_time=0            let the game count time in frames, as it does on the console:
     *                          it then runs in slow motion whenever a frame takes longer than
     *                          a sixtieth of a second, which here is always. By default the
     *                          emulator has it run by the clock
     *   hands=0                do not use hand tracking to place the controller; it is then held
     *                          at a fixed spot in front of the player
     *   own_controllers=0      never play with the headset's own controllers, whoever holds them
     *   pad_hand=left          with those, the left one is the controller in the game instead of
     *                          the right one
     *   pad_tilt=-15           for a gamepad without motion sensors, which the hands holding it
     *                          turn and tilt in the game: how many degrees higher (lower, if
     *                          negative) its front points than the hands do, -60 to 60
     *   sharpen=0              leave the picture as soft as it gets from being shown larger
     *                          than it was drawn. By default (1) the emulator sharpens it once
     *                          per frame of the game. 2 has the headset do it instead, with its
     *                          Super Resolution filter on every refresh: finer, but it takes
     *                          about a twentieth of the GPU from the game. 3 is the same left
     *                          to the system, which drops it when the GPU is busy (always, in
     *                          a level). 4 is the headset's plainer filter
     *   cubic=1                have the headset enlarge the picture with a cubic filter instead
     *                          of a linear one: sharper, for some GPU time on every refresh
     *   stick_touchpad=0       stop using the right stick as a finger on the touchpad
     *   motion=0               ignore the controller's motion sensors (the hands holding it
     *                          then say how it is turned, as for a gamepad that has none)
     *   rumble=0               no vibration
     *   msaa=4                 let the game multisample as it does on the console (1, 2 or 4).
     *                          The default of 1 turns that off: it is by far the most expensive
     *                          thing the headset's GPU is asked to do
     *   stats=1                keep a small panel in view with the game's frame rate, the
     *                          display's refresh rate and whether the hands are seen
     *   predict_ms=25          how far beyond the next refresh the head pose given to the game
     *                          is predicted (0 to 80). More if the edges of the picture show
     *                          when turning the head, less if the world wobbles
     *   dynamic_resolution=0   do not ask the system what size it recommends for the
     *                          picture. Asking is what the system wants of an app before it
     *                          lets the GPU run at its faster clocks; the picture is shown at
     *                          full size all the same (the game draws its scene smaller when
     *                          the GPU is short, which is what saves time). 2 also shows the
     *                          picture at the recommended size, as up to version 0.6
     *   cpu_boost=0            do not ask for the processor's fastest clock, which the system
     *                          otherwise grants for the first 45 seconds and now and then
     *   fov=100                have the game draw that many percent of this headset's full
     *                          projection: 100 fills its field of view. Showing the same
     *                          pixels over fewer degrees is a sharper
     *                          picture, with a soft squircle border below 100. Default 85;
     *                          100 is the headset's own, 50 to 100. Seeds the startup menu
     *   detail=console         the game draws all it draws on the console further away;
     *                          lighter (the default) has its models change to their simpler
     *                          forms at a third of the game's distances and the sun's shadows
     *                          drawn at half the resolution, which takes the GPU less time.
     *                          Seeds the startup menu
     *   foveation=0            every pixel of the picture shaded on its own; 1, 2 (the
     *                          default), 3 and 4 shade more and more of its edges in blocks of
     *                          2x2 pixels (3: the far edges in 4x4, 4: the outer part in 4x4),
     *                          where the lenses show less, which takes the GPU less time. Seeds
     *                          the startup menu
     *   fsr=0                  the game enlarges its picture itself (bilinear), as on the
     *                          console; by default FSR 1 does it in the middle of the view,
     *                          wherever the game draws it smaller than 1440x1536 (crisper
     *                          edges, well under a millisecond of GPU time a frame). Seeds the
     *                          startup menu
     *   mic=0                  the game does not get to hear the headset's microphone
     *   mic_gain=2             make what it hears that many times louder (0.1 to 30), if
     *                          blowing does too little or everything counts as blowing
     *   arg=...                extra emulator argument (may repeat)
     *   env=NAME=value         extra emulator environment variable (may repeat)
     */
    private void readSettings() {
        File file = new File(getExternalFilesDir(null), "vrhost.txt");
        // The test that starts the activity without showing it may bring settings of its own.
        String testSettings = dryRun ? getIntent().getStringExtra("settings") : null;
        if (testSettings != null) {
            file = new File(testSettings);
        }
        if (!file.isFile()) {
            return;
        }
        try {
            for (String line : Files.readAllLines(file.toPath(), StandardCharsets.UTF_8)) {
                line = line.trim();
                int equals = line.indexOf('=');
                if (line.isEmpty() || line.startsWith("#") || equals <= 0) {
                    continue;
                }
                String key = line.substring(0, equals).trim();
                String value = line.substring(equals + 1).trim();
                switch (key) {
                    case "refresh_rate":
                        refreshRate = Float.parseFloat(value);
                        if (refreshRate <= 0) {
                            refreshRate = REFRESH_DEFAULT;
                        }
                        break;
                    case "resolution":
                        switch (value) {
                            case "816":
                                resolution = "3";
                                break;
                            case "960":
                                resolution = "4";
                                break;
                            case "1200":
                                resolution = "5";
                                break;
                            case "1440":
                                resolution = "6";
                                break;
                            case "game":
                                resolution = "title";
                                break;
                            default:
                                resolution = null;
                                break;
                        }
                        break;
                    case "largest_resolution":
                        switch (value) {
                            case "816":
                                largestResolution = "3";
                                break;
                            case "960":
                                largestResolution = "4";
                                break;
                            case "1200":
                                largestResolution = "5";
                                break;
                            case "1440":
                                largestResolution = "6";
                                break;
                            default:
                                break;
                        }
                        break;
                    case "real_time":
                        realTime = !value.equals("0");
                        break;
                    case "hands":
                        trackHands = !value.equals("0");
                        break;
                    case "own_controllers":
                        ownControllers = !value.equals("0");
                        break;
                    case "pad_hand":
                        padHand = value.equalsIgnoreCase("left") || value.equals("0") ? 0 : 1;
                        break;
                    case "pad_tilt":
                        padTilt = Math.max(-60.0f, Math.min(60.0f, Float.parseFloat(value)));
                        break;
                    case "language":
                        language = value.equalsIgnoreCase("headset") ? "" : value;
                        break;
                    case "turn":
                        extraEnv.add("SHADPS4_VR_TURN="
                                + Math.max(0, Math.min(90, Integer.parseInt(value))));
                        break;
                    case "sharpen":
                        sharpen = Math.max(0, Math.min(4, Integer.parseInt(value)));
                        break;
                    case "cubic":
                        cubic = !value.equals("0");
                        break;
                    case "antialias":
                        antialias = !value.equals("0");
                        break;
                    case "pace":
                        pace = Math.max(0, Math.min(6, Integer.parseInt(value)));
                        break;
                    case "stick_touchpad":
                        stickTouchpad = !value.equals("0");
                        break;
                    case "motion":
                        motion = !value.equals("0");
                        break;
                    case "rumble":
                        rumble = !value.equals("0");
                        break;
                    case "stats":
                        showStats = !value.equals("0");
                        break;
                    case "predict_ms":
                        predictMs = Math.max(0, Math.min(80, Float.parseFloat(value)));
                        break;
                    case "msaa":
                        msaa = Math.max(1, Math.min(4, Integer.parseInt(value)));
                        break;
                    case "dynamic_resolution":
                        dynamicResolution = Math.max(0, Math.min(2, Integer.parseInt(value)));
                        break;
                    case "cpu_boost":
                        cpuBoost = !value.equals("0");
                        break;
                    case "fov":
                        fieldOfView = Math.max(FOV_MIN, Math.min(FOV_MAX, Integer.parseInt(value)));
                        break;
                    case "detail":
                        detail = value.equalsIgnoreCase("console") || value.equals("0")
                                ? DETAIL_CONSOLE : DETAIL_LIGHTER;
                        break;
                    case "foveation":
                        foveation = Math.max(0, Math.min(FOVEATION_MAX, Integer.parseInt(value)));
                        break;
                    case "fsr":
                        upscaleFsr = !value.equals("0");
                        break;
                    case "mic":
                        microphone = !value.equals("0");
                        break;
                    case "mic_gain":
                        microphoneGain = Math.max(0.1f, Math.min(30.0f, Float.parseFloat(value)));
                        break;
                    case "arg":
                        extraArgs.add(value);
                        break;
                    case "env":
                        extraEnv.add(value);
                        break;
                    default:
                        logWarning("unknown setting " + key, null);
                        continue;
                }
                logInfo("setting " + key + "=" + value);
            }
        } catch (IOException | NumberFormatException e) {
            logWarning("unable to read " + file, e);
        }
    }

    // --- runtime and game -----------------------------------------------------------------------

    /** The emulator must see the chosen optics before the game opens its emulated headset. */
    private void startGame() {
        if (!dryRun && headsetFov == null) {
            return;
        }
        choosingFov = false;
        shownStatus = "";
        // From here on, what the player presses is the game's; what is still held down from
        // confirming the menu is not, until it is let go.
        nativeSetPlaying(true);
        nativeSetFieldOfView(fieldOfView);
        if (!dryRun) {
            getPreferences(MODE_PRIVATE).edit().putInt("fov", fieldOfView)
                    .putInt("detail", detail).putInt("foveation", foveation)
                    .putBoolean("fsr", upscaleFsr)
                    .putInt("refresh", Math.round(refreshRate)).apply();
            logInfo("startup FOV chosen: " + fieldOfView + "%, detail: "
                    + (detail == DETAIL_CONSOLE ? "as on the console" : "lighter")
                    + ", foveated rendering: " + FOVEATION_NAMES[foveation]
                    + ", enlargement: " + (upscaleFsr ? "FSR 1" : "the game's own")
                    + ", display refresh rate: " + Math.round(refreshRate) + " Hz");
            if (inputView.hasWindowFocus()) {
                inputView.requestPointerCapture();
            }
        }
        nativeSetMicrophone(microphone, microphoneGain);
        if (microphone && !dryRun && checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                != PackageManager.PERMISSION_GRANTED) {
            logInfo("asking for the microphone");
            requestPermissions(new String[] {Manifest.permission.RECORD_AUDIO},
                    REQUEST_MICROPHONE);
        }
        new Thread(this::setUpAndStart, "setup").start();
    }

    private void setUpAndStart() {
        try {
            // On the first start, and after every update of the app: a few seconds.
            File runtime = new File(getFilesDir(), "runtime");
            setupStatus = "Installing the emulator runtime...";
            if (RuntimeInstaller.install(this, runtime)) {
                logInfo("runtime unpacked");
            }

            File external = getExternalFilesDir(null);
            File games = new File(external, "games");
            games.mkdirs();
            new File(external, "data/shadPS4/sys_modules").mkdirs();
            File eboot = findGame(games);
            if (eboot == null) {
                // Where "adb push" can put it without caring who owns the files.
                eboot = findGame(new File(SHELL_GAMES));
            }
            if (eboot == null) {
                setupFailed = true;
                setupStatus = "No game found.\n\nCopy the extracted game folder (the one that "
                        + "contains eboot.bin) to the headset, to either of\n" + games
                        + "/CUSA12392\n" + SHELL_GAMES + "/CUSA12392\nthen start this app again.";
                return;
            }
            String described = gameDescriptionProblem(eboot.getParentFile());
            if (described != null) {
                setupFailed = true;
                setupStatus = described;
                return;
            }

            File storage = new File(getFilesDir(), "core");
            storage.mkdirs();
            // Extra settings replace the defaults; the menu's FOV remains authoritative.
            List<String> env = new ArrayList<>();
            // Saves, firmware modules and logs go where the user can reach them over USB.
            env.add("XDG_DATA_HOME=" + new File(external, "data"));
            env.add("SHADPS4_MAX_MSAA=" + msaa);
            // The emulator has more busy threads than the headset has processor cores for it.
            // The sound must not stutter for that (the game mixes it on two threads, the
            // emulator passes it on with one per output), a frame should not wait behind the
            // game's helpers on the two threads all of it passes through, and the two that
            // keep time with the display (they have next to nothing to do) must do so on time.
            env.add("SHADPS4_THREAD_NICE=Game:Main=-14,shadPS4:GpuComm=-14,sndx_out_thread=-16,"
                    + "sndx_audio3d=-16,shadPS4:AudioOu=-16,shadPS4:VrHostL=-18,"
                    + "shadPS4:Present=-18");
            // On the GPU the emulator steps back behind this app: copying a finished picture
            // to the display takes a millisecond, and must not wait until the GPU is through
            // with everything the game has queued for the pictures after it.
            env.add("KGSL_COMPAT_PRIORITY=12");
            if (msaa == 1) {
                // Without multisampling the GPU driver's direct rendering is the faster of its
                // two ways to draw (about 5% of the GPU's time with this game); its own choice
                // per render pass goes for the tiled way too often.
                env.add("TU_DEBUG=sysmem");
            }
            // The driver's shader compiler puts no "preamble" before the game's shaders (the
            // part of a shader that is the same for a whole draw, worked out once at its
            // start): this game makes about 1700 draws a frame, mostly small, and the GPU spends
            // 0.5 ms a frame less without them (2.6%, four runs of the first level, 2026-10-11).
            env.add("IR3_SHADER_DEBUG=nopreamble");
            // A few lines every five or ten seconds in core.log: the frame rate and where a
            // frame's time goes, how well each sound port keeps up, what the microphone hears.
            env.add("SHADPS4_FRAME_STATS=1");
            env.add("SHADPS4_AUDIOOUT_STATS=1");
            env.add("SHADPS4_AUDIOIN_STATS=1");
            // The emulated headset refreshes as often as the display does (and, once frames
            // are shown, when it does): the game draws one frame for every two refreshes, or
            // for every three where that is all the GPU manages.
            int displayRate = (nativeXrStatus() >> 8) & 0xff;
            if (displayRate == 0) {
                displayRate = refreshRate > 0 ? Math.round(refreshRate) : 120;
            }
            env.add("SHADPS4_VR_REFRESH_RATE=" + displayRate);
            logInfo("the emulated headset refreshes " + displayRate + " times a second");
            if (resolution != null) {
                env.add("SHADPS4_TITLE_RESOLUTION=" + resolution);
            } else {
                env.add("SHADPS4_VR_LARGEST_SIZE=" + largestResolution);
            }
            if (!realTime) {
                env.add("SHADPS4_TITLE_TIMESTEP=0");
            }
            if (pace >= 2) {
                env.add("SHADPS4_VR_PACE=" + pace);
            } else {
                // 30 frames a second throughout (3 refreshes at 90 Hz, 4 at 120): only the size
                // of the picture follows what the GPU manages, so frames never come unevenly
                // because a faster rate was tried.
                env.add("SHADPS4_VR_STEADY_FPS=30");
            }
            if (headsetFov != null) {
                double angle = horizontalFov(fieldOfView);
                logInfo("the game draws " + fieldOfView + "% of this headset's field of view: "
                        + Math.round(angle) + " visible degrees across an eye, "
                        + Math.round(centerPixelsPerDegree(fieldOfView) * 10) / 10.0
                        + " pixels per degree at the center"
                        + " (100 fills the headset; "
                        + (fieldOfView < 100 ? "soft squircle border" : "no added border") + ")");
            }
            if (!antialias) {
                env.add("SHADPS4_RESOLVE_AA=0");
            }
            if (sharpen == 1) {
                // The game draws 1440 pixels across for what the display shows with well over
                // twice as many, and enlarges its scene to get even there.
                env.add("SHADPS4_VR_SHARPEN=0.6");
            }
            if (detail == DETAIL_LIGHTER) {
                // Models change to their simpler forms at a third of the game's distances: a
                // sixth of the GPU's time in a level like 3-2, hard to see from the seat. The
                // sun's shadow maps a quarter of the size: a few percent more.
                env.add("SHADPS4_TITLE_LOD=0.333");
                env.add("SHADPS4_TITLE_SHADOW_MAP=1024");
            }
            if (foveation > 0) {
                // The edges of each eye's scene shaded in blocks, centred on straight ahead
                // (which the symmetric projection below makes the middle of the picture).
                env.add("SHADPS4_VR_FOVEATION=" + foveation);
            }
            // Wherever the game draws its scene smaller than the picture it hands over, the
            // middle of it is enlarged with FSR 1 (crisper edges) unless the game is to do it
            // itself.
            env.add("SHADPS4_TITLE_FSR=" + (upscaleFsr ? 2 : 0));
            // The game speaks the language its console is set to, if it has it (English
            // otherwise): the one the headset is set to, unless the settings name another.
            env.add("SHADPS4_CONSOLE_LANGUAGE="
                    + (language.isEmpty() ? Locale.getDefault().toLanguageTag() : language));
            env.addAll(extraEnv);
            // Explicit even at 100, so an inherited environment cannot change the choice.
            env.add("SHADPS4_VR_FOV=" + fieldOfView);
            env.add("SHADPS4_VR_FOV_OF=headset");
            env.add("SHADPS4_VR_FOV_SYMMETRIC=1");
            if (headsetFov != null) {
                env.add("SHADPS4_VR_HEADSET_FOV_TAN=" + headsetFov[0] + "," + headsetFov[1]
                        + "," + headsetFov[2] + "," + headsetFov[3]);
            }

            setupStatus = "Starting the emulator...";
            keepPrevious(new File(external, "core.log"), new File(external, "core.prev.log"));
            logInfo("game: " + eboot);
            String loader = getApplicationInfo().nativeLibraryDir + "/libastro_ld.so";
            boolean started = nativeStartCore(loader, runtime.getAbsolutePath(),
                    storage.getAbsolutePath(), eboot.getAbsolutePath(),
                    new File(external, "core.log").getAbsolutePath(),
                    extraArgs.toArray(new String[0]), env.toArray(new String[0]));
            if (!started) {
                setupFailed = true;
                setupStatus = "The emulator could not be started.";
            }
        } catch (IOException e) {
            nativeLog(Log.ERROR, "setup failed: " + e);
            setupFailed = true;
            setupStatus = "Setup failed: " + e.getMessage();
        }
    }

    /**
     * What is wrong with the game's description, sce_sys/param.sfo, or null if nothing is: the
     * emulator stops at once without it, and a package keeps it apart from the game's files, so
     * a game unpacked by hand can lack it, or have another of the package's entries in its place.
     */
    private static String gameDescriptionProblem(File folder) {
        File description = new File(folder, "sce_sys/param.sfo");
        String fix = "\n\nUnpack the game's package with the PC package's launcher, which adds it, "
                + "or add it by hand as the README says (the number to give PkgTool is the one "
                + "in the Index column of the PARAM_SFO line), then start this app again.";
        if (!description.isFile()) {
            return "The game in\n" + folder + "\nhas no description (sce_sys/param.sfo)." + fix;
        }
        byte[] head = new byte[4];
        try (java.io.FileInputStream in = new java.io.FileInputStream(description)) {
            if (in.read(head) == head.length && head[0] == 0 && head[1] == 'P' && head[2] == 'S'
                    && head[3] == 'F') {
                return null;
            }
        } catch (IOException e) {
            // Unreadable here: the emulator says what it makes of it.
            return null;
        }
        return "The game in\n" + folder + "\nhas a file sce_sys/param.sfo that is not the game's "
                + "description (it does not start with PSF)." + fix;
    }

    /** The game to run: games/CUSA12392 if present, otherwise the first folder with an eboot. */
    private static File findGame(File games) {
        File preferred = new File(games, "CUSA12392/eboot.bin");
        if (preferred.isFile()) {
            return preferred;
        }
        File[] folders = games.listFiles();
        if (folders != null) {
            for (File folder : folders) {
                File eboot = new File(folder, "eboot.bin");
                if (eboot.isFile()) {
                    return eboot;
                }
            }
        }
        return null;
    }

    // --- status panel ---------------------------------------------------------------------------

    private void refreshStatus() {
        if (choosingFov) {
            headsetFov = nativeHeadsetFov();
            String status = "fov:" + fieldOfView + ":" + detail + ":" + foveation + ":"
                    + upscaleFsr + ":"
                    + Math.round(refreshRate) + ":" + menuRow + ":"
                    + (gamepad != null) + ":"
                    + ((nativeController() & CONTROLLER_OWN_HELD) != 0)
                    + ":" + Arrays.toString(headsetFov);
            if (!status.equals(shownStatus)) {
                shownStatus = status;
                drawFovMenu();
            }
            handler.postDelayed(this::refreshStatus, 500);
            return;
        }
        String status = setupStatus;
        if (!setupFailed) {
            switch (nativeCoreState()) {
                case CORE_STARTING:
                    status = "Starting the emulator...";
                    break;
                case CORE_RUNNING:
                    status = nativeFrameCount() == 0 ? "Booting the game..." : "Loading...";
                    if (showStats && nativeFrameCount() > 0) {
                        status = statsLine();
                    }
                    // A game that stopped drawing looks like one that loads. Loads end.
                    long frames = nativeFrameCount();
                    long now = SystemClock.elapsedRealtime();
                    if (frames != quietFrames) {
                        quietFrames = frames;
                        quietSince = now;
                    }
                    long quiet = (now - quietSince) / 1000;
                    if (frames > 0 && quiet >= 20) {
                        status = "No new picture from the game for " + quiet / 5 * 5
                                + " seconds.\nA long load looks like this too. Past a minute the "
                                + "game is stuck: close the app with the Meta button and start it "
                                + "again. What happened is kept in host.log and core.log.";
                    }
                    break;
                case CORE_STOPPED:
                case CORE_FAILED:
                    status = nativeCoreMessage() + "\n\nLog: "
                            + new File(getExternalFilesDir(null), "core.log");
                    break;
                default:
                    break;
            }
            // Waiting is the moment to say what the game will be played with.
            int controller = nativeController();
            if ((controller & CONTROLLER_WHICH) == CONTROLLER_OWN) {
                status += "\n\nController: the headset's own (A cross, B square, X circle, Y "
                        + "triangle, right stick = touchpad, menu button = OPTIONS)."
                        + "\nTo reset the view, hold the menu button for a second or press both "
                        + "sticks in.";
            } else if (gamepad != null) {
                status += "\n\nController: " + gamepad.getName()
                        + (controllerSensors != null ? ""
                                : " (no motion sensors: your hands turn it in the game)")
                        + "\nTo reset the view, hold OPTIONS for a second"
                        + (gamepad.getVendorId() == VENDOR_SONY ? " or press the PS button" : "")
                        + ": where you are then is your seat, and the way you face is straight "
                        + "ahead.";
            } else {
                status += "\n\nNo controller found. Pick up the headset's controllers, or switch "
                        + "a gamepad on (pair it first under Settings > Bluetooth if it never "
                        + "was).";
            }
        }
        // On a controller without a touchpad, buttons do what the game wants done on one:
        // which ones is said over the game whenever OPTIONS is pressed (the game's pause), for
        // as long as it takes to read. (Not as the game starts: there it was in the way.)
        String title = "Astro VR Host";
        int controller = nativeController();
        int presses = nativeOptionsPresses();
        long clock = SystemClock.elapsedRealtime();
        boolean playing = !setupFailed && nativeCoreState() == CORE_RUNNING
                && nativeFrameCount() > 0;
        if (playing && (controller & CONTROLLER_GESTURE_BUTTONS) != 0 && presses != optionsSeen) {
            cardUntil = clock + CARD_ON_PAUSE_MS;
        }
        optionsSeen = presses;
        boolean card = playing && (controller & CONTROLLER_GESTURE_BUTTONS) != 0
                && clock < cardUntil;
        if (card != cardShowing) {
            cardShowing = card;
            nativeShowPanel(card);
        }
        if (card) {
            title = "The touchpad, on this controller";
            status = controlsCard((controller & CONTROLLER_WHICH) == CONTROLLER_OWN);
        }
        if (!status.equals(shownStatus)) {
            shownStatus = status;
            drawStatus(title, status);
        }
        handler.postDelayed(this::refreshStatus, card || playing ? 250 : 500);
    }

    /** Which buttons do what a finger does on a DualShock 4's touchpad. */
    private String controlsCard(boolean ownControllers) {
        if (ownControllers) {
            String pad = padHand == 0 ? "Left" : "Right";
            String other = padHand == 0 ? "Right" : "Left";
            return pad + " trigger: press the touchpad (water, guns)\n"
                    + pad + " grip: swipe forward (hook, stars, chests)\n"
                    + other + " trigger: pull back, aim at the goal, let go (catapult)\n"
                    + "Right stick: a finger on the touchpad\n"
                    + other + " grip held, right stick to a side: turn the view\n"
                    + "X and Y together: blow\n"
                    + "Shown whenever the menu button is pressed.";
        }
        return "R2: press the touchpad (water, guns)\n"
                + "R1: swipe forward (hook, stars, chests)\n"
                + "L2: pull back, aim at the goal, let go (catapult)\n"
                + "Right stick: a finger on the touchpad\n"
                + "L1 held, right stick to a side: turn the view\n"
                + "Shown whenever OPTIONS is pressed.";
    }

    /** What the game and the display are doing, for the panel that stats=1 keeps in view. */
    private String statsLine() {
        long frames = nativeFrameCount();
        long now = SystemClock.elapsedRealtime();
        if (statsFrames < 0) {
            statsFrames = frames;
            statsTime = now;
        } else if (now - statsTime >= 2000) {
            // Over two seconds: a rate counted over less jumps about.
            statsRate = String.format(Locale.US, "%.0f",
                    (frames - statsFrames) * 1000.0 / (now - statsTime));
            statsFrames = frames;
            statsTime = now;
        }
        String rate = statsRate;
        int xr = nativeXrStatus();
        return "Game: " + rate + " frames a second\nDisplay: " + ((xr >> 8) & 0xff) + " Hz\n"
                + ((nativeController() & CONTROLLER_WHICH) == CONTROLLER_OWN
                        ? "The headset's controller: " + ((xr & 4) != 0 ? "seen" : "not seen")
                        : "Hands holding the controller: " + ((xr & 2) != 0 ? "seen" : "not seen"));
    }

    /** Visible coverage is the symmetric render envelope clipped by the actual headset. */
    private double horizontalFov(int percent) {
        double renderTan = Math.max(headsetFov[0], headsetFov[1]) * percent / 100.0;
        return Math.toDegrees(Math.atan(Math.min(headsetFov[0], renderTan))
                + Math.atan(Math.min(headsetFov[1], renderTan)));
    }

    private double centerPixelsPerDegree(int percent) {
        double renderTan = Math.max(headsetFov[0], headsetFov[1]) * percent / 100.0;
        return 1440 * Math.PI / 180 / (2 * renderTan);
    }

    /**
     * A popup drawn into the OpenXR panel: visible in both eyes while the core is still idle. The
     * host stands it still in the room, in front of where the player faced when it appeared.
     */
    private void drawFovMenu() {
        Bitmap bitmap = Bitmap.createBitmap(STATUS_WIDTH, STATUS_HEIGHT, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(Color.rgb(16, 20, 32));
        canvas.drawRoundRect(new RectF(0, 0, STATUS_WIDTH, STATUS_HEIGHT), 40, 40, paint);

        paint.setColor(Color.rgb(90, 170, 255));
        paint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        paint.setTextSize(34);
        canvas.drawText("Before you play", 48, 43, paint);

        // The line that left and right change is the one with a frame around it.
        final float[][] rows = {{52, 172}, {176, 240}, {244, 308}, {312, 376}, {380, 444}};
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(3);
        canvas.drawRoundRect(new RectF(24, rows[menuRow][0], 1000, rows[menuRow][1]), 18, 18,
                paint);
        paint.setStyle(Paint.Style.FILL);

        // The field of view.
        paint.setColor(Color.WHITE);
        paint.setTextSize(26);
        canvas.drawText("Field of view", 48, 82, paint);
        paint.setTextAlign(Paint.Align.RIGHT);
        canvas.drawText(headsetFov == null ? fieldOfView + "% of your headset's"
                : fieldOfView + "%  /  about " + Math.round(horizontalFov(fieldOfView))
                        + " degrees per eye", 976, 82, paint);
        paint.setTextAlign(Paint.Align.LEFT);
        paint.setTypeface(Typeface.DEFAULT);
        paint.setTextSize(20);
        String density = "Detecting your headset's field of view...";
        if (headsetFov != null) {
            int gain = (int) Math.round((100.0 / fieldOfView - 1) * 100);
            density = gain == 0 ? "All of your headset's view, at the game's own pixel density"
                    : "A narrower view: about " + gain + "% more pixels per degree at the center";
        }
        canvas.drawText(density, 48, 107, paint);
        paint.setColor(Color.rgb(48, 56, 76));
        canvas.drawRoundRect(new RectF(120, 120, 904, 132), 6, 6, paint);
        float sliderX = 120 + (fieldOfView - FOV_MIN) * 784.0f / (FOV_MAX - FOV_MIN);
        paint.setColor(Color.rgb(90, 170, 255));
        canvas.drawRoundRect(new RectF(120, 120, sliderX, 132), 6, 6, paint);
        canvas.drawCircle(sliderX, 126, 11, paint);
        paint.setColor(Color.LTGRAY);
        paint.setTextSize(18);
        canvas.drawText("50%: sharper", 120, 160, paint);
        paint.setTextAlign(Paint.Align.RIGHT);
        canvas.drawText("100%: all of it", 904, 160, paint);
        paint.setTextAlign(Paint.Align.CENTER);
        canvas.drawText(fieldOfView < 100 ? "Soft border  /  Default 85%"
                : "No border  /  Default 85%", STATUS_WIDTH / 2.0f, 160, paint);

        // How much is drawn further away.
        drawMenuLine(canvas, paint, rows[1][0], "Detail further away",
                detail == DETAIL_CONSOLE ? "As on the console" : "Lighter: faster",
                detail == DETAIL_CONSOLE
                        ? "Everything the PS4 version draws, at any distance: the most GPU work"
                        : "Simpler models further away, softer shadow edges: hard to tell apart");

        // How much of the edges is shaded in blocks.
        drawMenuLine(canvas, paint, rows[2][0], "Foveated rendering",
                FOVEATION_NAMES[foveation] + (foveation == FOVEATION_DEFAULT ? " (default)" : ""),
                foveation == 0 ? "Every pixel shaded on its own, out to the edges of the picture"
                        : foveation == 1 ? "The picture's outer edge shaded in blocks of 2x2 pixels"
                        : foveation == 2 ? "The outer part of the picture shaded in 2x2 blocks"
                        : foveation == 3 ? "Most of the edges in 2x2 blocks, the far edges in 4x4"
                        : "All but the middle in 2x2 blocks, the outer part in 4x4");

        // How the game's picture is enlarged.
        drawMenuLine(canvas, paint, rows[3][0], "Picture enlarged with",
                upscaleFsr ? "FSR 1 (default)" : "The game's own",
                upscaleFsr ? "AMD FSR 1 in the middle of the view: crisper edges"
                        : "The game's own stretch, as on the console: softer edges");

        // How often the display refreshes.
        int rate = Math.round(refreshRate);
        drawMenuLine(canvas, paint, rows[4][0], "Display refresh rate",
                rate + " Hz" + (rate == REFRESH_DEFAULT ? " (default)" : ""),
                rate == REFRESH_DEFAULT
                        ? "Each of the 30 frames shown 3 times: the most GPU time for the game"
                        : rate == REFRESH_FASTER
                        ? "Each frame shown 4 times: smoother head turns, less GPU time for the game"
                        : "As vrhost.txt asks");

        // Whatever the player has in their hands chooses: a gamepad, or the headset's own
        // controllers.
        boolean controller = gamepad != null
                || (nativeController() & CONTROLLER_OWN_HELD) != 0;
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTypeface(Typeface.DEFAULT);
        paint.setColor(controller ? Color.WHITE : Color.rgb(255, 200, 110));
        paint.setTextSize(21);
        canvas.drawText(controller ? "Up / Down: choose a line    Left / Right: change it"
                : "Pick up the controllers, or switch a gamepad on, to choose and start",
                STATUS_WIDTH / 2.0f, 467, paint);

        paint.setColor(headsetFov != null ? Color.rgb(45, 104, 182) : Color.rgb(48, 56, 76));
        canvas.drawRoundRect(new RectF(322, 475, 702, 509), 16, 16, paint);
        paint.setColor(Color.WHITE);
        paint.setTextSize(25);
        paint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        canvas.drawText(headsetFov != null ? "X / A or trigger: Play" : "Detecting FOV...",
                STATUS_WIDTH / 2.0f, 501, paint);
        publishStatusBitmap(bitmap);
    }

    /** A line of the start-up menu with a choice and what it does, from `top` down. */
    private static void drawMenuLine(Canvas canvas, Paint paint, float top, String label,
            String value, String description) {
        paint.setTextAlign(Paint.Align.LEFT);
        paint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        paint.setColor(Color.WHITE);
        paint.setTextSize(26);
        canvas.drawText(label, 48, top + 28, paint);
        paint.setTextAlign(Paint.Align.RIGHT);
        canvas.drawText("<  " + value + "  >", 976, top + 28, paint);
        paint.setTextAlign(Paint.Align.LEFT);
        paint.setTypeface(Typeface.DEFAULT);
        paint.setTextSize(20);
        canvas.drawText(description, 48, top + 54, paint);
    }

    /** Left or right in the start-up menu: changes the line chosen. */
    private void adjustMenu(int direction) {
        if (menuRow == 0) {
            adjustFov(direction);
            return;
        }
        // Lines of two choices take the one on the side pushed to, so that a stick held over
        // does not flip them back and forth.
        if (menuRow == 1) {
            detail = direction < 0 ? DETAIL_CONSOLE : DETAIL_LIGHTER;
        } else if (menuRow == 2) {
            foveation = Math.max(0, Math.min(FOVEATION_MAX, foveation + direction));
        } else if (menuRow == 3) {
            upscaleFsr = direction > 0;
        } else {
            int wanted = direction < 0 || !offersRate(REFRESH_FASTER) ? REFRESH_DEFAULT
                    : REFRESH_FASTER;
            if (wanted != Math.round(refreshRate)) {
                // The display switches at once: what the menu says is what it does.
                refreshRate = wanted;
                nativeSetRefreshRate(refreshRate);
            }
        }
        drawFovMenu();
    }

    /** Up or down in the start-up menu: chooses another line. */
    private void moveMenuRow(int direction) {
        int row = Math.max(0, Math.min(MENU_ROWS - 1, menuRow + direction));
        if (row != menuRow) {
            menuRow = row;
            drawFovMenu();
        }
    }

    private void adjustFov(int direction) {
        fieldOfView = Math.max(FOV_MIN, Math.min(FOV_MAX, fieldOfView + direction * FOV_STEP));
        drawFovMenu();
    }

    /**
     * The start-up menu, worked with the headset's own controllers: a stick to a side
     * chooses, A or a trigger starts the game. (The native side reads them; nothing of them
     * comes this way by itself.)
     */
    private void pollOwnControllers() {
        if (!choosingFov) {
            return;
        }
        int menu = nativeTouchMenu();
        long now = SystemClock.uptimeMillis();
        int direction = (menu & MENU_LEFT) != 0 ? -1 : (menu & MENU_RIGHT) != 0 ? 1 : 0;
        if (direction == 0) {
            nextOwnFovMoveAt = 0;
        } else if (now >= nextOwnFovMoveAt) {
            nativeUseController(CONTROLLER_OWN);
            adjustMenu(direction);
            nextOwnFovMoveAt = now + 250;
        }
        int row = (menu & MENU_UP) != 0 ? -1 : (menu & MENU_DOWN) != 0 ? 1 : 0;
        if (row == 0) {
            nextOwnRowMoveAt = 0;
        } else if (now >= nextOwnRowMoveAt) {
            nativeUseController(CONTROLLER_OWN);
            moveMenuRow(row);
            nextOwnRowMoveAt = now + 300;
        }
        if ((menu & MENU_CONFIRM) == 0) {
            ownConfirmArmed = true;
        } else if (ownConfirmArmed && headsetFov != null) {
            logInfo("the menu was confirmed with the headset's own controllers");
            nativeUseController(CONTROLLER_OWN);
            startGame();
            return;
        }
        handler.postDelayed(this::pollOwnControllers, 50);
    }

    private void drawStatus(String title, String status) {
        Bitmap bitmap = Bitmap.createBitmap(STATUS_WIDTH, STATUS_HEIGHT, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(Color.argb(230, 16, 20, 32));
        canvas.drawRoundRect(new RectF(0, 0, STATUS_WIDTH, STATUS_HEIGHT), 40, 40, paint);

        paint.setColor(Color.rgb(90, 170, 255));
        paint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        paint.setTextSize(52);
        canvas.drawText(title, 48, 86, paint);

        paint.setColor(Color.WHITE);
        paint.setTypeface(Typeface.DEFAULT);
        paint.setTextSize(30);
        float y = 160;
        for (String paragraph : status.split("\n")) {
            // Wrap by measuring: at a space where there is one, anywhere where there is none
            // (paths have no spaces to break at).
            String rest = paragraph;
            do {
                int count = paint.breakText(rest, true, STATUS_WIDTH - 96, null);
                if (count < rest.length()) {
                    int space = rest.lastIndexOf(' ', count);
                    if (space > 0) {
                        count = space + 1;
                    }
                }
                canvas.drawText(rest, 0, count, 48, y, paint);
                rest = rest.substring(count);
                y += 40;
            } while (!rest.isEmpty());
        }

        publishStatusBitmap(bitmap);
    }

    private void publishStatusBitmap(Bitmap bitmap) {
        if (dryRun && statusPictures < 12) {
            // Nobody sees the panel in a test: its pictures are kept for looking at, in the
            // folder the test has made for them.
            File picture = new File(new File(getFilesDir(), "drystart"),
                    "status-" + statusPictures++ + ".png");
            try (FileOutputStream out = new FileOutputStream(picture)) {
                bitmap.compress(Bitmap.CompressFormat.PNG, 100, out);
            } catch (IOException e) {
                logWarning("unable to write " + picture, e);
            }
        }

        ByteBuffer pixels = ByteBuffer.allocateDirect(STATUS_WIDTH * STATUS_HEIGHT * 4);
        bitmap.copyPixelsToBuffer(pixels);
        nativeSetStatusImage(pixels);
        bitmap.recycle();
    }

    // --- controller: buttons and sticks ---------------------------------------------------------

    private static int buttonFor(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A:
                return CROSS;
            case KeyEvent.KEYCODE_BUTTON_B:
                return CIRCLE;
            case KeyEvent.KEYCODE_BUTTON_X:
                return SQUARE;
            case KeyEvent.KEYCODE_BUTTON_Y:
                return TRIANGLE;
            case KeyEvent.KEYCODE_BUTTON_L1:
                return L1;
            case KeyEvent.KEYCODE_BUTTON_R1:
                return R1;
            case KeyEvent.KEYCODE_BUTTON_L2:
                return L2;
            case KeyEvent.KEYCODE_BUTTON_R2:
                return R2;
            case KeyEvent.KEYCODE_BUTTON_THUMBL:
                return L3;
            case KeyEvent.KEYCODE_BUTTON_THUMBR:
                return R3;
            case KeyEvent.KEYCODE_BUTTON_START:
                return OPTIONS;
            // The Create button stands in for a click of the touchpad as well.
            case KeyEvent.KEYCODE_BUTTON_SELECT:
            case KeyEvent.KEYCODE_BUTTON_1:
                return TOUCHPAD;
            case KeyEvent.KEYCODE_DPAD_UP:
                return UP;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                return RIGHT;
            case KeyEvent.KEYCODE_DPAD_DOWN:
                return DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT:
                return LEFT;
            default:
                return 0;
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (isOwnController(event.getDevice())) {
            // The headset's own controllers are read by the native side, through the
            // headset's runtime. Should the system pass their buttons this way as well, they
            // are not counted twice, nor left to what the system makes of a button nobody
            // took: it takes B for "back", and may pass the menu button on as "back" itself,
            // either of which would close the app in the middle of the game.
            int code = event.getKeyCode();
            boolean back = code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_MENU;
            if (back && choosingFov) {
                // (Before the game, backing out of the app is what it has always done.)
                if (code == KeyEvent.KEYCODE_BACK && event.getAction() == KeyEvent.ACTION_DOWN) {
                    finish();
                }
                return true;
            }
            return back || buttonFor(code) != 0 || code == KeyEvent.KEYCODE_BUTTON_MODE
                    || super.dispatchKeyEvent(event);
        }
        // Consume the entire confirming press, including repeats and its release. It must
        // not press X in the game, or move the player's seat before they have settled in.
        if (fovConfirmKey != KeyEvent.KEYCODE_UNKNOWN && event.getKeyCode() == fovConfirmKey) {
            if (event.getAction() == KeyEvent.ACTION_UP) {
                fovConfirmKey = KeyEvent.KEYCODE_UNKNOWN;
            }
            return true;
        }
        if (choosingFov) {
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
                noteInputFrom(event.getDevice());
                if (isGamepad(event.getDevice())) {
                    nativeUseController(CONTROLLER_GAMEPAD);
                }
                switch (event.getKeyCode()) {
                    case KeyEvent.KEYCODE_DPAD_LEFT:
                        adjustMenu(-1);
                        break;
                    case KeyEvent.KEYCODE_DPAD_RIGHT:
                        adjustMenu(1);
                        break;
                    case KeyEvent.KEYCODE_DPAD_UP:
                        moveMenuRow(-1);
                        break;
                    case KeyEvent.KEYCODE_DPAD_DOWN:
                        moveMenuRow(1);
                        break;
                    case KeyEvent.KEYCODE_BUTTON_A:
                    case KeyEvent.KEYCODE_DPAD_CENTER:
                    case KeyEvent.KEYCODE_ENTER:
                        if (headsetFov != null) {
                            fovConfirmKey = event.getKeyCode();
                            startGame();
                        }
                        break;
                    case KeyEvent.KEYCODE_BACK:
                        finish();
                        break;
                    default:
                        break;
                }
            }
            return true;
        }
        if (event.getKeyCode() == KeyEvent.KEYCODE_BUTTON_MODE) {
            // The PS button resets the view: where the head is now is where the player sits,
            // the way they face is straight ahead, and so is the way the controller points
            // (which only matters while the headset does not see the hands holding it).
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
                logInfo("PS button: view reset");
                nativeRecenter(RECENTER_PAD | RECENTER_SEAT);
            }
            return true;
        }
        int keyCode = event.getKeyCode();
        if (keyCode == KeyEvent.KEYCODE_BACK && isGamepad(event.getDevice())) {
            // Some gamepads have the system take their Back / View button for its own "back",
            // which would close the app in the middle of the game: it is the button that
            // presses the touchpad, as on every other gamepad.
            keyCode = KeyEvent.KEYCODE_BUTTON_SELECT;
        }
        int button = buttonFor(keyCode);
        if (button == 0) {
            return super.dispatchKeyEvent(event);
        }
        // (What the buttons mean about the view besides - the first press of X takes the
        // player's seat, OPTIONS held for a second resets the view - the native side sees
        // to, for whichever controller the game is played with.)
        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            noteInputFrom(event.getDevice());
            keyButtons |= button;
        } else if (event.getAction() == KeyEvent.ACTION_UP) {
            keyButtons &= ~button;
        }
        sendInput();
        return true;
    }

    private static float axis(MotionEvent event, int axis) {
        InputDevice.MotionRange range = event.getDevice() != null
                ? event.getDevice().getMotionRange(axis, InputDevice.SOURCE_JOYSTICK) : null;
        float value = event.getAxisValue(axis);
        // Sticks rest slightly off centre.
        return range != null && Math.abs(value) <= range.getFlat() ? 0.0f : value;
    }

    /**
     * Where a gamepad has its right stick and whether its triggers say how far they are
     * pulled: most gamepads agree on that, the others are met half way.
     */
    private void learnAxes(InputDevice device) {
        if (device == null || device.getId() == axesDevice) {
            return;
        }
        axesDevice = device.getId();
        boolean standard = hasAxis(device, MotionEvent.AXIS_Z)
                && hasAxis(device, MotionEvent.AXIS_RZ);
        boolean other = hasAxis(device, MotionEvent.AXIS_RX)
                && hasAxis(device, MotionEvent.AXIS_RY);
        rightXAxis = standard || !other ? MotionEvent.AXIS_Z : MotionEvent.AXIS_RX;
        rightYAxis = standard || !other ? MotionEvent.AXIS_RZ : MotionEvent.AXIS_RY;
        analogTriggers = hasAxis(device, MotionEvent.AXIS_LTRIGGER)
                || hasAxis(device, MotionEvent.AXIS_BRAKE)
                || hasAxis(device, MotionEvent.AXIS_RTRIGGER)
                || hasAxis(device, MotionEvent.AXIS_GAS);
        if (!standard || !analogTriggers) {
            logInfo("this gamepad: right stick on " + MotionEvent.axisToString(rightXAxis) + " and "
                    + MotionEvent.axisToString(rightYAxis) + ", triggers "
                    + (analogTriggers ? "by how far they are pulled" : "as buttons only"));
        }
    }

    private static boolean hasAxis(InputDevice device, int axis) {
        return device.getMotionRange(axis, InputDevice.SOURCE_JOYSTICK) != null;
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (isOwnController(event.getDevice())) {
            // Read by the native side, see dispatchKeyEvent.
            return true;
        }
        if (choosingFov) {
            if (event.isFromSource(InputDevice.SOURCE_JOYSTICK)
                    && event.getAction() == MotionEvent.ACTION_MOVE) {
                noteInputFrom(event.getDevice());
                float x = axis(event, MotionEvent.AXIS_X);
                float y = axis(event, MotionEvent.AXIS_Y);
                float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
                float hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
                // The way the stick is pushed most is the one that counts.
                boolean sideways = Math.abs(x) >= Math.abs(y);
                int direction = hatX < -0.5f || (sideways && x < -0.55f) ? -1
                        : hatX > 0.5f || (sideways && x > 0.55f) ? 1 : 0;
                int row = hatY < -0.5f || (!sideways && y < -0.55f) ? -1
                        : hatY > 0.5f || (!sideways && y > 0.55f) ? 1 : 0;
                long now = SystemClock.uptimeMillis();
                if (direction == 0) {
                    nextFovMoveAt = 0;
                } else if (now >= nextFovMoveAt) {
                    nativeUseController(CONTROLLER_GAMEPAD);
                    adjustMenu(direction);
                    nextFovMoveAt = now + 250;
                }
                if (row == 0) {
                    nextRowMoveAt = 0;
                } else if (now >= nextRowMoveAt) {
                    nativeUseController(CONTROLLER_GAMEPAD);
                    moveMenuRow(row);
                    nextRowMoveAt = now + 300;
                }
            }
            return true;
        }
        if (event.isFromSource(InputDevice.SOURCE_JOYSTICK)
                && event.getAction() == MotionEvent.ACTION_MOVE) {
            learnAxes(event.getDevice());
            leftX = axis(event, MotionEvent.AXIS_X);
            leftY = axis(event, MotionEvent.AXIS_Y);
            rightX = axis(event, rightXAxis);
            rightY = axis(event, rightYAxis);
            leftTrigger = Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER),
                    event.getAxisValue(MotionEvent.AXIS_BRAKE));
            rightTrigger = Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER),
                    event.getAxisValue(MotionEvent.AXIS_GAS));

            float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
            float hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
            hatButtons = (hatX < -0.5f ? LEFT : 0) | (hatX > 0.5f ? RIGHT : 0)
                    | (hatY < -0.5f ? UP : 0) | (hatY > 0.5f ? DOWN : 0);
            triggerButtons = (leftTrigger > 0.12f ? L2 : 0) | (rightTrigger > 0.12f ? R2 : 0);
            sendInput();
            return true;
        }
        if (handleTouchpad(event)) {
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    /** The controller's own touchpad, when the system exposes it with finger positions. */
    private boolean handleTouchpad(MotionEvent event) {
        if (!event.isFromSource(InputDevice.SOURCE_TOUCHPAD) || event.getDevice() == null) {
            return false;
        }
        // Sticks and touchpad are one device with two meanings for X and Y.
        InputDevice.MotionRange rangeX = event.getDevice().getMotionRange(MotionEvent.AXIS_X,
                InputDevice.SOURCE_TOUCHPAD);
        InputDevice.MotionRange rangeY = event.getDevice().getMotionRange(MotionEvent.AXIS_Y,
                InputDevice.SOURCE_TOUCHPAD);
        if (rangeX == null || rangeY == null || rangeX.getRange() <= 0 || rangeY.getRange() <= 0) {
            return false;
        }
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_MOVE:
                padTouchDown = true;
                padTouchX = (event.getX() - rangeX.getMin()) / rangeX.getRange();
                padTouchY = (event.getY() - rangeY.getMin()) / rangeY.getRange();
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                padTouchDown = false;
                break;
            default:
                break;
        }
        padTouchClick = (event.getButtonState() & MotionEvent.BUTTON_PRIMARY) != 0;
        sendInput();
        return true;
    }

    /**
     * Hands what the gamepad says to the native side as it is. What a gamepad lacks is made
     * up for there, the same way for every controller: the right stick as a finger on the
     * touchpad (unless stick_touchpad=0), a finger in the pad's middle for a button that
     * presses it.
     */
    private void sendInput() {
        if (choosingFov) {
            return;
        }
        int buttons = keyButtons | hatButtons | triggerButtons | (padTouchClick ? TOUCHPAD : 0);
        float left = leftTrigger, right = rightTrigger;
        if (!analogTriggers) {
            // Triggers that are only buttons are pulled all the way or not at all.
            left = (keyButtons & L2) != 0 ? 1.0f : 0.0f;
            right = (keyButtons & R2) != 0 ? 1.0f : 0.0f;
        }
        nativeInput(buttons, leftX, leftY, rightX, rightY, left, right, padTouchDown, padTouchX,
                padTouchY);
    }

    // --- controller: the device, its motion sensors, rumble and light ----------------------------

    /**
     * The headset's own controllers, which the system lists as gamepads too (and first, when
     * the app was started with one of them). They are no gamepad here: this way they come
     * without where they are, without rumble and without telling whether anybody holds them.
     * The native side reads them through the headset's runtime, which has all of that.
     */
    private static boolean isOwnController(InputDevice device) {
        return device != null && device.getVendorId() == VENDOR_META
                && device.supportsSource(InputDevice.SOURCE_GAMEPAD);
    }

    private static boolean isGamepad(InputDevice device) {
        return device != null && !device.isVirtual() && !isOwnController(device)
                && device.supportsSource(InputDevice.SOURCE_GAMEPAD)
                && device.supportsSource(InputDevice.SOURCE_JOYSTICK);
    }

    /**
     * How much a device looks like the controller the game was made for, where more than one
     * gamepad is there: a controller from the console's maker goes before anything else, and
     * motion sensors and motors count for the rest.
     */
    private int rank(InputDevice device) {
        int rank = 0;
        if (device.getVendorId() == VENDOR_SONY) {
            rank += 8;
        }
        try {
            if (findSensors(device) != null) {
                rank += 2;
            }
            if (device.getVibratorManager().getVibratorIds().length > 0) {
                rank += 1;
            }
        } catch (RuntimeException e) {
            // A device that cannot say what it has is ranked by its maker alone.
        }
        return rank;
    }

    private void findGamepad() {
        InputManager inputManager = getSystemService(InputManager.class);
        InputDevice best = gamepad;
        for (int id : inputManager.getInputDeviceIds()) {
            InputDevice device = inputManager.getInputDevice(id);
            if (isGamepad(device) && (best == null || rank(device) > rank(best))) {
                best = device;
            }
        }
        useGamepad(best);
    }

    /** Makes `device` the controller, if it is not that already. */
    private void useGamepad(InputDevice device) {
        if (device == null || (gamepad != null && gamepad.getId() == device.getId())) {
            return;
        }
        if (gamepad != null) {
            logInfo("another controller takes over");
            detachGamepad();
        }
        attachGamepad(device);
    }

    /**
     * Buttons and sticks are taken from whichever gamepad they come from. If that is not the
     * device taken for the controller, and it looks no less like one, it is the controller from
     * now on: its motion sensors, motors and light are the ones to use.
     */
    private void noteInputFrom(InputDevice device) {
        if (gamepad != null && device != null && device.getId() == gamepad.getId()) {
            return;
        }
        if (isGamepad(device) && (gamepad == null || rank(device) >= rank(gamepad))) {
            useGamepad(device);
        }
    }

    private void attachGamepad(InputDevice device) {
        gamepad = device;
        logInfo("controller: " + device.getName() + " (vendor " + device.getVendorId()
                + ", product " + device.getProductId() + ", sources 0x"
                + Integer.toHexString(device.getSources()) + ")");

        learnAxes(device);
        nativeSetGamepad(true, device.supportsSource(InputDevice.SOURCE_TOUCHPAD));

        // Every part is optional: what a controller offers depends on it and on the system.
        try {
            SensorManager sensors = motion ? findSensors(device) : null;
            if (sensors != null && device.getVendorId() != VENDOR_SONY) {
                // Which way is up for a motion sensor is each maker's own affair, and only
                // this one's is known here. A gamepad turned the wrong way round in the game
                // is worse than one the hands holding it turn.
                logInfo("this gamepad has motion sensors, which are not used: only for "
                        + "PlayStation controllers is it known which way theirs point");
                sensors = null;
            }
            if (sensors != null) {
                sensors.registerListener(this, sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER),
                        SensorManager.SENSOR_DELAY_FASTEST);
                sensors.registerListener(this, sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE),
                        SensorManager.SENSOR_DELAY_FASTEST);
                controllerSensors = sensors;
            }
        } catch (RuntimeException e) {
            logWarning("no motion sensors", e);
        }
        try {
            VibratorManager vibrators = device.getVibratorManager();
            if (rumble && vibrators.getVibratorIds().length > 0) {
                controllerVibrators = vibrators;
            }
        } catch (RuntimeException e) {
            logWarning("no rumble", e);
        }
        try {
            LightsManager lights = device.getLightsManager();
            for (Light light : lights.getLights()) {
                if (light.hasRgbControl()) {
                    lightBar = light;
                    lightSession = lights.openSession();
                    break;
                }
            }
        } catch (RuntimeException e) {
            logWarning("no light bar", e);
        }
        logInfo("controller offers: motion " + (controllerSensors != null) + ", rumble "
                + (controllerVibrators != null) + ", light bar " + (lightBar != null)
                + ", touchpad " + device.supportsSource(InputDevice.SOURCE_TOUCHPAD)
                + ", analog triggers " + analogTriggers);
        shownFeedback = -1;
    }

    /**
     * The motion sensors of a controller. The system normally presents them as part of the
     * gamepad; where it lists them as a device of their own, that one is found by its maker
     * and model.
     */
    private SensorManager findSensors(InputDevice device) {
        InputManager inputManager = getSystemService(InputManager.class);
        List<InputDevice> candidates = new ArrayList<>();
        candidates.add(device);
        for (int id : inputManager.getInputDeviceIds()) {
            InputDevice other = inputManager.getInputDevice(id);
            if (other != null && other.getId() != device.getId()
                    && other.getVendorId() == device.getVendorId()
                    && other.getProductId() == device.getProductId()) {
                candidates.add(other);
            }
        }
        for (InputDevice candidate : candidates) {
            SensorManager sensors = candidate.getSensorManager();
            if (sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) != null
                    && sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE) != null) {
                return sensors;
            }
        }
        return null;
    }

    private void detachGamepad() {
        if (controllerSensors != null) {
            controllerSensors.unregisterListener(this);
            controllerSensors = null;
        }
        if (controllerVibrators != null) {
            try {
                controllerVibrators.cancel();
            } catch (RuntimeException e) {
                // The controller is gone, and its motors with it.
            }
            controllerVibrators = null;
        }
        if (lightSession != null) {
            try {
                lightSession.close();
            } catch (RuntimeException e) {
                // Likewise.
            }
            lightSession = null;
        }
        lightBar = null;
        gamepad = null;
    }

    /** Carries out what the game last asked of the controller: rumble and light bar colour. */
    private void applyFeedback() {
        long feedback = nativePadFeedback();
        if ((nativeController() & CONTROLLER_WHICH) != CONTROLLER_GAMEPAD) {
            // The game is played with the headset's own controllers, which the native side
            // shakes: a gamepad lying about has no business rattling.
            feedback &= ~0xffffL;
        }
        long now = android.os.SystemClock.uptimeMillis();
        int small = (int) (feedback & 0xff);
        int large = (int) ((feedback >> 8) & 0xff);
        boolean changed = feedback != shownFeedback;

        if (controllerVibrators != null
                && (changed || ((small | large) != 0 && now >= rumbleRenewAt))) {
            try {
                int[] ids = controllerVibrators.getVibratorIds();
                if ((small | large) == 0) {
                    controllerVibrators.cancel();
                } else {
                    // The first motor is the heavy one, as the system lists them.
                    CombinedVibration.ParallelCombination both = CombinedVibration.startParallel();
                    if (ids.length == 1) {
                        both.addVibrator(ids[0], VibrationEffect.createOneShot(RUMBLE_MILLIS,
                                Math.max(small, large)));
                    } else {
                        if (large != 0) {
                            both.addVibrator(ids[0],
                                    VibrationEffect.createOneShot(RUMBLE_MILLIS, large));
                        }
                        if (small != 0) {
                            both.addVibrator(ids[1],
                                    VibrationEffect.createOneShot(RUMBLE_MILLIS, small));
                        }
                    }
                    controllerVibrators.vibrate(both.combine());
                    rumbleRenewAt = now + RUMBLE_RENEW_MILLIS;
                }
            } catch (RuntimeException e) {
                logWarning("rumble failed", e);
                controllerVibrators = null;
            }
        }

        if (changed && lightSession != null && lightBar != null) {
            int red = (int) ((feedback >> 16) & 0xff);
            int green = (int) ((feedback >> 24) & 0xff);
            int blue = (int) ((feedback >> 32) & 0xff);
            // Before the game has chosen a colour the bar keeps the one the system gave it.
            if ((red | green | blue) != 0) {
                try {
                    lightSession.requestLights(new LightsRequest.Builder()
                            .addLight(lightBar, new LightState.Builder()
                                    .setColor(Color.rgb(red, green, blue)).build())
                            .build());
                } catch (RuntimeException e) {
                    logWarning("light bar failed", e);
                    lightSession = null;
                }
            }
        }
        shownFeedback = feedback;
        handler.postDelayed(this::applyFeedback, 16);
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (event.sensor.getType() == Sensor.TYPE_ACCELEROMETER) {
            System.arraycopy(event.values, 0, acceleration, 0, 3);
        } else if (event.sensor.getType() == Sensor.TYPE_GYROSCOPE) {
            nativeMotion(event.values[0], event.values[1], event.values[2], acceleration[0],
                    acceleration[1], acceleration[2]);
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {}

    @Override
    public void onInputDeviceAdded(int deviceId) {
        // A controller that is switched on later takes over from a lesser one found before.
        findGamepad();
        adoptLateSensors();
    }

    /** The motion sensors of a controller may be listed a moment after the controller. */
    private void adoptLateSensors() {
        if (gamepad == null || controllerSensors != null || !motion
                || gamepad.getVendorId() != VENDOR_SONY) {
            return;
        }
        InputDevice device = getSystemService(InputManager.class).getInputDevice(gamepad.getId());
        if (device != null && findSensors(device) != null) {
            detachGamepad();
            attachGamepad(device);
        }
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        if (gamepad != null && gamepad.getId() == deviceId) {
            logInfo("controller gone");
            detachGamepad();
            // Nothing is held down on a controller that is gone.
            keyButtons = 0;
            hatButtons = 0;
            triggerButtons = 0;
            leftX = leftY = rightX = rightY = leftTrigger = rightTrigger = 0.0f;
            padTouchDown = false;
            padTouchClick = false;
            sendInput();
            findGamepad();
            if (gamepad == null) {
                // The headset's own controllers play on, if the player picks them up.
                nativeSetGamepad(false, false);
            }
        }
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
        adoptLateSensors();
    }
}
