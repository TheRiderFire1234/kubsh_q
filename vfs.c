#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>

#define MAX_INPUT_SIZE 1024
#define MAX_HISTORY_SIZE 100
#define MAX_USERNAME_SIZE 32
#define HISTORY_FILE ".kubsh_history"

// Для тестов kubsh в Docker VFS должен быть в /opt/users (запуск под root),
// но для обычного пользователя на хосте создаём VFS в $HOME/users.
#define USERS_DIR_ROOT "/opt/users"
#define USERS_DIR_HOME "users"

static char *vfs_root_path = NULL;

// Получение пути к VFS
char* get_users_dir_path() {
    static char path[1024];

    if (vfs_root_path) {
        return vfs_root_path;
    }

    if (geteuid() == 0) {
        snprintf(path, sizeof(path), "%s", USERS_DIR_ROOT);
    } else {
        char *home = getenv("HOME");
        if (home == NULL) {
            struct passwd *pw = getpwuid(getuid());
            if (pw) home = pw->pw_dir;
            else home = "/tmp";
        }
        snprintf(path, sizeof(path), "%s/%s", home, USERS_DIR_HOME);
    }

    return path;
}

// Установка пути VFS
void vfs_set_root_path(const char *path) {
    if (vfs_root_path) {
        free(vfs_root_path);
    }
    vfs_root_path = strdup(path);
}

// Получение полного пути в VFS
static void get_full_path(char *full_path, const char *path) {
    char *users_dir = get_users_dir_path();
    if (strcmp(path, "/") == 0) {
        strcpy(full_path, users_dir);
    } else {
        snprintf(full_path, 1024, "%s%s", users_dir, path);
    }
}

// Получение информации о файле
static int vfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void)fi;
    char full_path[1024];
    get_full_path(full_path, path);
    
    int res = lstat(full_path, stbuf);
    if (res == -1) {
        return -errno;
    }
    
    return 0;
}

// Чтение директории
static int vfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;
    
    char full_path[1024];
    get_full_path(full_path, path);
    
    DIR *dp = opendir(full_path);
    if (!dp) return -errno;
    
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        
        struct stat st;
        memset(&st, 0, sizeof(st));
        char item_path[1024];
        snprintf(item_path, sizeof(item_path), "%s/%s", full_path, de->d_name);
        
        if (lstat(item_path, &st) == 0) {
            filler(buf, de->d_name, &st, 0, 0);
        }
    }
    
    closedir(dp);
    return 0;
}

// Открытие файла
static int vfs_open(const char *path, struct fuse_file_info *fi) {
    char full_path[1024];
    get_full_path(full_path, path);
    
    int res = open(full_path, fi->flags);
    if (res == -1) return -errno;
    
    close(res);
    return 0;
}

// Чтение файла
static int vfs_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi) {
    (void)fi;
    char full_path[1024];
    get_full_path(full_path, path);
    
    int fd = open(full_path, O_RDONLY);
    if (fd == -1) return -errno;
    
    int res = pread(fd, buf, size, offset);
    if (res == -1) res = -errno;
    
    close(fd);
    return res;
}

// Запись в файл
static int vfs_write(const char *path, const char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi) {
    (void)fi;
    char full_path[1024];
    get_full_path(full_path, path);
    
    int fd = open(full_path, O_WRONLY);
    if (fd == -1) return -errno;
    
    int res = pwrite(fd, buf, size, offset);
    if (res == -1) res = -errno;
    
    close(fd);
    return res;
}

// Создание файла
static int vfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    char full_path[1024];
    get_full_path(full_path, path);
    
    int fd = creat(full_path, mode);
    if (fd == -1) return -errno;
    
    if (fi) {
        fi->fh = fd;
    } else {
        close(fd);
    }
    
    return 0;
}

// Создание директории
static int vfs_mkdir(const char *path, mode_t mode) {
    char full_path[1024];
    get_full_path(full_path, path);
    
    int res = mkdir(full_path, mode);
    if (res == -1) return -errno;
    
    return 0;
}

// Удаление файла
static int vfs_unlink(const char *path) {
    char full_path[1024];
    get_full_path(full_path, path);
    
    int res = unlink(full_path);
    if (res == -1) return -errno;
    
    return 0;
}

