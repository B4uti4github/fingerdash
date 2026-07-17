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

/* POSIX-UEFI exposes these globals:
 *   BS   (efi_boot_services_t*)   — boot services table
 *   ST   (efi_system_table_t*)   — system table
 *   LIP  (efi_loaded_image_protocol_t*) — current image's Loaded Image Protocol
 *   IM   (efi_handle_t)           — current image handle
 * All names are exactly as POSIX-UEFI defines them. */

#define UEFI_LOADED_IMAGE_GUID_INIT   \
    { 0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B} }

/* Device-path node types used by uefi_path_to_dp() */
#define MEDIA_DEVICE_PATH_TYPE         4
#define MEDIA_FILEPATH_SUBTYPE         4
#define END_DEVICE_PATH_TYPE           0x7f
#define END_ENTIRE_DEVICE_PATH_SUBTYPE 0xff

/* ------------------------------------------------------------------ *
 *  Internal helpers
 * ------------------------------------------------------------------ */

/*
 * Convert a POSIX-style path (e.g. "\\path\\to\\app.efi") into a
 * UEFI device path.
 *
 * UEFI device-path format for a file:
 *   MEDIA_FILEPATH_DEVICE_PATH node + END_DEVICE_PATH node
 *
 * MEDIA_FILEPATH_DEVICE_PATH (Type 4, SubType 4):
 *   uint8_t  Type      = MEDIA_DEVICE_PATH_TYPE
 *   uint8_t  SubType   = MEDIA_FILEPATH_SUBTYPE
 *   uint8_t  Length[2] = little-endian total node size (incl. header)
 *   char_t   Path[]    = backslash-prefixed path, UEFI-native separator
 *   char_t   '\0'      = NUL terminator included in node
 */
static efi_device_path_t *uefi_path_to_dp(const char_t *path)
{
    size_t plen = 0;
    const char_t *p;

    if (!path || !*path) return NULL;

    for (p = path; *p; p++) plen++;

    size_t total = 4 + (plen + 1) * sizeof(char_t) + 4;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return NULL;
    memset(buf, 0, total);

    efi_device_path_t *fp = (efi_device_path_t *)buf;
    fp->Type    = MEDIA_DEVICE_PATH_TYPE;
    fp->SubType = MEDIA_FILEPATH_SUBTYPE;
    uint16_t fp_len = (uint16_t)(4 + (plen + 1) * sizeof(char_t));
    fp->Length[0] = (uint8_t)(fp_len & 0xff);
    fp->Length[1] = (uint8_t)((fp_len >> 8) & 0xff);

    char_t *dst = (char_t *)(buf + 4);
    dst[0] = (char_t)'\\';
    size_t i;
    for (i = 0; i < plen && i < 1024; i++) {
        dst[1 + i] = (path[i] == '/') ? (char_t)'\\' : path[i];
    }
    dst[1 + i] = 0; /* already zeroed above, being explicit */

    efi_device_path_t *end = (efi_device_path_t *)(buf + fp_len + 4);
    end->Type    = END_DEVICE_PATH_TYPE;
    end->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    end->Length[0] = (uint8_t)(sizeof(efi_device_path_t) & 0xff);
    end->Length[1] = (uint8_t)((sizeof(efi_device_path_t) >> 8) & 0xff);

    return (efi_device_path_t *)buf;
}

static void uefi_free_dp(efi_device_path_t *dp)
{
    free(dp);
}

/*
 * Marshal argv into the LoadOptions blob format UEFI expects:
 *   uint32_t  argc
 *   void     *argv[0]
 *   void     *argv[1]
 *   ...
 *   NULL
 *
 * Blob is allocated with malloc(); caller frees with free().
 */
