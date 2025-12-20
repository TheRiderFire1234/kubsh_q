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
