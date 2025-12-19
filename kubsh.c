#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <time.h>

#define MAX_INPUT_SIZE 1024
#define MAX_HISTORY_SIZE 100
#define MAX_USERNAME_SIZE 32
#define HISTORY_FILE ".kubsh_history"

char *history[MAX_HISTORY_SIZE];
int history_count = 0;

// Структура для хранения информации о пользователе
typedef struct {
    char username[MAX_USERNAME_SIZE];
    int uid;
    char home[256];
    char shell[256];
} UserInfo;

// Обработчик сигнала SIGHUP
void sighup_handler(int sig) {
    printf("\nConfiguration reloaded (SIGHUP received)\n");
    printf("kubsh> ");
    fflush(stdout);
}

// Получение домашней директории
char* get_home_path() {
    char *home = getenv("HOME");
    if (home == NULL) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
        else home = "/tmp";
    }
    return home;
}

// Путь к файлу истории
char* get_history_path() {
    static char path[1024];
    char *home = get_home_path();
    snprintf(path, sizeof(path), "%s/%s", home, HISTORY_FILE);
    return path;
}

// Объявления функций из vfs.c
char* get_users_dir_path();
void create_users_vfs();
void sync_vfs_with_system();
void create_user_vfs_entry(struct passwd *pw);

// История
void load_history() {
    char *path = get_history_path();
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[MAX_INPUT_SIZE];
    while (history_count < MAX_HISTORY_SIZE && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) == 0) continue;
        history[history_count] = strdup(line);
        history_count++;
    }
    fclose(f);
}

void save_history() {
    char *path = get_history_path();
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < history_count; i++) {
        fprintf(f, "%s\n", history[i]);
    }
    fclose(f);
}

void add_to_history(const char *cmd) {
    if (strlen(cmd) == 0 || strcmp(cmd, "\\q") == 0) return;
    if (history_count >= MAX_HISTORY_SIZE) {
        free(history[0]);
        for (int i = 1; i < history_count; i++) history[i-1] = history[i];
        history_count--;
    }
    history[history_count++] = strdup(cmd);
}

void print_history() {
    for (int i = 0; i < history_count; i++) {
        printf("%3d: %s\n", i+1, history[i]);
    }
}

void free_history() {
    for (int i = 0; i < history_count; i++) free(history[i]);
    history_count = 0;
}

// echo
void cmd_echo(const char *args) {
    if (!args || !*args) { printf("\n"); return; }
    while (*args == ' ') args++;
    if ((*args == '"' && args[strlen(args)-1] == '"') ||
        (*args == '\'' && args[strlen(args)-1] == '\'')) {
        printf("%.*s\n", (int)strlen(args)-2, args+1);
    } else {
        printf("%s\n", args);
    }
}

// \e — переменные окружения
void cmd_environment(const char *args) {
    if (!args || !*args) {
        printf("Использование: \\e <переменная> (например, \\e PATH)\n");
        return;
    }
    while (*args == ' ') args++;
    if (*args == '$') args++;

    char var[256];
    sscanf(args, "%255s", var);
    char *val = getenv(var);
    if (!val) {
        printf("Переменная '%s' не найдена\n", var);
        return;
    }

    printf("Переменная: %s\nЗначение: %s\n", var, val);
    printf("%s\n", val);
    if (strchr(val, ':')) {
        char *copy = strdup(val);
        char *tok = strtok(copy, ":");
        while (tok) {
            printf("%s\n", tok);
            tok = strtok(NULL, ":");
        }
        free(copy);
    }
}

