# Integrando el shim con dash

Este archivo describe, en base a la estructura real de dash
(`https://github.com/tklauser/dash`, espejo de
`git.kernel.org/pub/scm/utils/dash/dash.git`), qué archivos tocar y por qué.
No es un parche automático — dash cambia con el tiempo y aplicar esto a
ciegas contra una versión distinta puede no calzar línea por línea. Es una
guía de **dónde mirar** y **qué reemplazar por qué**.

## Los tres puntos de contacto reales

dash no llama `fork()` en cualquier lado — está concentrado en un puñado
de sitios. Son estos tres los que importan:

### 1. `jobs.c` — `forkshell()`

Esta es la función central que casi todo el resto de dash usa para crear
un proceso hijo (comandos externos, subshells, partes de un pipeline).
Internamente hace algo como:

```c
pid = fork();
if (pid == 0) {
    forkchild(...);   /* setea redirecciones, cae a exec o corre el nodo */
} else {
    forkparent(...);  /* registra el pid en la tabla de jobs */
}
```

**Reemplazo**: `forkshell()` deja de bifurcar. En su lugar, inspecciona
qué tipo de nodo va a correr (esto ya lo hace dash internamente vía el
parámetro `flags`/`n->type`) y llama directo a:

- `uefi_spawn_external()` si el nodo es un comando externo con un path
  resuelto (`NCMD` con `find_command()` exitoso).
- `uefi_run_subshell()` si el nodo es un `NSUBSHELL` o cualquier caso donde
  dash forkeaba solo para aislar estado, no para ejecutar un binario.

El valor de retorno que `forkshell()` normalmente entrega (el pid) se
reemplaza por un puntero/handle opaco a un `uefi_proc_result_t` ya
resuelto — porque en este modelo, cuando `forkshell()` retorna, el
"hijo" ya terminó. No hay pid que trackear para un `waitpid()` posterior.

### 2. `jobs.c` — `waitforjob()` / `dowait()`

Normalmente esperan a que un pid específico (o cualquiera) cambie de
estado. En este modelo, para el caso síncrono (que es el único soportado),
esto se vuelve casi un no-op: el resultado ya está en el
`uefi_proc_result_t` que `forkshell()` produjo. Reemplazar por una función
que simplemente traduce ese struct al formato de status que el resto de
dash espera (`WIFEXITED`/`WEXITSTATUS`), usando las macros
`UEFI_WIFEXITED`/`UEFI_WEXITSTATUS` del header.

Para el caso asíncrono (`cmd &`, luego `wait` sin argumentos, o
`wait %1`) — no hay nada que mapear. Redirigir a `uefi_bg_unsupported()`.

### 3. `eval.c` — `evalsubshell()` y `expbackq()` (command substitution)

`evalsubshell()` es el `(comando)` explícito. `expbackq()` es
`` `comando` `` / `$(comando)` — que dash implementa **con un pipe interno**,
no leyendo texto: corre el comando forkeado, con su stdout conectado a un
pipe, y lee del otro extremo.

**Reemplazo**: ambos casos calzan con `uefi_pipe_stage_capture()` — no con
`uefi_run_subshell()` a secas, porque necesitás capturar stdout, no solo
aislar estado. Este es el punto de contacto entre el header y la función
`shstate_redirect_stdout_to_buf()` que tenés que escribir vos mismo,
apoyándote en el mecanismo que dash *ya tiene* para redirección de output
en memoria (dash ya sabe hacer esto para heredocs y para su propio
manejo de `$(...)` — no es una capacidad nueva que agregar, es una que
hay que desviar hacia `uefi_pipe_buf_t` en vez de hacia un fd real).

## Lo que se vuelve inalcanzable, explícitamente

No hay forma honesta de soportar estos sin un scheduler real (ver la
conversación de diseño — esto es la frontera donde entra un microkernel,
no boot services de UEFI):

- `cmd &` (jobs en background reales)
- `jobs`, `bg`, `fg`, `wait %N` con un job específico todavía corriendo
- `SIGCHLD` y cualquier trap basado en señales
- Pipelines con un productor infinito (`yes | head`) — el stage `yes`
  nunca termina de llenar el buffer, así que `head` nunca arranca.

dash detecta la mayoría de estos casos en su propio parser/jobs.c; la
integración más limpia es que esas rutas llamen a `uefi_bg_unsupported()`
con un mensaje claro, en vez de fallar en silencio o colgarse.

## Funciones puente que hay que escribir (no vienen en el shim)

El shim (`uefi_process_compat.c`) declara pero no implementa:

```c
char_t *shstate_get_cwd(void);
int      shstate_set_cwd(const char_t *cwd);
void    *shstate_snapshot_vars(void);
void     shstate_restore_vars(void *snapshot);
void     shstate_free_snapshot(void *snapshot);
int      shstate_redirect_stdout_to_buf(uefi_pipe_buf_t *buf);
int      shstate_redirect_stdin_from_buf(const uefi_pipe_buf_t *buf);
void     shstate_redirect_restore(void);
```

Estas son deliberadamente responsabilidad de un `shstate_bridge.c` que
vive junto al fork de dash, no del shim genérico — porque su
implementación depende de las estructuras internas de dash
(`struct var`, `localvars`, el stack de redirecciones en `redir.c`), que
cambian entre versiones. El shim no debe conocer esos tipos.
