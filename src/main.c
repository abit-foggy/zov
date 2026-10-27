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
#include "cimport.h"
#include "pkg.h"
#include "parser.h"
#include "modules.h"
#include "sema.h"
#include "borrow.h"
#include "codegen.h"

#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* ---- workspace subcommands ---- */

static int cmd_init(const char *dir);
static int cmd_project(const char *dir, int release, int test_mode,
                       const char *run_args, int check_only);

static void mkdirs(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void write_file_if_absent(const char *path, const char *text)
{
    if (access(path, F_OK) == 0)
        return;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "zovc: cannot write '%s'\n", path);
        exit(1);
    }
    fputs(text, f);
    fclose(f);
}

static int cmd_init(const char *dir)
{
    char sub[600];
    mkdirs(dir);
    snprintf(sub, sizeof(sub), "%s/src", dir);
    mkdirs(sub);
    char name[128];
    const char *base = strrchr(dir, '/');
    base = base ? base + 1 : dir;
    snprintf(name, sizeof(name), "%s", base);
    char toml[1024];
    snprintf(toml, sizeof(toml),
             "[package]\nname = \"%s\"\nversion = \"0.1.0\"\n"
             "entry = \"src/main.zov\"\n\n[dependencies]\n\n"
             "[c-libraries]\nlinks = [\"m\"]\n",
             name);
    snprintf(sub, sizeof(sub), "%s/zov.toml", dir);
    write_file_if_absent(sub, toml);
    snprintf(sub, sizeof(sub), "%s/src/main.zov", dir);
    {
        char starter[512];
        snprintf(starter, sizeof(starter),
                 "export fn main() i32 = {\n"
                 "    println(\"hello from %s\");\n"
                 "    return 0;\n"
                 "};\n"
                 "\n"
                 "test \"sanity\" {\n"
                 "    assert(1 + 1 == 2);\n"
                 "};\n",
                 name);
        write_file_if_absent(sub, starter);
    }
    snprintf(sub, sizeof(sub), "%s/.gitignore", dir);
    write_file_if_absent(sub, "/target/\n.zov-cache/\n");
    printf("initialized zov package '%s'\n", name);
    return 0;
}

