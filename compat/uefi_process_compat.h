/*
 * uefi_process_compat.h
 *
 * fork()/execve()/waitpid()-shaped shim so that dash (or anything with the
 * same three call patterns) can run as a single .efi image on top of
 * POSIX-UEFI, with NO real multiprocessing, NO MMU-managed isolation, and
 * NO scheduler.
 *
 * WHAT THIS IS
 *   A mapping of dash's *specific, limited* uses of fork()/exec()/wait()
 *   onto primitives UEFI Boot Services already provides (LoadImage /
 *   StartImage) or that can be faked cheaply with save/restore of shell
 *   state, since nothing ever runs concurrently.
 *
 * WHAT THIS IS NOT
 *   - Not a general fork() implementation. There is no address-space clone.
 *   - Not a scheduler. Background jobs ("cmd &"), SIGCHLD, and WNOHANG
 *     polling of an arbitrary still-running child are impossible under
 *     this model and are stubbed to fail loudly rather than pretend.
 *   - Not signal handling. UEFI Boot Services has no concept of it; the
 *     handful of signal-related calls dash makes are stubbed as no-ops.
 *
 * INTEGRATION MODEL
 *   dash's process creation funnels through a small number of call sites
 *   (mainly jobs.c: forkshell()/forkparent()/forkchild(), and eval.c around
 *   subshell and command evaluation). You do NOT implement fork() as a
 *   libc-shaped function and hope dash's control flow "just works" across
 *   it (it won't -- there is no second instruction stream). Instead you
 *   replace those call sites directly with the two entry points below:
 *
 *     - uefi_spawn_external()  replaces the fork()+execve() pair used to
 *       run an external command (an on-disk .efi).
 *     - uefi_run_subshell()    replaces the fork() used to isolate a
 *       builtin/subshell's state (e.g. "(cd /x && ls)") from the parent
 *       shell, without needing a second address space.
 *
 *   See patches/README.md in this repo for exactly which functions in
 *   dash's jobs.c / eval.c to redirect, and why each one maps the way it
 *   does.
 *
 * LICENSE: MIT, same terms as POSIX-UEFI, so it can sit next to it freely.
 */

#ifndef UEFI_PROCESS_COMPAT_H
#define UEFI_PROCESS_COMPAT_H

#include <uefi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 *  Result / status codes
 * ------------------------------------------------------------------ */

/* Mirrors just enough of <sys/wait.h> for dash's WIFEXITED/WEXITSTATUS
 * call sites to keep compiling unmodified where possible. */
typedef struct {
    int      exited;      /* always 1 in this model: nothing "signals" */
    int      exit_status; /* 0-255, matches EFI_STATUS low byte convention */
    efi_status_t efi_status; /* raw EFI status, for diagnostics */
} uefi_proc_result_t;

#define UEFI_WIFEXITED(r)   ((r).exited)
#define UEFI_WEXITSTATUS(r) ((r).exit_status)

/* ------------------------------------------------------------------ *
 *  1. External command execution
 *     Replaces: pid = fork(); if (pid == 0) execve(path, argv, envp);
 *               waitpid(pid, &status, 0);
 * ------------------------------------------------------------------ */

/*
 * Loads `path` (an .efi on a volume reachable via Simple File System
 * Protocol) as a child image and runs it to completion. This is a direct
 * wrapper around BS->LoadImage + BS->StartImage, which is UEFI's own
 * synchronous "run this program and block until it's done" primitive --
 * dash never needs to see that there was no fork() underneath.
 *
 * argv/envp are flattened into the LoadOptions blob StartImage's child
 * reads via EFI_LOADED_IMAGE_PROTOCOL, using POSIX-UEFI's argv marshalling
 * convention so a POSIX-UEFI-built child sees a normal argc/argv.
 *
 * Returns 0 on successful load+run (check result->exit_status for the
 * child's actual return code), non-zero if the image could not be loaded
 * at all (file not found, bad PE header, out of memory, etc).
 */
