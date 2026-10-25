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

#include "driver.h"
#include "common.h"

#include <sys/wait.h>

#ifdef USE_LIBTCC
#include <libtcc.h>
#endif

#define ZOV_TCC_DIR "vendor/tinycc"

static int compile_via_cc_ex(const char *c_src, const char *out_path,
                             char **links, size_t nlinks, char **include_dirs,
                             size_t ninclude)
{
    char cmd[2048];
    size_t off = (size_t)snprintf(cmd, sizeof(cmd),
                                  "cc -x c -std=c99 -O2");
    for (size_t i = 0; i < ninclude && off < sizeof(cmd) - 256; i++)
        off += (size_t)snprintf(cmd + off, sizeof(cmd) - off,
                                " -I'%s'", include_dirs[i]);
    for (size_t i = 0; i < nlinks && off < sizeof(cmd) - 256; i++)
        off += (size_t)snprintf(cmd + off, sizeof(cmd) - off,
                                " -l%s", links[i]);
    off += (size_t)snprintf(cmd + off, sizeof(cmd) - off,
                            " -o '%s' -", out_path);
    if (off >= sizeof(cmd)) {
        fatal("link command too long");
        return 1;
    }
    FILE *cc = popen(cmd, "w");
    if (!cc) {
        fatal("failed to spawn cc");
        return 1;
    }
    fputs(c_src, cc);
    int st = pclose(cc);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fatal("cc failed with status %d", st);
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
    tcc_set_lib_path(tcc, ZOV_TCC_DIR);
    if (tcc_set_output_type(tcc, TCC_OUTPUT_EXE) < 0) {
        tcc_delete(tcc);
        return 1;
    }
    tcc_add_sysinclude_path(tcc, ZOV_TCC_DIR "/include");
    tcc_add_sysinclude_path(tcc, "/usr/include");
    for (size_t i = 0; i < ninclude; i++)
        tcc_add_sysinclude_path(tcc, include_dirs[i]);
    tcc_add_library(tcc, "c");
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

/* Freestanding backend flags; -c emits a linkable kernel object. */
static int compile_via_cc_fs(const char *c_src, const char *out_path)
{
    int to_object = strlen(out_path) > 2
                    && strcmp(out_path + strlen(out_path) - 2, ".o") == 0;
    char cmd[2048];
    size_t off = (size_t)snprintf(cmd, sizeof(cmd),
                                  "cc -x c -std=c99 -O2 -ffreestanding "
                                  "-nostdlib -mno-red-zone -fno-pie");
    if (to_object)
        off += (size_t)snprintf(cmd + off, sizeof(cmd) - off, " -c");
    off += (size_t)snprintf(cmd + off, sizeof(cmd) - off,
                            " -o '%s' -", out_path);
    if (off >= sizeof(cmd)) {
        fatal("freestanding link command too long");
        return 1;
    }
    FILE *cc = popen(cmd, "w");
    if (!cc) {
        fatal("failed to spawn cc");
        return 1;
    }
    fputs(c_src, cc);
    int st = pclose(cc);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fatal("cc failed with status %d", st);
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
                             ninclude);
}

int driver_compile(const char *c_src, const char *out_path,
                   int emit_c_only, int force_cc)
{
    return driver_compile_ex(c_src, out_path, emit_c_only, force_cc, NULL,
                             0, NULL, 0, 0);
}
