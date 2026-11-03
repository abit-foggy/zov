/*
 * Copyright (c) 2026 foggy (abit-foggy)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define _POSIX_C_SOURCE 200809L

#include <unistd.h>

#include "driver.h"
#include "common.h"

#include <sys/wait.h>
#include <sys/utsname.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef USE_LIBTCC
#include <libtcc.h>

/* tcc runtime dir: cwd (dev tree), then exe-relative installs */
static const char *tcc_runtime_dir(void)
{
    static char resolved[1024];
    if (access("vendor/tinycc/include", F_OK) == 0)
        return "vendor/tinycc";
    ssize_t n = -1;
#ifdef __APPLE__
    uint32_t bufsize = (uint32_t)sizeof(resolved);
    if (_NSGetExecutablePath(resolved, &bufsize) == 0)
        n = (ssize_t)strlen(resolved);
#else
    n = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
#endif
    if (n > 0) {
        resolved[n] = '\0';
        char *slash = strrchr(resolved, '/');
        if (slash) {
            *slash = '\0';
            static char cand[1024];
            if (snprintf(cand, sizeof(cand), "%s/../lib/zov/tinycc",
                         resolved) < (int)sizeof(cand)
                && access(cand, F_OK) == 0)
                return cand;
            if (snprintf(cand, sizeof(cand), "%s/vendor/tinycc",
                         resolved) < (int)sizeof(cand)
                && access(cand, F_OK) == 0)
                return cand;
        }
    }
    return "vendor/tinycc";
}
#endif

static int compile_via_cc_ex(const char *c_src, const char *out_path,
                             char **links, size_t nlinks, char **include_dirs,
                             size_t ninclude, int release)
{
    char cmd[2048];
    const char *cc_mode = release
                              ? "cc -x c -std=c99 -O3 -DNDEBUG"
                              : "cc -x c -std=c99 -O0 -g"
                                " -Wno-unused-variable"
                                " -Wno-unused-function"
                                " -Wno-unused-but-set-variable";
    size_t off = (size_t)snprintf(cmd, sizeof(cmd), "%s", cc_mode);
    for (size_t i = 0; i < ninclude && off < sizeof(cmd) - 256; i++)
        off += (size_t)snprintf(cmd + off, sizeof(cmd) - off,
                                " -I'%s'", include_dirs[i]);
    for (size_t i = 0; i < nlinks && off < sizeof(cmd) - 256; i++)
        off += (size_t)snprintf(cmd + off, sizeof(cmd) - off,
                                " -l%s", links[i]);
    // std::math and the C library both resolve through libm
    off += (size_t)snprintf(cmd + off, sizeof(cmd) - off,
                            " -lm -o '%s' -", out_path);
    if (off >= sizeof(cmd)) {
        failure("link command too long");
        return 1;
    }
    FILE *cc = popen(cmd, "w");
    if (!cc) {
        failure("failed to spawn cc");
        return 1;
    }
    fputs(c_src, cc);
    int st = pclose(cc);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        failure("cc failed with status %d", st);
        return 1;
    }
    return 0;
}

#ifdef USE_LIBTCC

/* libtcc must come from the vendored tree; the runtime resolves its own
 * headers and libtcc1.a relative to the lib path. */
static int compile_via_tcc_ex(const char *c_src, const char *out_path,
                              char **links, size_t nlinks,
                              char **include_dirs, size_t ninclude)
{
    TCCState *tcc = tcc_new();
    if (!tcc)
        return 1;
    /* {B} expands inside tcc_set_output_type, so the lib path must be
     * installed first */
    const char *trt = tcc_runtime_dir();
    tcc_set_lib_path(tcc, trt);
    if (tcc_set_output_type(tcc, TCC_OUTPUT_EXE) < 0) {
        tcc_delete(tcc);
        return 1;
    }
    {
        char incdir[1024];
        if (snprintf(incdir, sizeof(incdir), "%s/include", trt)
            < (int)sizeof(incdir))
            tcc_add_sysinclude_path(tcc, incdir);
    }
    tcc_add_sysinclude_path(tcc, "/usr/include");
    for (size_t i = 0; i < ninclude; i++)
        tcc_add_sysinclude_path(tcc, include_dirs[i]);
    tcc_add_library(tcc, "c");
    tcc_add_library(tcc, "m");
    for (size_t i = 0; i < nlinks; i++)
        tcc_add_library(tcc, links[i]);
    if (tcc_compile_string(tcc, c_src) < 0) {
        tcc_delete(tcc);
        return 1;
    }
    if (tcc_output_file(tcc, out_path) < 0) {
        tcc_delete(tcc);
        return 1;
    }
    tcc_delete(tcc);
    return 0;
}

