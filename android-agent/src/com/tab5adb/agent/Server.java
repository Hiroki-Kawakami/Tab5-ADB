package com.tab5adb.agent;

import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.Build;

import java.io.OutputStream;

/**
 * tab5adb-agent — Android-side companion for Tab5-ADB.
 *
 * Launched via app_process (scrcpy style, no APK), it runs with shell uid and
 * listens on the abstract socket {@code localabstract:tab5adb-agent}, which the
 * Tab5 host reaches over its embedded ADB. This is the Phase 1 skeleton: it just
 * serves a banner so the build + app_process launch + socket path can be verified
 * end to end with standard adb. Screen capture / offload services land later.
 */
public final class Server {
    private static final String SOCKET_NAME = "tab5adb-agent";

    public static void main(String[] args) throws Exception {
        System.out.println("tab5adb-agent: listening on localabstract:" + SOCKET_NAME);

        LocalServerSocket server = new LocalServerSocket(SOCKET_NAME);
        String banner = "tab5adb-agent v0\n"
                + "model=" + Build.MODEL + "\n"
                + "android=" + Build.VERSION.RELEASE + " (sdk " + Build.VERSION.SDK_INT + ")\n";

        while (true) {
            LocalSocket client = server.accept();
            System.out.println("tab5adb-agent: client connected");
            try {
                OutputStream out = client.getOutputStream();
                out.write(banner.getBytes("UTF-8"));
                out.flush();
            } catch (Exception e) {
                System.err.println("tab5adb-agent: " + e);
            } finally {
                client.close();
            }
        }
    }
}
