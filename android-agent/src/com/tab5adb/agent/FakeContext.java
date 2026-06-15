/*
 * Portions of this file are derived from scrcpy (FakeContext.java — the
 * shell-package Context whose AttributionSource lets AudioRecord initialize on
 * Android 12+):
 *
 *   Copyright (C) 2018 Genymobile
 *   Copyright (C) 2018-2025 Romain Vimont
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.tab5adb.agent;

import android.content.AttributionSource;
import android.content.Context;
import android.content.ContextWrapper;
import android.os.Process;

/**
 * A {@link Context} that reports the shell package ({@code com.android.shell},
 * uid 2000) — the scrcpy "FakeContext" workaround. {@link android.media.AudioRecord}
 * on Android 12+ (API 31) attributes its {@code CAPTURE_AUDIO_OUTPUT} appop to the
 * caller's {@link AttributionSource}; app_process has none, so without this the
 * builder yields a {@code STATE_UNINITIALIZED} recorder on newer devices (the
 * REMOTE_SUBMIX capture silently failing on a Pixel 10 / Android 16). Wrapping the
 * synthetic {@link SystemContext} and overriding the package / attribution makes the
 * appop resolve against shell (which holds the permission), so capture initialises.
 */
final class FakeContext extends ContextWrapper {
    static final String PACKAGE_NAME = "com.android.shell";

    private static FakeContext instance;

    static synchronized FakeContext get() throws Exception {
        if (instance == null) {
            instance = new FakeContext(SystemContext.get());
            installAppEnvironment(instance);
        }
        return instance;
    }

    /**
     * scrcpy's {@code Workarounds.fillAppInfo()} + {@code fillAppContext()}: make
     * the process's {@code ActivityThread} report the shell package as both its
     * bound app-info package and its initial {@link android.app.Application} (whose
     * base is this FakeContext). This matters for audio playback capture on Android
     * 16: {@code AudioPolicy.createAudioRecordSink()} builds its internal
     * {@code AudioRecord} from {@code ActivityThread.currentApplication()} (NOT the
     * policy's context), so without this the record is attributed to package
     * "android" and AudioFlinger rejects it (uid 2000 ≠ "android" →
     * {@code createRecord ... EX_SECURITY: invalid attr}). Best-effort.
     */
    private static void installAppEnvironment(FakeContext ctx) {
        Object at;
        Class<?> atClass;
        try {
            atClass = Class.forName("android.app.ActivityThread");
            at = atClass.getMethod("currentActivityThread").invoke(null);
            if (at == null) return;
        } catch (Throwable t) {
            return;
        }
        // mBoundApplication.appInfo.packageName = com.android.shell
        try {
            Class<?> abdClass = Class.forName("android.app.ActivityThread$AppBindData");
            java.lang.reflect.Constructor<?> abdCtor = abdClass.getDeclaredConstructor();
            abdCtor.setAccessible(true);
            Object abd = abdCtor.newInstance();
            android.content.pm.ApplicationInfo ai = new android.content.pm.ApplicationInfo();
            ai.packageName = PACKAGE_NAME;
            java.lang.reflect.Field appInfoF = abdClass.getDeclaredField("appInfo");
            appInfoF.setAccessible(true);
            appInfoF.set(abd, ai);
            java.lang.reflect.Field mBoundF = atClass.getDeclaredField("mBoundApplication");
            mBoundF.setAccessible(true);
            mBoundF.set(at, abd);
        } catch (Throwable ignore) {
            // best-effort
        }
        // mInitialApplication = Instrumentation.newApplication(Application.class, FakeContext)
        try {
            android.app.Application app = android.app.Instrumentation.newApplication(
                    android.app.Application.class, ctx);
            java.lang.reflect.Field miaF = atClass.getDeclaredField("mInitialApplication");
            miaF.setAccessible(true);
            miaF.set(at, app);
        } catch (Throwable ignore) {
            // best-effort (SystemContext also installs a fallback Application)
        }
    }

    private FakeContext(Context base) {
        super(base);
    }

    @Override
    public String getPackageName() {
        return PACKAGE_NAME;
    }

    @Override
    public String getOpPackageName() {
        return PACKAGE_NAME;
    }

    @Override
    public AttributionSource getAttributionSource() {
        AttributionSource.Builder builder = new AttributionSource.Builder(Process.SHELL_UID);
        // The AudioFlinger NativePermissionController validates (uid, package): for
        // the shell uid it accepts "com.android.shell" on Android 16 release (an
        // early-Android-16 build wanted bare "shell" — scrcpy c27d116a — but that was
        // reverted as it broke beta 4; verified com.android.shell on a Pixel 10).
        builder.setPackageName(PACKAGE_NAME);
        return builder.build();
    }

    @Override
    public Context getApplicationContext() {
        return this;
    }
}