// Удаление директории
static int vfs_rmdir(const char *path) {
    char full_path[1024];
    get_full_path(full_path, path);
    
    int res = rmdir(full_path);
    if (res == -1) return -errno;
    
    return 0;
}

// Переименование
static int vfs_rename(const char *from, const char *to, unsigned int flags) {
    char full_from[1024], full_to[1024];
    get_full_path(full_from, from);
    get_full_path(full_to, to);
    
    int res;
    if (flags) {
        res = renameat2(0, full_from, 0, full_to, flags);
    } else {
        res = rename(full_from, full_to);
    }
    
    if (res == -1) return -errno;
    return 0;
}

// Создание символьной ссылки
static int vfs_symlink(const char *target, const char *linkpath) {
    char full_link[1024];
    get_full_path(full_link, linkpath);
    
    int res = symlink(target, full_link);
    if (res == -1) return -errno;
    
    return 0;
}

// Чтение ссылки
static int vfs_readlink(const char *path, char *buf, size_t size) {
    char full_path[1024];
    get_full_path(full_path, path);
    
    int res = readlink(full_path, buf, size - 1);
    if (res == -1) return -errno;
    
    buf[res] = '\0';
    return 0;
}

// Создание файлов пользователя в VFS
void create_user_vfs_entry(struct passwd *pw) {
    char *users_dir = get_users_dir_path();
    char user_dir_path[512];
    snprintf(user_dir_path, sizeof(user_dir_path), "%s/%s", users_dir, pw->pw_name);

    if (mkdir(user_dir_path, 0755) == -1 && errno != EEXIST) {
        perror("Ошибка создания директории пользователя");
        return;
    }

    // id
    char id_file_path[512];
    snprintf(id_file_path, sizeof(id_file_path), "%s/id", user_dir_path);
    FILE *f = fopen(id_file_path, "w");
    if (f) { fprintf(f, "%d", pw->pw_uid); fclose(f); }

    // home
    char home_file_path[512];
    snprintf(home_file_path, sizeof(home_file_path), "%s/home", user_dir_path);
    f = fopen(home_file_path, "w");
    if (f) { fprintf(f, "%s", pw->pw_dir); fclose(f); }

    // shell
    char shell_file_path[512];
    snprintf(shell_file_path, sizeof(shell_file_path), "%s/shell", user_dir_path);
    f = fopen(shell_file_path, "w");
    if (f) { fprintf(f, "%s", pw->pw_shell); fclose(f); }

    // info
    char info_file_path[512];
    snprintf(info_file_path, sizeof(info_file_path), "%s/info", user_dir_path);
    f = fopen(info_file_path, "w");
    if (f) {
        fprintf(f, "Username: %s\n", pw->pw_name);
        fprintf(f, "UID: %d\n", pw->pw_uid);
        fprintf(f, "GID: %d\n", pw->pw_gid);
        fprintf(f, "Home: %s\n", pw->pw_dir);
        fprintf(f, "Shell: %s\n", pw->pw_shell);
        if (pw->pw_gecos) fprintf(f, "GECOS: %s\n", pw->pw_gecos);
        fclose(f);
    }

    // symlink
    char link_path[512];
    snprintf(link_path, sizeof(link_path), "%s/home_link", user_dir_path);
    if (access(link_path, F_OK) != 0) {
        symlink(pw->pw_dir, link_path);
    }
}

