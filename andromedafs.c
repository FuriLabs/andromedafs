/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#define FUSE_USE_VERSION 35

#include <fuse.h>

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

struct afs_config {
    char *backing;
    uid_t host_uid;
    gid_t host_gid;
    int debug_enabled;
    char *disk_side_str;
    int disk_side;
};

static struct afs_config g_cfg;

#define ANDROID_UID 1023
#define ANDROID_GID 1023

#define ANDROID_CREATED_FILE_MODE 0664
#define ANDROID_CREATED_DIR_MODE 0775

static ino_t g_host_mntns_ino;

enum {
    DISK_SIDE_HOST = 0,
    DISK_SIDE_ANDROID = 1,
};

static void
debug(const char *fmt, ...)
{
    va_list ap;

    if (!g_cfg.debug_enabled)
        return;

    fprintf(stderr, "[andromedafs] ");

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n");
}

static ino_t
get_mntns_ino_of_pid(pid_t pid)
{
    char p[64];
    struct stat st;

    snprintf(p, sizeof(p), "/proc/%d/ns/mnt", pid);

    if (stat(p, &st) != 0)
        return 0;

    return st.st_ino;
}

static int
is_android_caller(void)
{
    const struct fuse_context *ctx;
    ino_t ino;

    ctx = fuse_get_context();
    if (!ctx)
        return 0;

    ino = get_mntns_ino_of_pid(ctx->pid);
    if (ino == 0)
        return 0;

    if (ino != g_host_mntns_ino)
        return 1;

    return 0;
}

static void
make_fullpath(char out[PATH_MAX], const char *path)
{
    if (!path || !out)
        return;

    if (!g_cfg.backing || g_cfg.backing[0] == '\0') {
        snprintf(out, PATH_MAX, "%s", path);
        return;
    }

    if (strcmp(path, "/") == 0) {
        snprintf(out, PATH_MAX, "%s", g_cfg.backing);
        return;
    }

    if (g_cfg.backing[strlen(g_cfg.backing) - 1] == '/')
        snprintf(out, PATH_MAX, "%s%s", g_cfg.backing, path[0] == '/' ? path + 1 : path);
    else
        snprintf(out, PATH_MAX, "%s%s", g_cfg.backing, path);
}

static mode_t
android_view_mode(mode_t real_mode)
{
    mode_t type;
    mode_t exec_bits;

    type = real_mode & S_IFMT;
    exec_bits = 0;

    if (type == S_IFDIR) {
        exec_bits = S_IXUSR | S_IXGRP | S_IXOTH;
        return type |
               (S_IRUSR | S_IWUSR | exec_bits) |
               (S_IRGRP | S_IWGRP | exec_bits) |
               (S_IROTH | S_IWOTH | exec_bits);
    }

    if (real_mode & (S_IXUSR | S_IXGRP | S_IXOTH))
        exec_bits = (S_IXUSR | S_IXGRP | S_IXOTH);

    return type |
           (S_IRUSR | S_IWUSR | exec_bits) |
           (S_IRGRP | S_IWGRP | exec_bits) |
           (S_IROTH | S_IWOTH | exec_bits);
}

static int
should_fix_created_node(void)
{
    int android = is_android_caller();

    if (g_cfg.disk_side == DISK_SIDE_HOST)
        return android ? 1 : 0;

    return android ? 0 : 1;
}

static void
fix_ownership_and_mode(const char *full, int is_dir)
{
    mode_t desired;
    uid_t uid;
    gid_t gid;

    if (!should_fix_created_node())
        return;

    if (g_cfg.disk_side == DISK_SIDE_HOST) {
        uid = g_cfg.host_uid;
        gid = g_cfg.host_gid;
    } else {
        uid = ANDROID_UID;
        gid = ANDROID_GID;
    }

    if (lchown(full, uid, gid) == -1) {
        debug("lchown failed: %s uid=%u gid=%u (%s)",
              full, (unsigned)uid, (unsigned)gid, strerror(errno));
        return;
    }

    desired = is_dir ? ANDROID_CREATED_DIR_MODE : ANDROID_CREATED_FILE_MODE;

    if (chmod(full, desired) == -1) {
        debug("chmod failed: %s mode=%o (%s)", full, desired, strerror(errno));
        return;
    }
}