#endif /* USE_LIBTCC */

/* The red zone exists only on x86-64.  Cross builds select the target with
 * ZOV_TARGET_ARCH; otherwise the host machine name is used. */
static int freestanding_is_x86_64(void)
{
    const char *arch = getenv("ZOV_TARGET_ARCH");
    struct utsname u;
    if (!arch && uname(&u) == 0)
        arch = u.machine;
    if (!arch)
        return 0;
    return strcmp(arch, "x86_64") == 0 || strcmp(arch, "amd64") == 0;
}

/* Freestanding backend flags; -c emits a linkable kernel object. */
static int compile_via_cc_fs(const char *c_src, const char *out_path)
{
    int to_object = strlen(out_path) > 2
                    && strcmp(out_path + strlen(out_path) - 2, ".o") == 0;
    char cmd[2048];
    size_t off = (size_t)snprintf(cmd, sizeof(cmd),
                                  "cc -x c -std=c99 -O2 -ffreestanding "
                                  "-nostdlib -fno-pie%s",
                                  freestanding_is_x86_64()
                                      ? " -mno-red-zone" : "");
    if (to_object)
        off += (size_t)snprintf(cmd + off, sizeof(cmd) - off, " -c");
    off += (size_t)snprintf(cmd + off, sizeof(cmd) - off,
                            " -o '%s' -", out_path);
    if (off >= sizeof(cmd)) {
        failure("freestanding link command too long");
        return 1;
    }
    FILE *cc = popen(cmd, "w");
    if (!cc) {
        failure("failed to spawn cc");
        return 1;
    }
    fputs(c_src, cc);
    int st = pclose(cc);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        failure("cc failed with status %d", st);
        return 1;
    }
    return 0;
}

int driver_compile_ex(const char *c_src, const char *out_path,
                      int emit_c_only, int force_cc, char **links,
                      size_t nlinks, char **include_dirs, size_t ninclude,
                      int freestanding)
{
    if (emit_c_only) {
        fputs(c_src, stdout);
        return 0;
    }
    if (freestanding) {
        /* bare-metal targets always take the portable cc pipe: libtcc's
         * hosted crt + libc wiring has no freestanding equivalent */
        return compile_via_cc_fs(c_src, out_path);
    }
#ifdef USE_LIBTCC
    if (!force_cc) {
        if (compile_via_tcc_ex(c_src, out_path, links, nlinks, include_dirs,
                               ninclude)
            == 0)
            return 0;
        /* libtcc failure falls through to the portable cc pipe */
    }
#else
    (void)force_cc;
#endif
    return compile_via_cc_ex(c_src, out_path, links, nlinks, include_dirs,
                             ninclude, force_cc);
}

int driver_compile(const char *c_src, const char *out_path,
                   int emit_c_only, int force_cc)
{
    return driver_compile_ex(c_src, out_path, emit_c_only, force_cc, NULL,
                             0, NULL, 0, 0);
}

/* ---- zovc cc: drop-in C99 compiler driver ---- */

#ifdef USE_LIBTCC