// Создание VFS
void create_users_vfs() {
    char *users_dir = get_users_dir_path();
    struct stat st = {0};

    if (stat(users_dir, &st) == -1) {
        if (mkdir(users_dir, 0755) == -1) {
            perror("Ошибка создания директории пользователей");
            return;
        }
    }

    struct passwd *pw;
    setpwent();
    while ((pw = getpwent()) != NULL) {
        // Создаём VFS только для пользователей с shell, заканчивающимся на 'sh'
        if (pw->pw_shell && strlen(pw->pw_shell) >= 2 && 
            pw->pw_shell[strlen(pw->pw_shell)-2] == 's' && 
            pw->pw_shell[strlen(pw->pw_shell)-1] == 'h') {
        create_user_vfs_entry(pw);
        }
    }
    endpwent();

    // system_stats
    char stats_path[512];
    snprintf(stats_path, sizeof(stats_path), "%s/system_stats", users_dir);
    FILE *f = fopen(stats_path, "w");
    if (f) {
        fprintf(f, "VFS создан: %s\n", users_dir);
        fprintf(f, "Владелец: %s\n", getenv("USER") ?: "unknown");
        time_t t = time(NULL);
        fprintf(f, "Время создания: %s", ctime(&t));
        fclose(f);
    }

    printf("VFS создан в %s\n", users_dir);
}

// Синхронизация VFS с системой
void sync_vfs_with_system() {
    char *users_dir = get_users_dir_path();
    DIR *dir = opendir(users_dir);
    if (!dir) {
        create_users_vfs();
        return;
    }

    // Сбор имён подкаталогов в VFS
    char vfs_dirs[200][MAX_USERNAME_SIZE];
    int vfs_count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char full_path[1024];
        struct stat st;
        snprintf(full_path, sizeof(full_path), "%s/%s", users_dir, entry->d_name);
        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (strcmp(entry->d_name, ".") != 0 &&
                strcmp(entry->d_name, "..") != 0) {
                strncpy(vfs_dirs[vfs_count], entry->d_name, MAX_USERNAME_SIZE - 1);
                vfs_dirs[vfs_count][MAX_USERNAME_SIZE - 1] = '\0';
                vfs_count++;
            }
        }
    }
    closedir(dir);

    // 1. Если каталог есть, но пользователя нет — создаём (ТОЛЬКО под root)
    if (geteuid() == 0) {
        for (int i = 0; i < vfs_count; i++) {
            if (getpwnam(vfs_dirs[i]) == NULL) {
                char cmd[512];
                snprintf(cmd, sizeof(cmd), "useradd -m -s /bin/bash %s", vfs_dirs[i]);
                int res = system(cmd);
                if (res == 0 || (WIFEXITED(res) && WEXITSTATUS(res) == 0)) {
                    setpwent();
                    struct passwd *pw = getpwnam(vfs_dirs[i]);
                    endpwent();
                    if (pw) {
                        create_user_vfs_entry(pw);
                    }
                }
            }
        }

        // 2. Если пользователь есть (UID>=1000), но каталога нет — удаляем пользователя
        // Удаляем только обычных пользователей с shell на *sh, root и системные аккаунты не трогаем.
        struct passwd *pw;
        char *vfs_root = get_users_dir_path();
        setpwent();
        while ((pw = getpwent()) != NULL) {
            if (pw->pw_uid < 1000) continue; // не трогаем root и системных
            if (!(pw->pw_shell && strlen(pw->pw_shell) >= 2 &&
                  pw->pw_shell[strlen(pw->pw_shell)-2] == 's' &&
                  pw->pw_shell[strlen(pw->pw_shell)-1] == 'h')) {
                continue;
            }

            char user_dir[512];
            snprintf(user_dir, sizeof(user_dir), "%s/%s", vfs_root, pw->pw_name);
            if (access(user_dir, F_OK) != 0) {
                char cmd[512];
                snprintf(cmd, sizeof(cmd), "userdel -r %s", pw->pw_name);
                system(cmd);
            }
        }
        endpwent();
    }
}

// Структура операций FUSE
static struct fuse_operations vfs_operations = {
    .getattr    = vfs_getattr,
    .readdir    = vfs_readdir,
    .open       = vfs_open,
    .read       = vfs_read,
    .write      = vfs_write,
    .create     = vfs_create,
    .mkdir      = vfs_mkdir,
    .unlink     = vfs_unlink,
    .rmdir      = vfs_rmdir,
    .rename     = vfs_rename,
    .symlink    = vfs_symlink,
    .readlink   = vfs_readlink,
};

// Основная функция FUSE (для отдельной программы VFS)
int vfs_fuse_main(int argc, char *argv[]) {
    create_users_vfs();
    return fuse_main(argc, argv, &vfs_operations, NULL);
}
