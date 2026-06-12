package com.tab5adb.agent;

import android.content.Context;
import android.os.Build;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/**
 * A synthetic system {@link Context} for app_process — the minimal scrcpy
 * "Workarounds": app_process has no Context, so build one off a synthetic
 * {@code ActivityThread}. Shared by every consumer of framework services
 * ({@link Input}'s InputManager, {@link AppInfo}'s PackageManager).
 *
 * <p>Must first be reached from a thread with a Looper (the main thread —
 * {@code new ActivityThread()} creates a Handler bound to the current thread),
 * which {@code Server.main} guarantees by building the service singletons at
 * startup; later callers get the cached instance.
 */
final class SystemContext {
    private static Context instance;

    private SystemContext() {}

    static synchronized Context get() throws Exception {
        if (instance == null) instance = create();
        return instance;
    }

    private static Context create() throws Exception {
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
                // workaround only — failing here is not fatal
            }
        }

        Method getSystemContext = atClass.getDeclaredMethod("getSystemContext");
        getSystemContext.setAccessible(true);
        Context ctx = (Context) getSystemContext.invoke(at);

        // scrcpy's fillAppContext: framework code reached during resource/drawable
        // loading calls ActivityThread.currentApplication().getResources(), which
        // NPEs while mInitialApplication is null (seen inflating launcher-icon
        // XML for GET_APP_ICON). Install a bare Application wrapping the system
        // context. Best-effort.
        try {
            android.app.Application app = new android.app.Application();
            Field base = android.content.ContextWrapper.class.getDeclaredField("mBase");
            base.setAccessible(true);
            base.set(app, ctx);
            Field initialApp = atClass.getDeclaredField("mInitialApplication");
            initialApp.setAccessible(true);
            initialApp.set(at, app);
        } catch (Throwable ignore) {
            // workaround only — without it some app icons fall back to EINVAL
        }
        return ctx;
    }
}
