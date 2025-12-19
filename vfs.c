#define FUSE_USE_VERSION 35

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pwd.h>
#include <time.h>
#include <unistd.h>

#define MAX_USERNAME_SIZE 32

static time_t vfs_create_time;
static char vfs_owner[64] = "unknown";

// Helper: check if shell ends with 'sh'
static int is_valid_shell(const char *shell) {
    if (!shell) return 0;
    size_t len = strlen(shell);
    return len >= 2 && shell[len-2] == 's' && shell[len-1] == 'h';
}

// Helper: parse path like "/alice/id" → username="alice", filename="id"
static int parse_user_path(const char *path, char *username, char *filename) {
    if (path[0] != '/') return 0;
    const char *p = path + 1;
    const char *slash = strchr(p, '/');
    if (!slash || slash == p) return 0;

    size_t ulen = slash - p;
    if (ulen >= MAX_USERNAME_SIZE) return 0;
    strncpy(username, p, ulen);
    username[ulen] = '\0';
    strcpy(filename, slash + 1);
    return 1;
}

// Read content of "info" file
static int generate_info(const char *user, char *buf, size_t size) {
    struct passwd *pw = getpwnam(user);
    if (!pw) return -ENOENT;
    return snprintf(buf, size, 
        "Username: %s\n"
        "UID: %d\n"
        "GID: %d\n"
        "Home: %s\n"
        "Shell: %s\n"
        "%s%s\n",
        pw->pw_name, pw->pw_uid, pw->pw_gid, pw->pw_dir, pw->pw_shell,
        pw->pw_gecos ? "GECOS: " : "", pw->pw_gecos ? pw->pw_gecos : ""
    );
}

// Read system stats
static int generate_system_stats(char *buf, size_t size) {
    char time_str[64];
    ctime_r(&vfs_create_time, time_str);
    time_str[strcspn(time_str, "\n")] = 0;
    return snprintf(buf, size,
        "VFS создан: in-memory (FUSE)\n"
        "Владелец: %s\n"
        "Время создания: %s\n",
        vfs_owner, time_str
    );
}

// === FUSE callbacks ===

static int vfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void) fi;
    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        stbuf->st_mtime = vfs_create_time;
        return 0;
    }

    if (strcmp(path, "/system_stats") == 0) {
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = 256;
        stbuf->st_mtime = vfs_create_time;
        return 0;
    }

    char username[MAX_USERNAME_SIZE], filename[32];
    if (!parse_user_path(path, username, filename)) return -ENOENT;

    struct passwd *pw = getpwnam(username);
    if (!pw || !is_valid_shell(pw->pw_shell)) return -ENOENT;

    if (strcmp(filename, "id") == 0 || strcmp(filename, "home") == 0 ||
        strcmp(filename, "shell") == 0 || strcmp(filename, "info") == 0) {
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = 128;
        stbuf->st_mtime = vfs_create_time;
        return 0;
    }

    if (strcmp(filename, "home_link") == 0) {
        stbuf->st_mode = S_IFLNK | 0777;
        stbuf->st_nlink = 1;
        stbuf->st_size = strlen(pw->pw_dir);
        return 0;
    }

    return -ENOENT;
}

static int vfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    (void) offset; (void) fi; (void) flags;

    if (strcmp(path, "/") != 0) return -ENOENT;

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, "system_stats", NULL, 0, 0);

    struct passwd *pw;
    setpwent();
    while ((pw = getpwent()) != NULL) {
        if (is_valid_shell(pw->pw_shell)) {
            filler(buf, pw->pw_name, NULL, 0, 0);
        }
    }
    endpwent();
    return 0;
}

static int vfs_open(const char *path, struct fuse_file_info *fi) {
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES;
    return 0;
}

static int vfs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi) {
    (void) fi;
    char content[1024];
    int len = 0;

    if (strcmp(path, "/system_stats") == 0) {
        len = generate_system_stats(content, sizeof(content));
    } else {
        char username[MAX_USERNAME_SIZE], filename[32];
        if (!parse_user_path(path, username, filename)) return -ENOENT;

        struct passwd *pw = getpwnam(username);
        if (!pw || !is_valid_shell(pw->pw_shell)) return -ENOENT;

        if (strcmp(filename, "id") == 0) {
            len = snprintf(content, sizeof(content), "%d\n", pw->pw_uid);
        } else if (strcmp(filename, "home") == 0) {
            len = snprintf(content, sizeof(content), "%s\n", pw->pw_dir);
        } else if (strcmp(filename, "shell") == 0) {
            len = snprintf(content, sizeof(content), "%s\n", pw->pw_shell);
        } else if (strcmp(filename, "info") == 0) {
            len = generate_info(username, content, sizeof(content));
        } else {
            return -ENOENT;
        }
    }

    if (offset >= len) return 0;
    if (offset + (off_t)size > (off_t)len) size = len - offset;
    memcpy(buf, content + offset, size);
    return (int)size;
}

static int vfs_readlink(const char *path, char *buf, size_t size) {
    char username[MAX_USERNAME_SIZE], filename[32];
    if (!parse_user_path(path, username, filename) || strcmp(filename, "home_link") != 0)
        return -EINVAL;

    struct passwd *pw = getpwnam(username);
    if (!pw || !is_valid_shell(pw->pw_shell)) return -ENOENT;

    strncpy(buf, pw->pw_dir, size - 1);
    buf[size - 1] = '\0';
    return 0;
}

static struct fuse_operations vfs_oper = {
    .getattr  = vfs_getattr,
    .readdir  = vfs_readdir,
    .open     = vfs_open,
    .read     = vfs_read,
    .readlink = vfs_readlink,
};

int start_fuse_vfs(const char *mountpoint) {
    vfs_create_time = time(NULL);
    char *user = getenv("USER");
    if (user) snprintf(vfs_owner, sizeof(vfs_owner), "%s", user);

    // FUSE args: foreground, read-only, allow_other
    char *argv[] = {(char*)"kubsh", (char*)mountpoint, "-f", "-s", "-o", "ro,allow_other,default_permissions"};
    int argc = 6;

    umask(0);
    return fuse_main(argc, argv, &vfs_oper, NULL);
}
