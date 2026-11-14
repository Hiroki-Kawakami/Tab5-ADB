/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

package com.tab5adb.agent;

import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;

// The rotation and the forced display size, which only the agent holding the
// lock on the state file may change. The file records how to put them back.
final class DisplayClaim {
    private static final String PATH = "/data/local/tmp/tab5adb-agent-display.state";

    private static String owner;
    private static RandomAccessFile file;
    private static FileLock lock;

    private DisplayClaim() {
    }

    // Puts back what a dead agent left, unless a live one holds the claim.
    static synchronized void recover(String id) {
        owner = id;
        try {
            if (claim()) {
                save();
            }
        } catch (IOException e) {
            System.out.println("display claim: " + e);
        }
    }

    // false while another agent holds the claim.
    static synchronized boolean claim() throws IOException {
        if (lock != null) {
            return true;
        }
        RandomAccessFile f = new RandomAccessFile(PATH, "rw");
        FileLock l;
        try {
            l = f.getChannel().tryLock();
        } catch (IOException e) {
            f.close();
            throw e;
        }
        if (l == null) {
            f.close();
            return false;
        }
        file = f;
        lock = l;
        byte[] left = new byte[(int) f.length()];
        f.readFully(left);
        if (left.length > 0) {
            restore(new String(left, StandardCharsets.UTF_8));
            f.setLength(0);
        }
        return true;
    }

    // Records what is held now, and gives the claim up when nothing is.
    static synchronized void save() {
        if (lock == null) {
            return;
        }
        try {
            file.setLength(0);
            if (!RotationLock.held() && !DisplaySize.held()) {
                lock.release();
                file.close();
                lock = null;
                file = null;
                return;
            }
            String state = owner + " " + (RotationLock.held() ? 1 : 0) + " " + (RotationLock.wasFrozen() ? 1 : 0) + " " + RotationLock.wasRotation()
                    + " " + (DisplaySize.held() ? 1 : 0) + "\n";
            file.seek(0);
            file.write(state.getBytes(StandardCharsets.UTF_8));
            file.getFD().sync();
        } catch (IOException e) {
            System.out.println("display claim: " + e);
        }
    }

    private static void restore(String state) {
        String[] f = state.trim().split(" ");
        if (f.length < 5 || !f[3].matches("[0-3]")) {
            System.out.println("display claim: bad state " + state.trim());
            return;
        }
        System.out.println("display: restoring what " + f[0] + " left");
        if (f[4].equals("1")) {
            DisplaySize.clear();
        }
        if (f[1].equals("1")) {
            RotationLock.restore(f[2].equals("1"), Integer.parseInt(f[3]));
        }
    }
}