static void
fix_ownership_only(const char *full)
{
    uid_t uid;
    gid_t gid;

    if (!should_fix_created_node())
        return;

    if (g_cfg.disk_side == DISK_SIDE_HOST) {
        uid = g_cfg.host_uid;
        gid = g_cfg.host_gid;
    } else {
        uid = ANDROID_UID;
        gid = ANDROID_GID;
    }

    if (lchown(full, uid, gid) == -1)
        debug("lchown failed: %s uid=%u gid=%u (%s)",
              full, (unsigned)uid, (unsigned)gid, strerror(errno));
}

static int
afs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    char full[PATH_MAX];
    int res;
    int android;

    (void)fi;

    make_fullpath(full, path);
    res = lstat(full, stbuf);

    if (res == -1)
        return -errno;

    android = is_android_caller();

    if (android) {
        stbuf->st_uid = ANDROID_UID;
        stbuf->st_gid = ANDROID_GID;
        stbuf->st_mode = android_view_mode(stbuf->st_mode);
    } else {
        stbuf->st_uid = g_cfg.host_uid;
        stbuf->st_gid = g_cfg.host_gid;
    }

    return 0;
}

static int
afs_access(const char *path, int mask)
{
    char full[PATH_MAX];
    int res;

    if (is_android_caller())
        return 0;

    make_fullpath(full, path);
    res = access(full, mask);

    if (res == -1)
        return -errno;

    return 0;
}

static int
afs_readlink(const char *path, char *buf, size_t size)
{
    char full[PATH_MAX];
    ssize_t res;

    make_fullpath(full, path);
    res = readlink(full, buf, size - 1);

    if (res == -1)
        return -errno;

    buf[res] = '\0';
    return 0;
}

static int
afs_readdir(const char *path,
            void *buf,
            fuse_fill_dir_t filler,
            off_t offset,
            struct fuse_file_info *fi,
            enum fuse_readdir_flags flags)
{
    char full[PATH_MAX];
    DIR *dp;
    struct dirent *de;

    (void)offset;
    (void)fi;
    (void)flags;

    make_fullpath(full, path);

    dp = opendir(full);
    if (!dp)
        return -errno;

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    while ((de = readdir(dp)) != NULL) {
        if (filler(buf, de->d_name, NULL, 0, 0) != 0)
            break;
    }

    closedir(dp);
    return 0;
}

static int
afs_mknod(const char *path, mode_t mode, dev_t rdev)
{
    char full[PATH_MAX];
    int res;
    int fd;

    make_fullpath(full, path);

    if (S_ISREG(mode)) {
        fd = open(full, O_CREAT | O_EXCL | O_WRONLY, mode);
        if (fd == -1)
            return -errno;

        res = close(fd);
        if (res == -1)
            return -errno;

        fix_ownership_and_mode(full, 0);
        return 0;
    }

    if (S_ISFIFO(mode)) {
        res = mkfifo(full, mode);
        if (res == -1)
            return -errno;

        fix_ownership_and_mode(full, 0);
        return 0;
    }

    res = mknod(full, mode, rdev);
    if (res == -1)
        return -errno;

    fix_ownership_and_mode(full, 0);
    return 0;
}

static int
afs_mkdir(const char *path, mode_t mode)
{
    char full[PATH_MAX];
    int res;

    make_fullpath(full, path);
    res = mkdir(full, mode);

    if (res == -1)
        return -errno;

    fix_ownership_and_mode(full, 1);
    return 0;
}

static int
afs_unlink(const char *path)
{
    char full[PATH_MAX];
    int res;

    make_fullpath(full, path);
    res = unlink(full);

    if (res == -1)
        return -errno;

    return 0;
}

static int
afs_rmdir(const char *path)
{
    char full[PATH_MAX];
    int res;

    make_fullpath(full, path);
    res = rmdir(full);

    if (res == -1)
        return -errno;

    return 0;
}

