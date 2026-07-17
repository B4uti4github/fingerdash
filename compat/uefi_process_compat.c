/*
 * uefi_process_compat.c
 *
 * See uefi_process_compat.h for the design rationale. This file only
 * implements the mechanism; it deliberately does not try to be clever
 * about anything fork()-shaped that isn't one of the two patterns dash
 * actually uses.
 */

#include "uefi_process_compat.h"
#include <string.h>

/* POSIX-UEFI exposes these globals: BS (boot services), ST (system table),
 * RT (runtime services), and gImageHandle-equivalent via uefi_image_handle
 * (see uefi.h). Names kept exactly as POSIX-UEFI defines them so this file
 * compiles unmodified against the upstream header. */

/* ------------------------------------------------------------------ *
 *  1. External command execution: LoadImage + StartImage
 * ------------------------------------------------------------------ */

int uefi_spawn_external(const char_t *path, char_t *const argv[],
                         char_t *const envp[], uefi_proc_result_t *result)
{
    efi_handle_t child = NULL;
    efi_status_t st;
    efi_device_path_t *dp;
    efi_loaded_image_protocol_t *li = NULL;
    uintn_t exit_size = 0;
    void   *load_options = NULL;
    uintn_t load_options_size = 0;

    (void)envp; /* UEFI has no per-process env inheritance; POSIX-UEFI
                   emulates getenv/setenv via a global NVRAM-backed table
                   that a freshly loaded image already sees, so envp is
                   not marshalled explicitly -- documented limitation. */

    memset(result, 0, sizeof(*result));

    dp = uefi_path_to_dp(path); /* POSIX-UEFI helper: char_t* -> device path */
    if (!dp) {
        result->exited = 0;
        return -1;
    }

    st = BS->LoadImage(FALSE, uefi_image_handle, dp, NULL, 0, &child);
    uefi_free_dp(dp);
    if (EFI_ERROR(st)) {
        result->exited = 0;
        result->efi_status = st;
        return -1;
    }

    /* Marshal argv into the LoadOptions blob using POSIX-UEFI's own
     * convention, so a child built with POSIX-UEFI sees a normal
     * int argc, char **argv in its main(). */
    if (argv) {
        load_options = uefi_build_load_options(argv, &load_options_size);
        if (load_options) {
            st = BS->OpenProtocol(child, &EFI_LOADED_IMAGE_PROTOCOL_GUID,
                                   (void **)&li, uefi_image_handle, NULL,
                                   EFI_OPEN_PROTOCOL_GET_PROTOCOL);
            if (!EFI_ERROR(st) && li) {
                li->LoadOptions     = load_options;
                li->LoadOptionsSize = (uint32_t)load_options_size;
            }
        }
    }

    /* This is the whole trick: StartImage blocks until the child returns
     * from its main()/exits, exactly like waitpid() with no WNOHANG.
     * There is no parent/child race to reason about because there is
     * only ever one instruction stream. */
    st = BS->StartImage(child, &exit_size, NULL);

    result->exited      = 1;
    result->efi_status  = st;
    result->exit_status = EFI_ERROR(st) ? (int)(st & 0xFF) : 0;

    BS->UnloadImage(child);
    if (load_options) uefi_free(load_options);

    return 0;
}

/* ------------------------------------------------------------------ *
 *  2. Subshell isolation via save/restore, no second address space
 * ------------------------------------------------------------------ */

/* These three hooks are expected to be provided by the dash integration
 * layer (patches/shstate_bridge.c), since only dash knows the concrete
 * shape of its variable table / redirection stack. Kept as weak/extern
 * so this file has no compile-time dependency on dash internals. */
extern char_t *shstate_get_cwd(void);
extern int      shstate_set_cwd(const char_t *cwd);
extern void    *shstate_snapshot_vars(void);
extern void     shstate_restore_vars(void *snapshot);
extern void     shstate_free_snapshot(void *snapshot);

void uefi_shell_state_save(uefi_shell_state_t *out)
{
    memset(out, 0, sizeof(*out));
    out->saved_cwd    = shstate_get_cwd();
    out->saved_vartab = shstate_snapshot_vars();
    /* fd fields are placeholders for the redirection-stack bridge;
     * populated by patches/shstate_bridge.c once wired to dash's own
     * redirect save/restore (dash already has this machinery for its
     * *builtin* redirection handling -- reuse it, don't reinvent it). */
}

