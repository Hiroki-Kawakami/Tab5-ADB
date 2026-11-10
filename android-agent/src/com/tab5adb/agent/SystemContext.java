/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.app.Application;
import android.content.AttributionSource;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;
import android.os.Process;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

// The system Context of an ActivityThread made for this process, which app_process does not set up.
final class SystemContext {
    private static final String SHELL_PACKAGE = "com.android.shell";

    private static Context system;
    private static Context shell;

    private SystemContext() {
    }

    // The first call needs a thread with a Looper: ActivityThread makes a Handler.
    static synchronized Context get() throws Exception {
        if (system == null) {
            create();
        }
        return system;
    }

    // The same, presented as the shell package, as AudioRecord needs.
    static synchronized Context shell() throws Exception {
        get();
        return shell;
    }

    private static void create() throws Exception {
        Class<?> threadClass = Class.forName("android.app.ActivityThread");
        Constructor<?> constructor = threadClass.getDeclaredConstructor();
        constructor.setAccessible(true);
        Object thread = constructor.newInstance();
        field(threadClass, "sCurrentActivityThread").set(null, thread);
        field(threadClass, "mSystemThread").setBoolean(thread, true);
        Method getSystemContext = threadClass.getDeclaredMethod("getSystemContext");
        getSystemContext.setAccessible(true);
        system = (Context) getSystemContext.invoke(thread);
        shell = new Shell(system);
        // An AudioPolicy's recording is made from ActivityThread.currentApplication() and the
        // bound app's package, not from the Context it was given; AudioFlinger refuses it unless
        // both say the shell package. Loading some drawables also throws without an Application.
        Class<?> bindClass = Class.forName("android.app.ActivityThread$AppBindData");
        Constructor<?> bindConstructor = bindClass.getDeclaredConstructor();
        bindConstructor.setAccessible(true);
        Object bind = bindConstructor.newInstance();
        ApplicationInfo info = new ApplicationInfo();
        info.packageName = SHELL_PACKAGE;
        field(bindClass, "appInfo").set(bind, info);
        field(threadClass, "mBoundApplication").set(thread, bind);
        Application application = new Application();
        Method attach = ContextWrapper.class.getDeclaredMethod("attachBaseContext", Context.class);
        attach.setAccessible(true);
        attach.invoke(application, shell);
        field(threadClass, "mInitialApplication").set(thread, application);
    }

    private static Field field(Class<?> type, String name) throws NoSuchFieldException {
        Field field = type.getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }

    private static final class Shell extends ContextWrapper {
        Shell(Context base) {
            super(base);
        }

        @Override
        public String getPackageName() {
            return SHELL_PACKAGE;
        }

        @Override
        public String getOpPackageName() {
            return SHELL_PACKAGE;
        }

        @Override
        public AttributionSource getAttributionSource() {
            return new AttributionSource.Builder(Process.SHELL_UID).setPackageName(SHELL_PACKAGE).build();
        }

        @Override
        public Context getApplicationContext() {
            return this;
        }
    }
}
