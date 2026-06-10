package com.tab5adb.agent;

import android.content.Context;
import android.os.Build;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * Input injection over hidden Android APIs — the scrcpy technique.
 *
 * <p>app_process runs with shell uid, which holds INJECT_EVENTS, so
 * {@code InputManager.injectInputEvent()} works with no permission dialog. This
 * is the agent's shared input foundation: keys today (the overlay power / volume
 * / nav buttons) and motion (touch passthrough) / text (keyboard) later all ride
 * the same {@code injectInputEvent} path (protocol.md §4.7 INPUT channel).
 *
 * <p>The framework {@code InputManager} is reached the version-robust way scrcpy
 * settled on: a synthetic system {@link Context} (built off an
 * {@code ActivityThread}) whose {@code getSystemService(INPUT_SERVICE)} returns
 * the real manager — its {@code injectInputEvent(InputEvent, int)} hidden method
 * is stable across Android 10..15, unlike the removed
 * {@code InputManager.getInstance()} / the unstable {@code IInputManager} AIDL.
 */
final class Input {
    private static final int INJECT_MODE_ASYNC = 0;  // InputManager.INJECT_INPUT_EVENT_MODE_ASYNC

    private final Object inputManager;      // android.hardware.input.InputManager
    private final Method injectInputEvent;  // injectInputEvent(InputEvent, int) -> boolean

    private Input(Object inputManager, Method injectInputEvent) {
        this.inputManager = inputManager;
        this.injectInputEvent = injectInputEvent;
    }

    /** Build the injector; throws if the hidden APIs are unreachable. */
    static Input create() throws Exception {
        Context ctx = systemContext();
        Object im = ctx.getSystemService(Context.INPUT_SERVICE);
        if (im == null) throw new IllegalStateException("no INPUT_SERVICE");
        Method m = im.getClass().getMethod("injectInputEvent", InputEvent.class, int.class);
        return new Input(im, m);
    }

    /**
     * Inject one key event on display 0 (the mirrored display). `action` is
     * {@code KeyEvent.ACTION_DOWN}/{@code ACTION_UP}; the event is sourced as a
     * virtual keyboard, matching scrcpy's {@code Device.injectKeyEvent}.
     */
    boolean injectKey(int action, int keyCode, int repeat, int metaState) {
        long now = SystemClock.uptimeMillis();
        KeyEvent ev = new KeyEvent(now, now, action, keyCode, repeat, metaState,
                KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD);
        return inject(ev);
    }

    private boolean inject(InputEvent ev) {
        try {
            return (boolean) injectInputEvent.invoke(inputManager, ev, INJECT_MODE_ASYNC);
        } catch (Exception e) {
            System.err.println("tab5adb-agent: injectInputEvent failed: " + e);
            return false;
        }
    }

    // --- minimal scrcpy Workarounds: a system Context for getSystemService ---
    //
    // app_process has no Context, so build one off a synthetic ActivityThread.
    // This is the subset of scrcpy's Workarounds needed for input injection (the
    // app-info / app-context fills, only relevant to clipboard / content
    // providers, are omitted).
    private static Context systemContext() throws Exception {
        Class<?> atClass = Class.forName("android.app.ActivityThread");
        Constructor<?> atCtor = atClass.getDeclaredConstructor();
        atCtor.setAccessible(true);
        Object at = atCtor.newInstance();

        Field sCurrent = atClass.getDeclaredField("sCurrentActivityThread");
        sCurrent.setAccessible(true);
        sCurrent.set(null, at);

        Field sysThread = atClass.getDeclaredField("mSystemThread");
        sysThread.setAccessible(true);
        sysThread.setBoolean(at, true);

        // Android 12+: getSystemContext() can route through code that wants a
        // ConfigurationController on the ActivityThread (Samsung). Best-effort.
        if (Build.VERSION.SDK_INT >= 31) {
            try {
                Class<?> ccClass = Class.forName("android.app.ConfigurationController");
                Class<?> atiClass = Class.forName("android.app.ActivityThreadInternal");
                Constructor<?> ccCtor = ccClass.getDeclaredConstructor(atiClass);
                ccCtor.setAccessible(true);
                Object cc = ccCtor.newInstance(at);
                Field ccField = atClass.getDeclaredField("mConfigurationController");
                ccField.setAccessible(true);
                ccField.set(at, cc);
            } catch (Throwable ignore) {
                // workaround only — failing here is not fatal for injection
            }
        }

        Method getSystemContext = atClass.getDeclaredMethod("getSystemContext");
        getSystemContext.setAccessible(true);
        return (Context) getSystemContext.invoke(at);
    }
}