// Команда: разделы диска
void cmd_list_partitions(const char *device) {
    if (!device) {
        device = "";
    }

    while (*device == ' ') device++;

    if (strlen(device) == 0) {
        printf("Использование: \\l <устройство> (например, \\l /dev/sda)\n");
        printf("Доступные устройства:\n");
        system("lsblk -o NAME,SIZE,TYPE,MOUNTPOINT 2>/dev/null | grep -E '^(sd|hd|vd|nvme|mmcblk)' | head -10");
        return;
    }

    char dev_path[256];
    if (strchr(device, '/')) {
        snprintf(dev_path, sizeof(dev_path), "%s", device);
    } else {
        snprintf(dev_path, sizeof(dev_path), "/dev/%s", device);
    }

    struct stat st;
    if (stat(dev_path, &st) == -1) {
        printf("Ошибка: устройство %s не найдено\n", dev_path);
        return;
    }

    printf("Информация о %s:\n", dev_path);
    printf("==========================================\n");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "fdisk -l %s 2>/dev/null | head -20", dev_path);
    system(cmd);
    printf("\n--- lsblk ---\n");
    snprintf(cmd, sizeof(cmd), "lsblk %s -o NAME,SIZE,TYPE,MOUNTPOINT,FSTYPE 2>/dev/null", dev_path);
    system(cmd);
    printf("\n--- df ---\n");
    snprintf(cmd, sizeof(cmd), "df -h | grep '%s' 2>/dev/null || echo 'Нет примонтированных разделов'", dev_path);
    system(cmd);
}

// Команда: показать VFS
void cmd_show_vfs() {
    char *users_dir = get_users_dir_path();
    printf("Структура VFS в %s:\n", users_dir);
    printf("==========================================\n");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "which tree >/dev/null 2>&1 && tree -L 2 %s || (echo 'Дерево:' && find %s -type f | sort | head -30)", users_dir, users_dir);
    system(cmd);
}

// Команда: обновить VFS (с синхронизацией)
void cmd_refresh_vfs() {
    printf("Синхронизация VFS с системой...\n");
    sync_vfs_with_system();
    printf("VFS обновлён\n");
}

// adduser (через команду)
void cmd_adduser(const char *user) {
    if (!user || !*user) { printf("Использование: adduser <username>\n"); return; }
    if (getpwnam(user)) { printf("Пользователь %s уже существует\n", user); return; }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "sudo useradd -m -s /bin/bash %s", user);
    if (system(cmd) == 0) {
        printf("Пользователь %s создан. Обновляем VFS...\n", user);
        cmd_refresh_vfs();
    } else {
        printf("Ошибка создания %s\n", user);
    }
}

// userdel
void cmd_userdel(const char *user) {
    if (!user || !*user) { printf("Использование: userdel <username>\n"); return; }
    if (!getpwnam(user)) { printf("Пользователь %s не существует\n", user); return; }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "sudo userdel -r %s", user);
    if (system(cmd) == 0) {
        printf("Пользователь %s удалён. Обновляем VFS...\n", user);
        cmd_refresh_vfs();
    } else {
        printf("Ошибка удаления %s\n", user);
    }
}

// listusers — из VFS
void cmd_listusers() {
    char *users_dir = get_users_dir_path();
    DIR *dir = opendir(users_dir);
    if (!dir) {
        printf("VFS не найден. Создаём...\n");
        create_users_vfs();
        dir = opendir(users_dir);
        if (!dir) { printf("Ошибка VFS\n"); return; }
    }

    printf("%-15s %-8s %-20s %s\n", "Username", "UID", "Home", "Shell");
    printf("------------------------------------------------------------\n");

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_type == DT_DIR && strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", users_dir, entry->d_name);

            char id[64] = "??", home[256] = "??", shell[256] = "??";

            char id_path[512]; snprintf(id_path, sizeof(id_path), "%s/id", path);
            FILE *f = fopen(id_path, "r");
            if (f) { fgets(id, sizeof(id), f); fclose(f); }

            char home_path[512]; snprintf(home_path, sizeof(home_path), "%s/home", path);
            f = fopen(home_path, "r");
            if (f) { fgets(home, sizeof(home), f); fclose(f); }
            home[strcspn(home, "\n")] = 0;

            char shell_path[512]; snprintf(shell_path, sizeof(shell_path), "%s/shell", path);
            f = fopen(shell_path, "r");
            if (f) { fgets(shell, sizeof(shell), f); fclose(f); }
            shell[strcspn(shell, "\n")] = 0;

            printf("%-15s %-8s %-20s %s\n", entry->d_name, id, home, shell);
        }
    }
    closedir(dir);
}