int uefi_spawn_external(const char_t *path, char_t *const argv[],
                         char_t *const envp[], uefi_proc_result_t *result);

/* ------------------------------------------------------------------ *
 *  2. Subshell / builtin isolation
 *     Replaces: pid = fork(); if (pid == 0) { <builtin runs>; _exit(rc); }
 *               waitpid(pid, &status, 0);
 * ------------------------------------------------------------------ */

typedef struct {
    char_t *saved_cwd;
    void   *saved_vartab;   /* opaque: shell's variable table, shallow-copied */
    int     saved_fd_stdin;
    int     saved_fd_stdout;
    int     saved_fd_stderr;
} uefi_shell_state_t;

/* Snapshots the bits of shell state a POSIX subshell is required to
 * change in isolation (cwd, local variables, redirected fds) without
 * affecting the parent. Call before running the subshell's body. */
void uefi_shell_state_save(uefi_shell_state_t *out);

/* Restores exactly what was snapshotted. Call after the subshell body
 * returns, regardless of its exit status. */
void uefi_shell_state_restore(const uefi_shell_state_t *saved);

/*
 * Runs `body(ctx)` as an isolated subshell: saves state, executes body,
 * restores state, and packages body's return value as if it had been a
 * real forked child's exit status. Because there's no second thread of
 * execution, `body` runs synchronously on the caller's stack -- there is
 * no race to guard against, which is precisely why this doesn't need to
 * be a real fork().
 */
int uefi_run_subshell(int (*body)(void *ctx), void *ctx,
                       uefi_proc_result_t *result);

/* ------------------------------------------------------------------ *
 *  3. Pipelines: cmd1 | cmd2 | cmd3
 *     Replaces: pipe() + fork() N times + dup2() + exec() per stage.
 * ------------------------------------------------------------------ */

/*
 * NOT a streaming pipe. Because there is no second process to run
 * concurrently, each stage must run to completion before the next stage
 * starts. uefi_pipe_buf_t is an in-memory growable buffer standing in for
 * both ends of a pipe(2) fd pair.
 *
 * Consequence dash/scripts must accept: pipelines that assume streaming
 * concurrency (e.g. "yes | head -1", "tail -f x | grep y") will not behave
 * correctly -- "yes" never terminates, so the buffer never completes and
 * the pipeline hangs. Finite pipelines ("cat f | grep x | sort") work
 * correctly because each stage naturally produces bounded output.
 */
typedef struct {
    uint8_t *data;
    uintn_t  len;
    uintn_t  cap;
} uefi_pipe_buf_t;

void uefi_pipe_buf_init(uefi_pipe_buf_t *buf);
void uefi_pipe_buf_free(uefi_pipe_buf_t *buf);

/* Runs `producer` with its stdout captured into `out`. */
int uefi_pipe_stage_capture(int (*producer)(void *ctx), void *ctx,
                             uefi_pipe_buf_t *out, uefi_proc_result_t *result);

/* Runs `consumer` with `in` fed to it as stdin. */
int uefi_pipe_stage_feed(int (*consumer)(void *ctx), void *ctx,
                          const uefi_pipe_buf_t *in, uefi_proc_result_t *result);

/* ------------------------------------------------------------------ *
 *  4. Stubs that must exist for dash to link, but cannot behave as on
 *     POSIX -- they fail explicitly instead of silently misbehaving.
 * ------------------------------------------------------------------ */

/* Job control ("cmd &", bg, fg, jobs). There is no concurrency to control.
 * Always returns -1 / sets an error dash can surface to the user, e.g.
 * "uefi-shell: job control not supported (no scheduler)". */
int uefi_bg_unsupported(void);

/* Signals: SIGCHLD, SIGINT handler installation, etc. No-ops that report
 * success so dash's setup code doesn't abort, but nothing ever fires one. */
int uefi_signal_noop(int signum, void *handler);

#ifdef __cplusplus
}
#endif

#endif /* UEFI_PROCESS_COMPAT_H */
