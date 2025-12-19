#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

#define MAX_INPUT_SIZE 1024
#define MAX_HISTORY_SIZE 100
#define HISTORY_FILE ".kubsh_history"

// Mount point for VFS
#define USERS_DIR_ROOT "/opt/users"
#define USERS_DIR_HOME "users"

char *history[MAX_HISTORY_SIZE];
int history_count = 0;

// === External FUSE VFS ===
extern int start_fuse_vfs(const char *mountpoint);

// Signal handler
void sighup_handler(int sig) {
    printf("\nSIGHUP received (FUSE mode: no reload)\n");
    printf("kubsh> ");
    fflush(stdout);
}

// Get home dir
char* get_home_path() {
    char *home = getenv("HOME");
    if (!home) home = "/tmp";
    return home;
}

// Get VFS mount path
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

// History
char* get_history_path() {
    static char path[1024];
    char *home = get_home_path();
    snprintf(path, sizeof(path), "%s/%s", home, HISTORY_FILE);
    return path;
}

void load_history() {
    FILE *f = fopen(get_history_path(), "r");
    if (!f) return;
    char line[MAX_INPUT_SIZE];
    while (history_count < MAX_HISTORY_SIZE && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line)) history[history_count++] = strdup(line);
    }
    fclose(f);
}

void save_history() {
    FILE *f = fopen(get_history_path(), "w");
    if (!f) return;
    for (int i = 0; i < history_count; i++)
        fprintf(f, "%s\n", history[i]);
    fclose(f);
}

void add_to_history(const char *cmd) {
    if (!cmd[0] || strcmp(cmd, "\\q") == 0) return;
    if (history_count >= MAX_HISTORY_SIZE) {
        free(history[0]);
        for (int i = 1; i < history_count; i++) history[i-1] = history[i];
        history_count--;
    }
    history[history_count++] = strdup(cmd);
}

void print_history() {
    for (int i = 0; i < history_count; i++)
        printf("%3d: %s\n", i+1, history[i]);
}

void free_history() {
    for (int i = 0; i < history_count; i++) free(history[i]);
    history_count = 0;
}

// Commands
void cmd_help() {
    printf("Команды:\n"
           "  \\q         — выход\n"
           "  \\history   — история\n"
           "  \\e <var>   — переменная окружения\n"
           "  echo ...    — вывод\n"
           "  help        — эта справка\n"
           "VFS: %s (FUSE-based)\n", get_users_dir_path());
}

void cmd_environment(const char *args) {
    if (!args || !*args) {
        printf("Использование: \\e <переменная>\n");
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
    printf("%s\n", val);
    if (strchr(val, ':')) {
        char *copy = strdup(val);
        for (char *tok = strtok(copy, ":"); tok; tok = strtok(NULL, ":"))
            printf("%s\n", tok);
        free(copy);
    }
}

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

void process_command(const char *input) {
    if (strncmp(input, "echo ", 5) == 0) {
        cmd_echo(input + 5);
    } else if (strncmp(input, "debug ", 6) == 0) {
        const char *msg = input + 6;
        while (*msg == ' ') msg++;
        size_t len = strlen(msg);
        if (len >= 2 && ((msg[0] == '\'' && msg[len-1] == '\'') || (msg[0] == '"' && msg[len-1] == '"'))) {
            printf("%.*s\n", (int)(len-2), msg+1);
        } else {
            printf("%s\n", msg);
        }
    } else if (strcmp(input, "\\history") == 0) {
        print_history();
    } else if (strncmp(input, "\\e", 2) == 0) {
        cmd_environment(input + 2);
    } else if (strcmp(input, "help") == 0) {
        cmd_help();
    } else {
        printf("%s: command not supported in FUSE mode\n", input);
    }
}

// FUSE thread
void* fuse_thread_func(void *arg) {
    const char *mountpoint = (const char*)arg;
    start_fuse_vfs(mountpoint);
    return NULL;
}

// Main
int main() {
    signal(SIGHUP, sighup_handler);
    char *mountpoint = get_users_dir_path();

    // Ensure mountpoint exists
    struct stat st;
    if (stat(mountpoint, &st) != 0) {
        if (mkdir(mountpoint, 0755) != 0) {
            perror("Не удалось создать точку монтирования");
            return 1;
        }
    }

    // Start FUSE in background thread
    pthread_t fuse_th;
    if (pthread_create(&fuse_th, NULL, fuse_thread_func, mountpoint) != 0) {
        perror("Не удалось запустить FUSE");
        return 1;
    }

    // Small delay to let FUSE initialize
    usleep(100000);

    load_history();
    printf("KubShell с FUSE-VFS\nVFS: %s\nВведите 'help' для справки\n\n", mountpoint);

    char input[MAX_INPUT_SIZE];
    while (1) {
        printf("kubsh> ");
        fflush(stdout);
        if (!fgets(input, sizeof(input), stdin)) break;
        input[strcspn(input, "\n")] = 0;
        if (!input[0]) continue;
        if (strcmp(input, "\\q") == 0) break;

        add_to_history(input);
        process_command(input);
    }

    printf("\nВыход из shell\n");
    save_history();
    free_history();
    return 0;
}
