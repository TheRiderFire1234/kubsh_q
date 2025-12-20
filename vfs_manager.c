#define FUSE_USE_VERSION 35

#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include "vfs_manager.h"
#include "vfs.h"

char* init_mountpoint() {
    char* home = getenv("HOME");

    if (!home) {
        fprintf(stderr, "\033[1;31mError getting HOME variable\033[0m");
        exit(EXIT_FAILURE);
    }

    size_t length = strlen(home) + 7;
    char* mountpoint = malloc(length);

    if (!mountpoint) {
        fprintf(stderr, "\033[1;31mAllocation error\033[0m");
        exit(EXIT_FAILURE);
    }

    snprintf(mountpoint, length, "%s/users", home);

    if (mkdir(mountpoint, 0755) != 0) {
        if (errno != EEXIST) {
            fprintf(stderr, "\033[1;31mFailed to create mountpoint\033[0m");
            exit(EXIT_FAILURE);
        }
    }

    return mountpoint;
}

void* fuse_thread_function(void* arg) {
    (void) arg;

    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);

    fuse_opt_add_arg(&args, "");
    fuse_opt_add_arg(&args, "-odefault_permissions");
    fuse_opt_add_arg(&args, "-oauto_unmount");

    struct fuse* fuse_instance = fuse_new(&args, &users_operations, sizeof(users_operations), NULL);

    fuse_mount(fuse_instance, "/opt/users");

    fuse_loop(fuse_instance);

    return NULL;
}

void fuse_start() {
    pthread_t fuse_thread;

    pthread_create(&fuse_thread, NULL, fuse_thread_function, NULL);
}
