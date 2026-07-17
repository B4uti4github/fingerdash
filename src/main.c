/*
 * lsh.c — FingerDash Shell for UEFI
 *
 * Compila como fingerdash.efi usando POSIX-UEFI.
 * No fork(), no exec(), no señales. Usa uefi_process_compat
 * para ejecutar otros .efi externos.
 */

#include <uefi.h>
#include <stdio.h>   /* para EOF */
#include <string.h>  /* para strcmp, strtok */
#include "uefi_process_compat.h"

/* ------------------------------------------------------------------ *
 *  Forward declarations
 * ------------------------------------------------------------------ */
static int lsh_loop(void);
static int lsh_execute(char **args);
static char *lsh_read_line(void);
static char **lsh_split_line(char *line);

/* ------------------------------------------------------------------ *
 *  Entry point — POSIX-UEFI calls this main(argc, argv)
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
 *  Read a line from stdin (UEFI ConsoleIn)
 * ------------------------------------------------------------------ */
static char *lsh_read_line(void)
{
    size_t bufsize = 1024;
    size_t pos = 0;
    char *buffer = (char *)malloc(bufsize);
    if (!buffer) return NULL;

    int c;

    while (1) {
        c = getchar();
        if (c == EOF || c == '\n') {
            buffer[pos] = '\0';
            return buffer;
        }
        buffer[pos] = c;
        pos++;

        if (pos >= bufsize) {
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

/* ------------------------------------------------------------------ *
 *  Split line into args (very basic tokenizer)
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
 *  Execute a command
 * ------------------------------------------------------------------ */
static int lsh_execute(char **args)
{
    if (!args || !args[0]) return 1;

    /* Builtins */
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
            /* UEFI path handling would go here */
            printf("cd: %s (stub)\n", args[1]);
        }
        return 1;
    }
    if (strcmp(args[0], "pwd") == 0) {
        printf("/\n"); /* stub */
        return 1;
    }

    /* External command: run another .efi */
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

    /* Unknown command */
    printf("fingerdash: command not found: %s\n", args[0]);
    return 1;
}