static int
afs_symlink(const char *from, const char *to)
{
    char full_to[PATH_MAX];
    int res;

    make_fullpath(full_to, to);
    res = symlink(from, full_to);

    if (res == -1)
        return -errno;

    fix_ownership_only(full_to);

    return 0;
}

static int
afs_rename(const char *from, const char *to, unsigned int flags)
{
    char full_from[PATH_MAX];
    char full_to[PATH_MAX];
    int res;

    if (flags != 0)
        return -EINVAL;

    make_fullpath(full_from, from);
    make_fullpath(full_to, to);

    res = rename(full_from, full_to);
    if (res == -1)
        return -errno;

    return 0;
}

static int
afs_link(const char *from, const char *to)
{
    char full_from[PATH_MAX];
    char full_to[PATH_MAX];
    int res;

    make_fullpath(full_from, from);
    make_fullpath(full_to, to);

    res = link(full_from, full_to);
    if (res == -1)
        return -errno;

    return 0;
}

static int
afs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    char full[PATH_MAX];
    int res;

    (void)fi;

    if (is_android_caller()) {
        debug("chmod no-op for android: %s mode=%o", path, mode);
        return 0;
    }

    make_fullpath(full, path);
    res = chmod(full, mode);

    if (res == -1)
        return -errno;

    return 0;
}

static int
afs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
    char full[PATH_MAX];
    int res;

    (void)fi;

    if (is_android_caller()) {
        debug("chown no-op for android: %s uid=%u gid=%u", path, (unsigned)uid, (unsigned)gid);
        return 0;
    }

    make_fullpath(full, path);
    res = lchown(full, uid, gid);

    if (res == -1)
        return -errno;

    return 0;
}

static int
afs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    char full[PATH_MAX];
    int res;

    (void)fi;

    make_fullpath(full, path);
    res = truncate(full, size);

    if (res == -1)
        return -errno;

    return 0;
}

static int
afs_open(const char *path, struct fuse_file_info *fi)
{
    char full[PATH_MAX];
    int fd;

    make_fullpath(full, path);
    fd = open(full, fi->flags);

    if (fd == -1)
        return -errno;

    fi->fh = (uint64_t)fd;
    return 0;
}

static int
afs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    char full[PATH_MAX];
    int fd;

    make_fullpath(full, path);
    fd = open(full, fi->flags | O_CREAT, mode);

    if (fd == -1)
        return -errno;

    fix_ownership_and_mode(full, 0);

    fi->fh = (uint64_t)fd;
    return 0;
}

static int
afs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    ssize_t res;
    int fd;

    (void)path;

    fd = (int)fi->fh;
    res = pread(fd, buf, size, offset);

    if (res == -1)
        return -errno;

    return (int)res;
}

static int
afs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    ssize_t res;
    int fd;

    (void)path;

    fd = (int)fi->fh;
    res = pwrite(fd, buf, size, offset);

    if (res == -1)
        return -errno;

    return (int)res;
}

static int
afs_release(const char *path, struct fuse_file_info *fi)
{
    int fd;

    (void)path;

    fd = (int)fi->fh;
    close(fd);

    return 0;
}

static int
afs_fsync(const char *path, int isdatasync, struct fuse_file_info *fi)
{
    int fd;
    int res;

    (void)path;

    fd = (int)fi->fh;

    if (isdatasync)
        res = fdatasync(fd);
    else
        res = fsync(fd);

    if (res == -1)
        return -errno;

    return 0;
}

static int
afs_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
    char full[PATH_MAX];
    int res;

    (void)fi;

    make_fullpath(full, path);
    res = utimensat(AT_FDCWD, full, tv, AT_SYMLINK_NOFOLLOW);

    if (res == -1)
        return -errno;

    return 0;
}

static const struct fuse_operations afs_ops = {
    .getattr = afs_getattr,
    .access = afs_access,
    .readlink = afs_readlink,
    .readdir = afs_readdir,
    .mknod = afs_mknod,
    .mkdir = afs_mkdir,
    .symlink = afs_symlink,
    .unlink = afs_unlink,
    .rmdir = afs_rmdir,
    .rename = afs_rename,
    .link = afs_link,
    .chmod = afs_chmod,
    .chown = afs_chown,
    .truncate = afs_truncate,
    .open = afs_open,
    .create = afs_create,
    .read = afs_read,
    .write = afs_write,
    .release = afs_release,
    .fsync = afs_fsync,
    .utimens = afs_utimens,
};

