/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import android.app.Application;
import android.content.Context;
import android.content.ContextWrapper;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

// The system Context of an ActivityThread made for this process, which app_process does not set up.
final class SystemContext {
    private static Context context;

    private SystemContext() {
    }

    // The first call needs a thread with a Looper: ActivityThread makes a Handler.
    static synchronized Context get() throws Exception {
        if (context == null) {
            context = create();
        }
        return context;
    }

    private static Context create() throws Exception {
        Class<?> threadClass = Class.forName("android.app.ActivityThread");
        Constructor<?> constructor = threadClass.getDeclaredConstructor();
        constructor.setAccessible(true);
        Object thread = constructor.newInstance();
        field(threadClass, "sCurrentActivityThread").set(null, thread);
        field(threadClass, "mSystemThread").setBoolean(thread, true);
        Method getSystemContext = threadClass.getDeclaredMethod("getSystemContext");
        getSystemContext.setAccessible(true);
        Context system = (Context) getSystemContext.invoke(thread);
        // Loading some drawables calls ActivityThread.currentApplication().getResources(),
        // which throws NullPointerException while the thread has no Application.
        Application application = new Application();
        Method attach = ContextWrapper.class.getDeclaredMethod("attachBaseContext", Context.class);
        attach.setAccessible(true);
        attach.invoke(application, system);
        field(threadClass, "mInitialApplication").set(thread, application);
        return system;
    }

    private static Field field(Class<?> type, String name) throws NoSuchFieldException {
        Field field = type.getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }
}