static void *uefi_build_load_options(char_t *const argv[], uintn_t *out_size)
{
    int argc = 0;
    char_t *const *arg;
    for (arg = argv; arg && *arg; arg++) argc++;

    size_t ptrs_size = (size_t)(argc + 1) * sizeof(void *);
    size_t str_size  = 0;
    for (arg = argv; arg && *arg; arg++) {
        str_size += (strlen((const char *)*arg) + 1) * sizeof(char_t);
    }

    size_t total = 4 + ptrs_size + str_size;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return NULL;
    memset(buf, 0, total);

    buf[0] = (uint8_t)(argc & 0xff);
    buf[1] = (uint8_t)((argc >> 8) & 0xff);
    buf[2] = (uint8_t)((argc >> 16) & 0xff);
    buf[3] = (uint8_t)((argc >> 24) & 0xff);

    void **ptrs = (void **)(buf + 4);
    char_t *str_dst = (char_t *)(buf + 4 + ptrs_size);
    size_t offset = 0;

    for (int i = 0; i < argc; i++) {
        ptrs[i] = (void *)(buf + 4 + ptrs_size + offset);
        const char *src = (const char *)argv[i];
        size_t len = (strlen(src) + 1) * sizeof(char_t);
        memcpy(str_dst, src, len);
        offset += len;
    }
    ptrs[argc] = NULL; /* already zeroed */

    *out_size = (uintn_t)total;
    return buf;
}

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
    uintn_t load_options_size = 0;
    void *load_options = NULL;

    (void)envp; /* UEFI has no per-process env inheritance; POSIX-UEFI
                   emulates getenv/setenv via a global NVRAM-backed table
                   that a freshly loaded image already sees, so envp is
                   not marshalled explicitly -- documented limitation. */

    memset(result, 0, sizeof(*result));

    dp = uefi_path_to_dp(path);
    if (!dp) {
        result->exited = 0;
        return -1;
    }

    /* LoadImage: BootPolicy=0, ParentHandle=IM, FilePath=dp,
     * SourceBuffer=NULL, SourceSize=0, ImageHandle=&child */
    st = BS->LoadImage(0, IM, dp, NULL, 0, &child);
    uefi_free_dp(dp);
    if (EFI_ERROR(st)) {
        result->exited     = 0;
        result->efi_status = st;
        return -1;
    }

    /* Marshal argv into LoadOptions so child sees argc/argv in main() */
    if (argv) {
        load_options = uefi_build_load_options(argv, &load_options_size);
        if (load_options) {
            /* OpenProtocol on the child handle to get its Loaded Image
             * Protocol instance. Pass IM as AgentHandle, NULL as
             * ControllerHandle, and EFI_OPEN_PROTOCOL_GET_PROTOCOL so
             * we get a direct pointer to the protocol interface. */
            static efi_guid_t loaded_image_guid =
                UEFI_LOADED_IMAGE_GUID_INIT;

            st = BS->OpenProtocol(child, &loaded_image_guid,
                                  (void **)&li, IM, NULL,
                                  EFI_OPEN_PROTOCOL_GET_PROTOCOL);
            if (!EFI_ERROR(st) && li) {
                li->LoadOptions     = load_options;
                li->LoadOptionsSize = (uint32_t)load_options_size;
            }
        }
    }

    /* StartImage blocks until child exits — equivalent to waitpid().
     * exit_size receives optional ExitData from the child; we ignore it. */
    st = BS->StartImage(child, NULL, NULL);

    /* POSIX-UEFI doesn't provide UnloadImage as a function pointer in BS.
     * The child process has already exited at this point (StartImage
     * returned), so we just release LoadOptions if any. */
    if (load_options) free(load_options);

    result->exited      = 1;
    result->efi_status  = st;
    result->exit_status = EFI_ERROR(st) ? (int)(st & 0xFF) : 0;
    return 0;
}

/* ------------------------------------------------------------------ *
 *  2. Subshell isolation via save/restore, no second address space
 * ------------------------------------------------------------------ */

/* These five hooks are provided by the dash integration layer
 * (patches/shstate_bridge.c); kept as extern so this file
 * compiles standalone without dash internals. */
extern char_t *shstate_get_cwd(void);
extern int     shstate_set_cwd(const char_t *cwd);
extern void   *shstate_snapshot_vars(void);
extern void    shstate_restore_vars(void *snapshot);
extern void    shstate_free_snapshot(void *snapshot);

void uefi_shell_state_save(uefi_shell_state_t *out)
{
    memset(out, 0, sizeof(*out));
    out->saved_cwd    = shstate_get_cwd();
    out->saved_vartab = shstate_snapshot_vars();
}

void uefi_shell_state_restore(const uefi_shell_state_t *saved)
{
    if (saved->saved_cwd) {
        shstate_set_cwd(saved->saved_cwd);
        free(saved->saved_cwd);
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
    buf->data = (uint8_t *)malloc(UEFI_PIPE_INITIAL_CAP);
    buf->len  = 0;
    buf->cap  = buf->data ? UEFI_PIPE_INITIAL_CAP : 0;
}

void uefi_pipe_buf_free(uefi_pipe_buf_t *buf)
{
    if (buf->data) free(buf->data);
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

    new_data = (uint8_t *)malloc(new_cap);
    if (!new_data) return -1;

    if (buf->data) {
        memcpy(new_data, buf->data, buf->len);
        free(buf->data);
    }
    buf->data = new_data;
    buf->cap  = new_cap;
    return 0;
}

/* Provided by the dash integration layer: redirect dash's internal
 * stdout sink / stdin source to/from a uefi_pipe_buf_t, reusing
 * dash's existing command-substitution output-redirection machinery. */
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

/* pipe_buf_grow is called by shstate_bridge.c via extern; silence the
 * unused-function warning during incremental builds where the bridge
 * hasn't been linked yet. */
static int (*_unused_ref)(uefi_pipe_buf_t *, uintn_t)
    __attribute__((unused)) = pipe_buf_grow;

/* ------------------------------------------------------------------ *
 *  4. Explicit-failure stubs
 * ------------------------------------------------------------------ */

int uefi_bg_unsupported(void)
{
    printf("uefi-shell: background jobs / job control are not "
           "supported (no scheduler in this build)\n");
    return -1;
}

int uefi_signal_noop(int signum, void *handler)
{
    (void)signum;
    (void)handler;
    return 0; /* pretend success so dash's init doesn't bail out */
}