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

#include "driver.h"
#include "parser.h"
#include "sema.h"
#include "codegen.h"

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fatal("cannot open '%s'", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fatal("cannot seek '%s'", path);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fatal("cannot stat '%s'", path);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fatal("out of memory");
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fatal("short read on '%s'", path);
        return NULL;
    }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [options] file.zov\n"
            "options:\n"
            "  --emit-c     print generated ISO C99 to stdout\n"
            "  --release    use the system cc pipe even with libtcc\n"
            "  -o <path>    output executable path (default a.out)\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *file = NULL;
    const char *out = "a.out";
    int emit_c = 0;
    int force_cc = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--emit-c") == 0) {
            emit_c = 1;
        } else if (strcmp(argv[i], "--release") == 0) {
            force_cc = 1;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return 1;
            }
            out = argv[i];
        } else if (strcmp(argv[i], "-h") == 0
                   || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "zovc: unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else if (!file) {
            file = argv[i];
        } else {
            fprintf(stderr, "zovc: multiple input files are unsupported\n");
            return 1;
        }
    }
    if (!file) {
        usage(argv[0]);
        return 1;
    }

    char *src = read_file(file);
    Program prog = {0};
    parse_program(src, file, &prog);
    sema_check(&prog);
    char *c_src = codegen_generate(&prog);
    int rc = driver_compile(c_src, out, emit_c, force_cc);
    free(c_src);
    free(src);
    return rc;
}