static int cc_via_tcc(int argc, char **argv)
{
    TCCState *tcc = tcc_new();
    if (!tcc)
        return 1;
    const char *trt = tcc_runtime_dir();
    tcc_set_lib_path(tcc, trt);

    char incdir[1024];
    if (snprintf(incdir, sizeof(incdir), "%s/include", trt)
        < (int)sizeof(incdir))
        tcc_add_sysinclude_path(tcc, incdir);
    tcc_add_sysinclude_path(tcc, "/usr/include");
    tcc_add_sysinclude_path(tcc, "/usr/local/include");
    tcc_add_sysinclude_path(tcc, "/usr/lib/gcc/x86_64-linux-gnu/12/include");
    tcc_add_sysinclude_path(tcc,
                            "/usr/lib/gcc/x86_64-linux-gnu/13/include");
    tcc_add_sysinclude_path(tcc,
                            "/usr/lib/gcc/x86_64-linux-gnu/14/include");

    const char *out = "a.out";
    int output_type = TCC_OUTPUT_EXE;
    const char *files[256];
    const char *libs[64];
    int nfiles = 0;
    int nlibs = 0;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-c") == 0) {
            output_type = TCC_OUTPUT_OBJ;
        } else if (strcmp(a, "-shared") == 0) {
            output_type = TCC_OUTPUT_DLL;
        } else if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strncmp(a, "-o", 2) == 0 && a[2]) {
            out = a + 2;
        } else if (strncmp(a, "-I", 2) == 0) {
            if (a[2]) {
                tcc_add_include_path(tcc, a + 2);
            } else if (i + 1 < argc) {
                tcc_add_include_path(tcc, argv[++i]);
            }
        } else if (strncmp(a, "-D", 2) == 0) {
            const char *def = a + 2;
            if (!*def && i + 1 < argc)
                def = argv[++i];
            const char *eq = strchr(def, '=');
            if (eq) {
                char name[256];
                size_t nlen = (size_t)(eq - def);
                if (nlen >= sizeof(name))
                    nlen = sizeof(name) - 1;
                memcpy(name, def, nlen);
                name[nlen] = '\0';
                tcc_define_symbol(tcc, name, eq + 1);
            } else {
                tcc_define_symbol(tcc, def, "1");
            }
        } else if (strncmp(a, "-L", 2) == 0) {
            if (a[2]) {
                tcc_add_library_path(tcc, a + 2);
            } else if (i + 1 < argc) {
                tcc_add_library_path(tcc, argv[++i]);
            }
        } else if (strncmp(a, "-l", 2) == 0) {
            if (a[2]) {
                if (nlibs < 64) libs[nlibs++] = a + 2;
            } else if (i + 1 < argc) {
                if (nlibs < 64) libs[nlibs++] = argv[++i];
            }
        } else if (a[0] == '-') {
            // optimization, warning, and debug flags are accepted silently
        } else if (nfiles < 256) {
            files[nfiles++] = a;
        }
    }

    if (nfiles == 0) {
        fprintf(stderr, "zovc cc: no input files\n");
        tcc_delete(tcc);
        return 1;
    }

    tcc_set_output_type(tcc, output_type);
    if (output_type != TCC_OUTPUT_OBJ) {
        tcc_add_library(tcc, "c");
        tcc_add_library(tcc, "m");
    }
    for (int i = 0; i < nlibs; i++)
        tcc_add_library(tcc, libs[i]);
    for (int i = 0; i < nfiles; i++) {
        if (tcc_add_file(tcc, files[i]) < 0) {
            fprintf(stderr, "zovc cc: failed to process '%s'\n", files[i]);
            tcc_delete(tcc);
            return 1;
        }
    }

    int rc = tcc_output_file(tcc, out) < 0 ? 1 : 0;
    tcc_delete(tcc);
    return rc;
}

#else

static int cc_via_cc_pipe(int argc, char **argv)
{
    char cmd[4096] = "cc";
    size_t off = 2;
    for (int i = 0; i < argc && off < sizeof(cmd) - 256; i++)
        off += (size_t)snprintf(cmd + off, sizeof(cmd) - off,
                                " '%s'", argv[i]);
    FILE *p = popen(cmd, "w");
    if (!p)
        return 1;
    int st = pclose(p);
    return (!WIFEXITED(st) || WEXITSTATUS(st) != 0) ? 1 : 0;
}

#endif /* USE_LIBTCC */

int driver_run_cc(int argc, char **argv)
{
    if (argc == 0) {
        fprintf(stderr, "usage: zovc cc [cc-flags] <files.c...>\n");
        return 1;
    }
#ifdef USE_LIBTCC
    return cc_via_tcc(argc, argv);
#else
    return cc_via_cc_pipe(argc, argv);
#endif
}