// help
void cmd_help() {
    printf("Команды:\n"
           "  \\q         — выход\n"
           "  \\history   — история\n"
           "  \\e <var>   — переменная окружения\n"
           "  \\l <диск>  — разделы диска\n"
           "  \\vfs       — структура VFS\n"
           "  \\refresh   — синхронизация VFS\n"
           "  echo ...    — вывод\n"
           "  adduser ... — создать пользователя\n"
           "  userdel ... — удалить пользователя\n"
           "  listusers   — список из VFS\n"
           "  help        — эта справка\n"
           "VFS: %s\n", get_users_dir_path());
}

// Обработка команд
void process_command(const char *input) {
    if (strncmp(input, "echo ", 5) == 0) {
        cmd_echo(input + 5);
    } else if (strncmp(input, "debug ", 6) == 0) {
        const char *msg = input + 6;
        while (*msg == ' ') msg++;
        size_t len = strlen(msg);
        if (len >= 2 && ((msg[0] == '\'' && msg[len-1] == '\'') || (msg[0] == '"' && msg[len-1] == '"'))) {
            printf("\n%.*s\n", (int)(len-2), msg+1);
        } else {
            printf("\n%s\n", msg);
        }
    } else if (strncmp(input, "adduser ", 8) == 0) {
        cmd_adduser(input + 8);
    } else if (strncmp(input, "userdel ", 8) == 0) {
        cmd_userdel(input + 8);
    } else if (strcmp(input, "listusers") == 0) {
        cmd_listusers();
    } else if (strcmp(input, "help") == 0) {
        cmd_help();
    } else if (strcmp(input, "\\history") == 0) {
        print_history();
    } else if (strncmp(input, "\\e", 2) == 0) {
        cmd_environment(input + 2);
    } else if (strncmp(input, "\\l", 2) == 0) {
        cmd_list_partitions(input + 2);
    } else if (strcmp(input, "\\vfs") == 0) {
        cmd_show_vfs();
    } else if (strcmp(input, "\\refresh") == 0) {
        cmd_refresh_vfs();
    } else {
        int result = system(input);
        if (result != 0) {
            printf("%s: command not found\n", input);
        }
    }
}

// Главная функция
int main() {
    signal(SIGHUP, sighup_handler);
    create_users_vfs();
    load_history();
    
    // Синхронизируем VFS при запуске (для тестов)
    sync_vfs_with_system();

    printf("KubShell с VFS\nVFS: %s\nВведите 'help' для справки\n\n", get_users_dir_path());

    char input[MAX_INPUT_SIZE];
    while (1) {
        // Синхронизируем VFS ПЕРЕД каждым промптом (для тестов)
        sync_vfs_with_system();
        
        printf("kubsh> ");
        fflush(stdout);

        while (1) {
            sync_vfs_with_system();

            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(STDIN_FILENO, &rfds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 200000;

            int rv = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
            if (rv == -1) {
                if (errno == EINTR) continue;
                break;
            }
            if (rv == 0) {
                continue;
            }
            break;
        }

        if (!fgets(input, sizeof(input), stdin)) {
            sync_vfs_with_system();
            break;
        }

        input[strcspn(input, "\n")] = 0;
        if (strlen(input) == 0) continue;

        if (strcmp(input, "\\q") == 0) break;

        add_to_history(input);
        process_command(input);
        
        sync_vfs_with_system();
    }

    printf("\nВыход из shell\n");
    save_history();
    free_history();
    return 0;
}