void uefi_shell_state_restore(const uefi_shell_state_t *saved)
{
    if (saved->saved_cwd) {
        shstate_set_cwd(saved->saved_cwd);
        uefi_free(saved->saved_cwd);
    }
    if (saved->saved_vartab) {
        shstate_restore_vars(saved->saved_vartab);
        shstate_free_snapshot(saved->saved_vartab);
    }
}

int uefi_run_subshell(int (*body)(void *ctx), void *ctx,
                       uefi_proc_result_t *result)
{
    uefi_shell_state_t saved;
    int rc;

    memset(result, 0, sizeof(*result));

    uefi_shell_state_save(&saved);
    rc = body(ctx);              /* runs synchronously, same stack */
    uefi_shell_state_restore(&saved);

    result->exited      = 1;
    result->exit_status = rc & 0xFF;
    return 0;
}

/* ------------------------------------------------------------------ *
 *  3. Pipeline stage buffering (non-streaming)
 * ------------------------------------------------------------------ */

#define UEFI_PIPE_INITIAL_CAP 4096

void uefi_pipe_buf_init(uefi_pipe_buf_t *buf)
{
    buf->data = (uint8_t *)uefi_malloc(UEFI_PIPE_INITIAL_CAP);
    buf->len  = 0;
    buf->cap  = buf->data ? UEFI_PIPE_INITIAL_CAP : 0;
}

void uefi_pipe_buf_free(uefi_pipe_buf_t *buf)
{
    if (buf->data) uefi_free(buf->data);
    buf->data = NULL;
    buf->len = buf->cap = 0;
}

static int pipe_buf_grow(uefi_pipe_buf_t *buf, uintn_t need)
{
    uintn_t new_cap;
    uint8_t *new_data;

    if (buf->len + need <= buf->cap) return 0;

    new_cap = buf->cap ? buf->cap * 2 : UEFI_PIPE_INITIAL_CAP;
    while (new_cap < buf->len + need) new_cap *= 2;

    new_data = (uint8_t *)uefi_malloc(new_cap);
    if (!new_data) return -1;

    if (buf->data) {
        memcpy(new_data, buf->data, buf->len);
        uefi_free(buf->data);
    }
    buf->data = new_data;
    buf->cap  = new_cap;
    return 0;
}

/* These two are provided by the dash integration layer: they redirect
 * the shell's notion of "current stdout" / "current stdin" to write into
 * / read from a uefi_pipe_buf_t instead of a real fd, reusing dash's own
 * existing output-redirection machinery (it already knows how to point
 * its output functions at an arbitrary sink for e.g. command substitution
 * "$(...)" -- a pipe stage is functionally the same problem). */
extern int shstate_redirect_stdout_to_buf(uefi_pipe_buf_t *buf);
extern int shstate_redirect_stdin_from_buf(const uefi_pipe_buf_t *buf);
extern void shstate_redirect_restore(void);

int uefi_pipe_stage_capture(int (*producer)(void *ctx), void *ctx,
                             uefi_pipe_buf_t *out, uefi_proc_result_t *result)
{
    int rc;

    memset(result, 0, sizeof(*result));
    uefi_pipe_buf_init(out);

    if (shstate_redirect_stdout_to_buf(out) != 0) return -1;
    rc = producer(ctx);
    shstate_redirect_restore();

    result->exited      = 1;
    result->exit_status = rc & 0xFF;
    return 0;
}

int uefi_pipe_stage_feed(int (*consumer)(void *ctx), void *ctx,
                          const uefi_pipe_buf_t *in, uefi_proc_result_t *result)
{
    int rc;

    memset(result, 0, sizeof(*result));

    if (shstate_redirect_stdin_from_buf(in) != 0) return -1;
    rc = consumer(ctx);
    shstate_redirect_restore();

    result->exited      = 1;
    result->exit_status = rc & 0xFF;
    return 0;
}

/* pipe_buf_grow is used internally by the redirect bridge (not here
 * directly) -- silence unused-function warnings if the bridge isn't
 * linked in yet during incremental builds. */
static void (*_unused_ref)(uefi_pipe_buf_t *, uintn_t) __attribute__((unused)) = pipe_buf_grow;

/* ------------------------------------------------------------------ *
 *  4. Explicit-failure stubs
 * ------------------------------------------------------------------ */

int uefi_bg_unsupported(void)
{
    uefi_printf(L"uefi-shell: background jobs / job control are not "
                 L"supported (no scheduler in this build)\n");
    return -1;
}

int uefi_signal_noop(int signum, void *handler)
{
    (void)signum;
    (void)handler;
    return 0; /* pretend success so dash's init doesn't bail out */
}