/* build (and optionally run) a zov.toml project */
static int cmd_project(const char *dir, int release, int test_mode,
                       const char *run_args, int check_only)
{
    char toml[600];
    snprintf(toml, sizeof(toml), "%s/zov.toml", dir);
    Pkg pkg;
    if (pkg_load(&pkg, toml) != 0) {
        fprintf(stderr, "zovc: cannot load '%s'\n", toml);
        return 1;
    }
    for (size_t i = 0; i < pkg.include_dirs.len; i++)
        cimport_add_include_dir(pkg.include_dirs.items[i]);
    char entry[1024];
    snprintf(entry, sizeof(entry), "%s/%s", pkg.dir, pkg.entry);
    char *src = read_file(entry);
    if (!src) {
        fprintf(stderr, "zovc: cannot open entry '%s'\n", entry);
        return 1;
    }
    Program prog = {0};
    prog.freestanding = pkg.freestanding;
    parse_program(src, entry, &prog);
    modules_load(&prog, entry, &pkg);
    sema_check(&prog, test_mode);
    borrow_check(&prog);
    if (check_only) {
        /* frontend only: no codegen, no lock, no target dir writes */
        free(src);
        pkg_free(&pkg);
        return 0;
    }
    char target[600];
    snprintf(target, sizeof(target), "%s/target", pkg.dir);
    mkdirs(target);
    char out[900];
    if (test_mode)
        snprintf(out, sizeof(out), "%s/%s_tests", target, pkg.name);
    else if (pkg.freestanding)
        /* kernel objects link via the platform's own linker scripts */
        snprintf(out, sizeof(out), "%s/%s.o", target, pkg.name);
    else
        snprintf(out, sizeof(out), "%s/%s", target, pkg.name);
    char *c_src = codegen_generate_mode(&prog, test_mode);
    int rc = driver_compile_ex(c_src, out, 0, release, pkg.links.items,
                               pkg.links.len, pkg.include_dirs.items,
                               pkg.include_dirs.len, pkg.freestanding);
    free(c_src);
    free(src);
    if (rc != 0)
        return rc;
    if (test_mode) {
        if (pkg_write_lock(&pkg) != 0)
            return 1;
        char cmd[1200];
        snprintf(cmd, sizeof(cmd), "'%s'%s", out,
                 run_args ? run_args : "");
        int st = system(cmd);
        pkg_free(&pkg);
        return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    }
    if (pkg_write_lock(&pkg) != 0)
        return 1;
    printf("built %s\n", out);
    if (run_args != NULL) {
        char cmd[1200];
        snprintf(cmd, sizeof(cmd), "'%s'%s", out, run_args);
        int st = system(cmd);
        pkg_free(&pkg);
        return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    }
    pkg_free(&pkg);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [options] file.zov\n"
            "options:\n"
            "  --emit-c     print generated ISO C99 to stdout\n"
            "  --check      run the frontend only, exit 0/1 with "
            "diagnostics\n"
            "  --freestanding  bare-metal build: no hosted libc, kernel "
            "entry\n"
            "  --release    use the system cc pipe even with libtcc\n"
            "  --version    print the compiler version\n"
            "  -o <path>    output executable path (default a.out)\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *file = NULL;
    const char *out = "a.out";
    int emit_c = 0;
    int force_cc = 0;
    int test_mode = 0;
    int check_only = 0;
    int freestanding = 0;

    if (argc >= 2 && strcmp(argv[1], "init") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: zovc init <dir>\n");
            return 1;
        }
        return cmd_init(argv[2]);
    }
    if (argc >= 2 && strcmp(argv[1], "build") == 0) {
        const char *dir = ".";
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-C") == 0 && i + 1 < argc)
                dir = argv[++i];
        }
        int release = 0;
        for (int i = 2; i < argc; i++)
            if (strcmp(argv[i], "--release") == 0)
                release = 1;
        return cmd_project(dir, release, 0, NULL, 0);
    }
    if (argc >= 2 && strcmp(argv[1], "run") == 0) {
        const char *dir = ".";
        int ai = 2;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
                dir = argv[++i];
                ai = i + 1;
            }
        }
        int release = 0;
        for (int i = ai; i < argc; i++)
            if (strcmp(argv[i], "--release") == 0)
                release = 1;
        char runargs[1024] = "";
        for (int i = ai; i < argc; i++) {
            if (strcmp(argv[i], "--release") == 0)
                continue;
            size_t len = strlen(runargs);
            snprintf(runargs + len, sizeof(runargs) - len, " %s",
                     argv[i]);
        }
        return cmd_project(dir, release, 0, runargs, 0);
    }
    if (argc >= 2 && strcmp(argv[1], "check") == 0) {
        if (argc >= 3 && strstr(argv[2], ".zov")) {
            file = argv[2];
            check_only = 1;
        } else {
            const char *dir = ".";
            for (int i = 2; i < argc; i++) {
                if (strcmp(argv[i], "-C") == 0 && i + 1 < argc)
                    dir = argv[++i];
            }
            return cmd_project(dir, 0, 0, NULL, 1);
        }
    }
    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        if (argc >= 3 && strstr(argv[2], ".zov")) {
            /* single-file test: flow through the direct pipeline */
            file = argv[2];
            out = ".zov-test-bin";
            test_mode = 1;
        } else {
        const char *dir = ".";
            for (int i = 2; i < argc; i++) {
                if (strcmp(argv[i], "-C") == 0 && i + 1 < argc)
                    dir = argv[++i];
            }
            return cmd_project(dir, 0, 1, NULL, 0);
        }
    }


    if (argc >= 2 && (strcmp(argv[1], "--version") == 0
                       || strcmp(argv[1], "-V") == 0)) {
        printf("zovc 1.4.0\n");
        return 0;
    }

    for (int i = file ? 2 : 1; i < argc; i++) {
        if (strcmp(argv[i], "--emit-c") == 0) {
            emit_c = 1;
        } else if (strcmp(argv[i], "--test") == 0) {
            test_mode = 1;
        } else if (strcmp(argv[i], "--check") == 0) {
            check_only = 1;
        } else if (strcmp(argv[i], "--freestanding") == 0) {
            freestanding = 1;
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
        } else if (strcmp(argv[i], file) != 0) {
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
    prog.freestanding = freestanding;
    parse_program(src, file, &prog);
    modules_load(&prog, file, NULL);
    sema_check(&prog, test_mode);
    borrow_check(&prog);
    if (check_only) {
        /* frontend pass complete: diagnostics already on stderr */
        free(src);
        return 0;
    }
    char *c_src = codegen_generate_mode(&prog, test_mode);
    int rc = driver_compile_ex(c_src, out, emit_c, force_cc, NULL, 0,
                               NULL, 0, freestanding);
    free(c_src);
    free(src);
    if (rc == 0 && test_mode) {
        /* test mode: execute the synthesized runner and forward status */
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "./%s", out);
        int st = system(cmd);
        remove(out);
        if (WIFEXITED(st))
            return WEXITSTATUS(st);
        return 1;
    }
    return rc;
}
