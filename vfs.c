#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>
#include "vfs.h"
#include "command_executor.h"

int can_login(struct passwd* pwd) {
    FILE* shells = fopen("/etc/shells", "r");

    if (!shells) {
        fprintf(stderr, "\033[1;31mCritical filesystem error\033[0m");
        exit(EXIT_FAILURE);
    }

    char line[256];

    while (fgets(line, sizeof(line), shells)) {
        line[strcspn(line, "\n")] = '\0';

        if (strcmp(line, pwd->pw_shell) == 0) {
            fclose(shells);
            return 1;
        }
    }

    fclose(shells);
    return 0;
}

int users_getattr(const char* path, struct stat* st, struct fuse_file_info* fi) {
    (void) fi;

    time_t now = time(NULL);

    memset(st, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        st->st_mode = __S_IFDIR | 0755;
        st->st_nlink = 2;
        st->st_uid = getuid();
        st->st_gid = getgid();
        st->st_size = 4096;
        st->st_blksize = 4096;
        st->st_blocks = 8;
        st->st_atime = now;
        st->st_mtime = now;
        st->st_ctime = now;

        return 0;
    }

    char username[256];
    char filename[256];

    if (sscanf(path, "/%255[^/]/%255[^/]", username, filename) == 2) {
        if (
            strcmp(filename, "id") == 0
            || strcmp(filename, "home") == 0
            || strcmp(filename, "shell") == 0
        ) {
            struct passwd* pwd = getpwnam(username);

            if (pwd && can_login(pwd)) {
                st->st_mode = __S_IFREG | 0644;
                st->st_nlink = 1;
                st->st_uid = pwd->pw_uid;
                st->st_gid = pwd->pw_gid;
                st->st_atime = now;
                st->st_mtime = now;
                st->st_ctime = now;

                if (strcmp(filename, "id") == 0) {
                    st->st_size = snprintf(NULL, 0, "%d", pwd->pw_uid);
                }
                else if (strcmp(filename, "home") == 0) {
                    st->st_size = strlen(pwd->pw_dir);
                }
                else {
                    st->st_size = strlen(pwd->pw_shell);
                }

                st->st_blksize = 4096;
                st->st_blocks = (st->st_size + 511) / 512;

                return 0;
            }
        }
        
        return -ENOENT;
    }

    if (sscanf(path, "/%255[^/]", username) == 1) {
        struct passwd* pwd = getpwnam(username);

        if (pwd && can_login(pwd)) {
            st->st_mode = __S_IFDIR | 0755;
            st->st_nlink = 2;
            st->st_uid = pwd->pw_uid;
            st->st_gid = pwd->pw_gid;
            st->st_size = 4096;
            st->st_blksize = 4096;
            st->st_blocks = 8;

            return 0;
        }
    }

    return -ENOENT;
}

int users_readdir(
    const char* path,
    void* buf, 
    fuse_fill_dir_t filler, 
    off_t offset, 
    struct fuse_file_info* fi, 
    enum fuse_readdir_flags flags
) {
    (void) offset;
    (void) fi;
    (void) flags;

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    if (strcmp(path, "/") == 0) {
        struct passwd* pwd;
        setpwent();

        while ((pwd = getpwent()) != NULL) {
            if (can_login(pwd)) {
                filler(buf, pwd->pw_name, NULL, 0, 0);
            }
        }

        endpwent();
        return 0;
    }

    filler(buf, "id", NULL, 0, 0);
    filler(buf, "home", NULL, 0, 0);
    filler(buf, "shell", NULL, 0, 0);
    
    return 0;
}

int users_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    (void) fi;

    char username[256];
    char filename[256];

    sscanf(path, "/%255[^/]/%255[^/]", username, filename);

    struct passwd* pwd = getpwnam(username);
    
    char content[256];
    content[0] = '\0';

    if (strcmp(filename, "id") == 0) {
        snprintf(content, sizeof(content), "%d", pwd->pw_uid);
    }
    else if (strcmp(filename, "home") == 0) {
        snprintf(content, sizeof(content), "%s", pwd->pw_dir);
    }
    else {
        snprintf(content, sizeof(content), "%s", pwd->pw_shell);
    }

    size_t len = strlen(content);
    if (len > 0 && content[len-1] == '\n') {
        content[len-1] = '\0';
        len--;
    }

    if (offset >= len) {
        return 0;
    }

    if (offset + size > len) {
        size = len - offset;
    }

    memcpy(buf, content + offset, size);

    return size;
}

int users_mkdir(const char* path, mode_t mode) {
    (void) mode;

    char username[256];

    if (sscanf(path, "/%255[^/]", username) == 1) {
        struct passwd* pwd = getpwnam(username);
        
        if (pwd != NULL) {
            return -EEXIST;
        }

        char* adduser_command[] = {
            "adduser",
            "--disabled-password",
            "--gecos",
            "",
            username,
            NULL
        };

        execute_command(adduser_command);

        pwd = getpwnam(username);

        return (pwd != NULL) ? 0 : -EIO;
    }

    return -EINVAL;
}

int users_rmdir(const char* path) {
    char username[256];

    if (sscanf(path, "/%255[^/]", username) == 1) {
        if (strchr(path + 1, '/') == NULL) {
            struct passwd* pwd= getpwnam(username);

            if (pwd != NULL) {
                char* userdel_command[] = {
                    "userdel",
                    "--remove",
                    username,
                    NULL
                };

                execute_command(userdel_command);

                pwd = getpwnam(username);

                if (pwd == NULL) {
                    return 0;
                }

                return -EIO;
            }

            return -ENOENT;
        }
        
        return -EPERM;
    }

    return -EPERM;
}

struct fuse_operations users_operations = {
    .getattr = users_getattr,
    .readdir = users_readdir,
    .read = users_read,
    .mkdir = users_mkdir,
    .rmdir = users_rmdir
};
