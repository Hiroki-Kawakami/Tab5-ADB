/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 *
 * Host-side bsp_sd: "mounting" maps the mount point onto a host directory
 * (SIMULATOR_SDCARD_PATH env var, default simulator/sdcard relative to the
 * cwd — run.sh runs from the repo root). App code keeps using plain POSIX
 * file I/O: the libc entry points below are defined in the executable, so
 * statically linked calls resolve here, translate the path, and forward to
 * the real libc via dlsym(RTLD_NEXT).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  /* RTLD_NEXT on glibc */
#endif

#include "bsp_sd.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MOUNT_POINT_MAX 64
#define SD_ROOT_MAX     1024
#define PATH_BUF_MAX    (SD_ROOT_MAX + 256)

static char   g_mount_point[MOUNT_POINT_MAX];
static size_t g_mount_len;  /* 0 = not mounted */
static char   g_sd_root[SD_ROOT_MAX] = "simulator/sdcard";

/* Translate `path` when it is under the mount point; otherwise pass through. */
static const char *redirect(const char *path, char *buf, size_t buf_sz) {
    if (!path || g_mount_len == 0) return path;
    if (strncmp(path, g_mount_point, g_mount_len) != 0) return path;
    char trail = path[g_mount_len];
    if (trail != '\0' && trail != '/') return path;
    snprintf(buf, buf_sz, "%s%s", g_sd_root, path + g_mount_len);
    return buf;
}

esp_err_t bsp_sd_mount(const char *mount_point, const bsp_sd_mount_config_t *config) {
    (void)config;
    if (!mount_point || strlen(mount_point) >= MOUNT_POINT_MAX) return ESP_ERR_INVALID_ARG;
    if (g_mount_len > 0) return ESP_ERR_INVALID_STATE;
    const char *env = getenv("SIMULATOR_SDCARD_PATH");
    if (env && strlen(env) < SD_ROOT_MAX) {
        strcpy(g_sd_root, env);
    }
    strcpy(g_mount_point, mount_point);
    g_mount_len = strlen(g_mount_point);
    fprintf(stderr, "I (BSP_SD) redirecting %s -> %s\n", g_mount_point, g_sd_root);
    return ESP_OK;
}

esp_err_t bsp_sd_unmount(void) {
    if (g_mount_len == 0) return ESP_ERR_INVALID_STATE;
    g_mount_len = 0;
    return ESP_OK;
}

bool bsp_sd_is_mounted(void) {
    return g_mount_len > 0;
}

/* ---- libc overrides: redirect mount-point paths, forward to the real call */

DIR *opendir(const char *name) {
    static DIR *(*real)(const char *) = NULL;
    if (!real) real = (DIR *(*)(const char *))dlsym(RTLD_NEXT, "opendir");
    char buf[PATH_BUF_MAX];
    return real(redirect(name, buf, sizeof(buf)));
}

int open(const char *pathname, int flags, ...) {
    static int (*real)(const char *, int, ...) = NULL;
    if (!real) real = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
    char buf[PATH_BUF_MAX];
    const char *p = redirect(pathname, buf, sizeof(buf));
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode_t mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        return real(p, flags, mode);
    }
    return real(p, flags);
}

FILE *fopen(const char *pathname, const char *mode) {
    static FILE *(*real)(const char *, const char *) = NULL;
    if (!real) real = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen");
    char buf[PATH_BUF_MAX];
    return real(redirect(pathname, buf, sizeof(buf)), mode);
}

int stat(const char *pathname, struct stat *st) {
    static int (*real)(const char *, struct stat *) = NULL;
    if (!real) real = (int (*)(const char *, struct stat *))dlsym(RTLD_NEXT, "stat");
    char buf[PATH_BUF_MAX];
    return real(redirect(pathname, buf, sizeof(buf)), st);
}