enum {
    KEY_HELP = 1,
};

#define AFS_OPT(t, p, v) { t, offsetof(struct afs_config, p), v }

static const struct fuse_opt afs_opts[] = {
    AFS_OPT("backing=%s", backing, 0),
    AFS_OPT("host_uid=%u", host_uid, 0),
    AFS_OPT("host_gid=%u", host_gid, 0),
    AFS_OPT("afs_debug=%u", debug_enabled, 0),
    AFS_OPT("disk_side=%s", disk_side_str, 0),

    FUSE_OPT_KEY("-h", KEY_HELP),
    FUSE_OPT_KEY("--help", KEY_HELP),

    FUSE_OPT_END
};

static int
afs_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
    (void)data;
    (void)arg;
    (void)outargs;

    if (key == KEY_HELP)
        return 1;

    return 1;
}

static void
usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s <mountpoint> -o backing=/abs/path[,disk_side=host|android,host_uid=1000,host_gid=1000,afs_debug=0]\n"
            "    (optional) -o allow_other\n",
            prog);
}

static int
parse_disk_side(void)
{
    const char *s = g_cfg.disk_side_str;

    if (!s || s[0] == '\0')
        s = "host";

    if (strcmp(s, "host") == 0) {
        g_cfg.disk_side = DISK_SIDE_HOST;
        return 0;
    }

    if (strcmp(s, "android") == 0) {
        g_cfg.disk_side = DISK_SIDE_ANDROID;
        return 0;
    }

    fprintf(stderr, "Invalid disk_side: %s (expected host or android)\n", s);
    return -1;
}

int
main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct stat st;
    int ret;

    if (geteuid() != 0) {
        fprintf(stderr, "andromedafs must be run as root (geteuid=%u)\n", (unsigned)geteuid());
        return 1;
    }

    memset(&g_cfg, 0, sizeof(g_cfg));

    g_cfg.backing = NULL;
    g_cfg.host_uid = getuid();
    g_cfg.host_gid = getgid();
    g_cfg.debug_enabled = 0;

    g_cfg.disk_side_str = NULL;
    g_cfg.disk_side = DISK_SIDE_HOST;

    g_host_mntns_ino = get_mntns_ino_of_pid(getpid());

    if (fuse_opt_parse(&args, &g_cfg, afs_opts, afs_opt_proc) == -1)
        return 1;

    if (g_cfg.backing == NULL || g_cfg.backing[0] == '\0') {
        usage(args.argv[0]);
        fuse_opt_free_args(&args);
        return 1;
    }

    if (parse_disk_side() != 0) {
        usage(args.argv[0]);
        fuse_opt_free_args(&args);
        free(g_cfg.backing);
        free(g_cfg.disk_side_str);
        return 1;
    }

    if (stat(g_cfg.backing, &st) != 0) {
        fprintf(stderr, "Backing path not accessible: %s (%s)\n", g_cfg.backing, strerror(errno));
        fuse_opt_free_args(&args);
        free(g_cfg.backing);
        free(g_cfg.disk_side_str);
        return 1;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Backing path is not a directory: %s\n", g_cfg.backing);
        fuse_opt_free_args(&args);
        free(g_cfg.backing);
        free(g_cfg.disk_side_str);
        return 1;
    }

    debug("backing=%s host_uid=%u host_gid=%u android_uid=%u android_gid=%u host_mntns_ino=%lu disk_side=%s",
          g_cfg.backing,
          (unsigned)g_cfg.host_uid,
          (unsigned)g_cfg.host_gid,
          (unsigned)ANDROID_UID,
          (unsigned)ANDROID_GID,
          (unsigned long)g_host_mntns_ino,
          (g_cfg.disk_side == DISK_SIDE_ANDROID) ? "android" : "host");

    ret = fuse_main(args.argc, args.argv, &afs_ops, NULL);

    fuse_opt_free_args(&args);
    free(g_cfg.backing);
    free(g_cfg.disk_side_str);

    return ret;
}
