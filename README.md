# Fingerdash (compat shim) (Hecho con ayuda de la IA)

Capa de compatibilidad para correr el *patrón de llamadas* que usa
`dash` (fork/exec/wait, subshells, pipelines) arriba de UEFI Boot
Services vía [POSIX-UEFI](https://gitlab.com/bztsrc/posix-uefi), sin
MMU propia, sin scheduler, sin multiprocessing real.

No es "fork() para UEFI" en general — es el mapeo específico de los
pocos patrones que dash usa, sobre las dos cosas que UEFI Boot Services
ya te da gratis: `LoadImage`/`StartImage` (que es un `execve`+`wait`
síncrono nativo) y ejecución secuencial de un solo hilo (que hace que
"aislar un subshell" sea guardar/restaurar estado en vez de clonar
memoria).

## Estructura

```
compat/
  uefi_process_compat.h   API del shim
  uefi_process_compat.c   Implementación sobre BS->LoadImage/StartImage
patches/
  README.md               Guía de integración: qué tocar en jobs.c/eval.c de dash y por qué
.github/workflows/
  build.yml                CI: compila el shim contra POSIX-UEFI, deja el .efi como artifact
```

## Qué funciona con este modelo

- Comandos externos (`ls`, `cat`, cualquier `.efi` en disco) — `execve`
  real vía `LoadImage`/`StartImage`.
- Subshells `( ... )` y sustitución de comandos `` `...` `` / `$(...)`
  — vía guardar/restaurar estado, sin clonar memoria.
- Pipelines finitos (`cat f | grep x | sort`) — vía buffer intermedio en
  memoria, no streaming real.

## Qué NO funciona, por diseño, y por qué

- `cmd &`, `jobs`, `bg`, `fg` — necesitan un scheduler real.
- `SIGCHLD` / traps — UEFI Boot Services no tiene señales.
- Pipelines con productor infinito (`yes | head`) — sin streaming, el
  productor nunca termina de llenar el buffer.

Ver `patches/README.md` para el detalle de integración contra el
código fuente real de dash, y la conversación de diseño que originó
este enfoque para el razonamiento completo detrás de cada límite.

## Build

El workflow de CI clona POSIX-UEFI (GitLab) y el árbol de dash (mirror
en GitHub, solo como referencia para aplicar los patches de
`patches/README.md`) y compila el shim de forma standalone contra
POSIX-UEFI para verificar que enlaza. La integración completa contra
dash requiere aplicar a mano los redirects descritos en
`patches/README.md`, porque dependen de la versión exacta de dash que
uses.

```
git clone --depth 1 https://gitlab.com/bztsrc/posix-uefi.git
cp -r posix-uefi/uefi ./uefi
make   # usando compat/ + tu Makefile de POSIX-UEFI
```
