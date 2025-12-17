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

// Для тестов kubsh в Docker VFS должен быть в /opt/users (запуск под root),
// но для обычного пользователя на хосте создаём VFS в $HOME/users.
#define USERS_DIR_ROOT "/opt/users"
#define USERS_DIR_HOME "users"

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

// Путь к VFS директории:
//  - если запущено от root (как в тестовом Docker-образе) → /opt/users
//  - если обычный пользователь на хосте → $HOME/users
char* get_users_dir_path() {
    static char path[1024];

    if (geteuid() == 0) {
        snprintf(path, sizeof(path), "%s", USERS_DIR_ROOT);
    } else {
        char *home = get_home_path();
        snprintf(path, sizeof(path), "%s/%s", home, USERS_DIR_HOME);
    }

    return path;
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

// 🔁 Синхронизация VFS с системой
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

// Команда: обновить VFS (с синхронизацией)
void cmd_refresh_vfs() {
    printf("Синхронизация VFS с системой...\n");
    sync_vfs_with_system();
    printf("VFS обновлён\n");
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

// Команда: разделы диска
void cmd_list_partitions(const char *device) {
    if (!device) {
        device = "";
    }

    // Пропускаем начальные пробелы
    while (*device == ' ') device++;

    if (strlen(device) == 0) {
        printf("Использование: \\l <устройство> (например, \\l /dev/sda)\n");
        printf("Доступные устройства:\n");
        system("lsblk -o NAME,SIZE,TYPE,MOUNTPOINT 2>/dev/null | grep -E '^(sd|hd|vd|nvme|mmcblk)' | head -10");
        return;
    }

    // Разрешаем ввод без /dev/, например: \l sda
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
    // Информация без sudo
    snprintf(cmd, sizeof(cmd), "fdisk -l %s 2>/dev/null | head -20", dev_path);
    system(cmd);
    printf("\n--- lsblk ---\n");
    snprintf(cmd, sizeof(cmd), "lsblk %s -o NAME,SIZE,TYPE,MOUNTPOINT,FSTYPE 2>/dev/null", dev_path);
    system(cmd);
    printf("\n--- df ---\n");
    snprintf(cmd, sizeof(cmd), "df -h | grep '%s' 2>/dev/null || echo 'Нет примонтированных разделов'", dev_path);
    system(cmd);
}

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
    // Для совместимости с тестами — выводим сырое значение отдельной строкой
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
        while (*msg == ' ') msg++; // Пропускаем пробелы
        size_t len = strlen(msg);
        if (len >= 2 && ((msg[0] == '\'' && msg[len-1] == '\'') || (msg[0] == '"' && msg[len-1] == '"'))) {
            // Убираем кавычки и выводим значение на отдельной строке
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
        // Выполнение бинарника из $PATH через system (использует shell → ищет в PATH)
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
        // Это гарантирует, что новые директории будут обнаружены
        // даже если они созданы после запуска kubsh
        sync_vfs_with_system();
        
        printf("kubsh> ");
        fflush(stdout);

        // Пока ждём ввод, периодически синхронизируем VFS
        while (1) {
            sync_vfs_with_system();

            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(STDIN_FILENO, &rfds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 200000; // 200ms

            int rv = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
            if (rv == -1) {
                if (errno == EINTR) continue;
                break;
            }
            if (rv == 0) {
                // таймаут — продолжаем ждать, но синхронизацию уже сделали
                continue;
            }
            break; // stdin готов для чтения
        }

        if (!fgets(input, sizeof(input), stdin)) {
            // перед выходом ещё раз синхронизируем
            sync_vfs_with_system();
            break;
        }

        input[strcspn(input, "\n")] = 0;
        if (strlen(input) == 0) continue;

        if (strcmp(input, "\\q") == 0) break;

        add_to_history(input);
        process_command(input);
        
        // Синхронизируем VFS после обработки команды (для тестов)
        // Это гарантирует, что новые директории будут обнаружены после команд
        sync_vfs_with_system();
    }

    printf("\nВыход из shell\n");
    save_history();
    free_history();
    return 0;
}