/*
 * lsh.c — FingerDash Shell for UEFI
 */

#include <uefi.h>
#include "uefi_process_compat.h"

/* ------------------------------------------------------------------ *
 *  Forward declarations
 * ------------------------------------------------------------------ */
static int lsh_loop(void);
static int lsh_execute(char **args);
static char *lsh_read_line(void);
static char **lsh_split_line(char *line);

/* ------------------------------------------------------------------ *
 *  Entry point
 * ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("\n");
    printf("========================================\n");
    printf("  FingerDash Shell v1.0  (UEFI)\n");
    printf("  Type 'help' for commands, 'exit' to quit\n");
    printf("========================================\n");
    printf("\n");

    return lsh_loop();
}

/* ------------------------------------------------------------------ *
 *  Read a line from UEFI ConsoleIn (teclado real)
 * ------------------------------------------------------------------ */
static char *lsh_read_line(void)
{
    size_t bufsize = 1024;
    size_t pos = 0;
    char *buffer = (char *)malloc(bufsize);
    if (!buffer) return NULL;

    efi_input_key_t key;
    efi_status_t status;

    while (1) {
        /* Esperar a que haya una tecla disponible */
        status = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        
        if (EFI_ERROR(status)) {
            /* No hay tecla, esperar el evento */
            if (status == EFI_NOT_READY) {
                /* Esperar el evento de teclado */
                uintn_t index;
                BS->WaitForEvent(1, &ST->ConIn->WaitForKey, &index);
                continue;
            }
            /* Error real */
            free(buffer);
            return NULL;
        }

        /* Enter = fin de línea */
        if (key.UnicodeChar == L'\r' || key.UnicodeChar == L'\n') {
            printf("\n");
            buffer[pos] = '\0';
            return buffer;
        }

        /* Backspace */
        if (key.UnicodeChar == L'\b' || key.ScanCode == 0x08 || key.ScanCode == 0x09) {
            if (pos > 0) {
                pos--;
                printf("\b \b");  /* borrar en pantalla */
            }
            continue;
        }

        /* Caracter imprimible */
        if (key.UnicodeChar != 0 && key.UnicodeChar >= L' ' && key.UnicodeChar < 0x7F) {
            char ch = (char)key.UnicodeChar;
            buffer[pos] = ch;
            pos++;
            printf("%c", ch);  /* eco en pantalla */

            if (pos >= bufsize - 1) {
                bufsize += 1024;
                char *newbuf = (char *)realloc(buffer, bufsize);
                if (!newbuf) {
                    free(buffer);
                    return NULL;
                }
                buffer = newbuf;
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Split line into args
 * ------------------------------------------------------------------ */
static char **lsh_split_line(char *line)
{
    size_t bufsize = 64;
    size_t pos = 0;
    char **tokens = (char **)malloc(bufsize * sizeof(char *));
    if (!tokens) return NULL;

    char *token = strtok(line, " \t\r\n");
    while (token) {
        tokens[pos] = token;
        pos++;

        if (pos >= bufsize) {
            bufsize += 64;
            char **newtokens = (char **)realloc(tokens, bufsize * sizeof(char *));
            if (!newtokens) {
                free(tokens);
                return NULL;
            }
            tokens = newtokens;
        }

        token = strtok(NULL, " \t\r\n");
    }
    tokens[pos] = NULL;
    return tokens;
}

/* ------------------------------------------------------------------ *
 *  Main interactive loop
 * ------------------------------------------------------------------ */
static int lsh_loop(void)
{
    char *line;
    char **args;
    int status = 1;

    while (status) {
        printf("fingerdash> ");
        line = lsh_read_line();
        if (!line) {
            printf("\n");
            break;
        }

        args = lsh_split_line(line);
        status = lsh_execute(args);

        free(line);
        free(args);
    }

    return 0;
}

/* ------------------------------------------------------------------ *
 *  Execute a command
 * ------------------------------------------------------------------ */
static int lsh_execute(char **args)
{
    if (!args || !args[0]) return 1;

    if (strcmp(args[0], "exit") == 0) {
        return 0;
    }
    if (strcmp(args[0], "help") == 0) {
        printf("FingerDash built-in commands:\n");
        printf("  help              Show this help\n");
        printf("  exit              Quit the shell\n");
        printf("  run <file.efi>    Load and execute a UEFI application\n");
        printf("  cd <path>         Change directory\n");
        printf("  pwd               Print working directory\n");
        printf("  echo <text>       Print text\n");
        printf("  clear             Clear screen\n");
        return 1;
    }
    if (strcmp(args[0], "echo") == 0) {
        for (int i = 1; args[i]; i++) {
            printf("%s ", args[i]);
        }
        printf("\n");
        return 1;
    }
    if (strcmp(args[0], "clear") == 0) {
        printf("\033[2J\033[H");
        return 1;
    }
    if (strcmp(args[0], "cd") == 0) {
        if (!args[1]) {
            printf("cd: missing argument\n");
        } else {
            printf("cd: %s (stub)\n", args[1]);
        }
        return 1;
    }
    if (strcmp(args[0], "pwd") == 0) {
        printf("/\n");
        return 1;
    }

    if (strcmp(args[0], "run") == 0) {
        if (!args[1]) {
            printf("run: missing file.efi argument\n");
            return 1;
        }

        uefi_proc_result_t result;
        int rc = uefi_spawn_external(args[1], args + 1, NULL, &result);
        if (rc != 0) {
            printf("run: failed to execute %s\n", args[1]);
            return 1;
        }
        if (UEFI_WIFEXITED(result)) {
            printf("run: exited with status %d\n", UEFI_WEXITSTATUS(result));
        }
        return 1;
    }

    if (strncmp(args[0], "./", 2) == 0) {
        const char *path = args[0] + 2;  /* saltar "./" */
        
        /* Si queda vacío, error */
        if (!path || path[0] == '\0') {
            printf("./: missing file name\n");
            return 1;
        }

        uefi_proc_result_t result;
        int rc = uefi_spawn_external(path, args + 1, NULL, &result);
        if (rc != 0) {
            printf("%s: failed to execute\n", path);
            return 1;
        }
        if (UEFI_WIFEXITED(result)) {
            printf("%s: exited with status %d\n", path, UEFI_WEXITSTATUS(result));
        }
        return 1;
    }

    printf("fingerdash: command not found: %s\n", args[0]);
    return 1;
}