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
#include "diag.h"
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
        failure("cannot open '%s'", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        failure("cannot seek '%s'", path);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        failure("cannot stat '%s'", path);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        failure("out of memory");
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        failure("short read on '%s'", path);
        return NULL;
    }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* ---- workspace subcommands ---- */

typedef struct CliPkg {
    char name[128];
    char path[512];
} CliPkg;

static void copy_clamped(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int parse_cli_pkg(const char *arg, CliPkg *out)
{
    const char *eq = strchr(arg, '=');
    if (!eq)
        return 0;
    size_t n = (size_t)(eq - arg);
    if (n >= sizeof(out->name))
        return 0;
    memcpy(out->name, arg, n);
    out->name[n] = '\0';
    copy_clamped(out->path, sizeof(out->path), eq + 1);
    return 1;
}

static void parse_ver(const char *s, int *maj, int *min, int *pat)
{
    *maj = *min = *pat = 0;
    if (!s) return;
    *maj = atoi(s);
    const char *d1 = strchr(s, '.');
    if (d1) {
        *min = atoi(d1 + 1);
        const char *d2 = strchr(d1 + 1, '.');
        if (d2)
            *pat = atoi(d2 + 1);
    }
}

static int semver_ge(const char *cur, const char *req)
{
    int cmaj, cmin, cpat;
    int rmaj, rmin, rpat;
    parse_ver(cur, &cmaj, &cmin, &cpat);
    parse_ver(req, &rmaj, &rmin, &rpat);
    if (cmaj != rmaj) return cmaj > rmaj;
    if (cmin != rmin) return cmin > rmin;
    return cpat >= rpat;
}

static void check_edition(const char *ed)
{
    if (ed && ed[0]) {
        if (strcmp(ed, "2026") != 0) {
            diag_fatal_global("E0903", "unsupported edition '%s'; supported editions: 2026", ed);
        }
    }
}

static int cmd_init(const char *dir);
static int cmd_project(const char *dir, int release, int test_mode,
                       const char *run_args, int check_only,
                       const char *cli_edition,
                       CliPkg *cli_pkgs, size_t cli_pkgs_len);

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
    if (!dir || !*dir)
        dir = ".";
    mkdirs(dir);
    char sub[600];
    snprintf(sub, sizeof(sub), "%s/src", dir);
    mkdirs(sub);
    char name[128] = "app";
    char dirbuf[512];
    copy_clamped(dirbuf, sizeof(dirbuf), dir);
    size_t dlen = strlen(dirbuf);
    while (dlen > 1 && dirbuf[dlen - 1] == '/') {
        dirbuf[dlen - 1] = '\0';
        dlen--;
    }
    if (strcmp(dirbuf, ".") == 0) {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd))) {
            const char *base = strrchr(cwd, '/');
            base = (base && base[1]) ? base + 1 : cwd;
            copy_clamped(name, sizeof(name), base);
        }
    } else {
        const char *base = strrchr(dirbuf, '/');
        base = (base && base[1]) ? base + 1 : dirbuf;
        copy_clamped(name, sizeof(name), base);
    }
    char toml[1024];
    snprintf(toml, sizeof(toml),
             "[package]\n"
             "name = \"%s\"\n"
             "version = \"0.1.0\"\n"
             "edition = \"2026\"\n"
             "zovc = \"0.10.0\"\n\n"
             "[dependencies]\n",
             name);
    snprintf(sub, sizeof(sub), "%s/zov.toml", dir);
    write_file_if_absent(sub, toml);
    snprintf(sub, sizeof(sub), "%s/src/main.zov", dir);
    {
        char starter[512];
        snprintf(starter, sizeof(starter),
                 "use std::io;\n"
                 "\n"
                 "export fn main() i32 = {\n"
                 "    io::println(\"hello from %s\");\n"
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
                       const char *run_args, int check_only,
                       const char *cli_edition,
                       CliPkg *cli_pkgs, size_t cli_pkgs_len)
{
    char toml[600];
    snprintf(toml, sizeof(toml), "%s/zov.toml", dir);
    Pkg pkg;
    if (pkg_load(&pkg, toml) != 0) {
        fprintf(stderr, "zovc: cannot load '%s'\n", toml);
        return 1;
    }

    const char *ed = (cli_edition && cli_edition[0]) ? cli_edition : pkg.edition;
    check_edition(ed);

    if (pkg.min_zovc[0]) {
        if (!semver_ge("0.10.0", pkg.min_zovc)) {
            fprintf(stderr, "zovc: manifest error: package requires zovc >= %s, "
                            "but active compiler is 0.10.0; please update zovc or "
                            "run 'sklad toolchain'\n", pkg.min_zovc);
            pkg_free(&pkg);
            return 1;
        }
    }

    for (size_t i = 0; i < cli_pkgs_len; i++) {
        PkgDep *existing = pkg_find_dep(&pkg, cli_pkgs[i].name);
        if (existing) {
            snprintf(existing->path, sizeof(existing->path), "%s", cli_pkgs[i].path);
        } else {
            PkgDep d;
            memset(&d, 0, sizeof(d));
            snprintf(d.name, sizeof(d.name), "%s", cli_pkgs[i].name);
            snprintf(d.path, sizeof(d.path), "%s", cli_pkgs[i].path);
            da_push(&pkg.deps, d);
        }
    }

    for (size_t i = 0; i < pkg.deps.len; i++) {
        PkgDep *d = &pkg.deps.items[i];
        if (d->path[0] != '\0')
            continue;
        if (d->git[0] != '\0' || d->url[0] != '\0') {
            char cand[1024];
            snprintf(cand, sizeof(cand), "%s/.sklad/deps/%s", pkg.dir, d->name);
            if (access(cand, F_OK) == 0) {
                copy_clamped(d->path, sizeof(d->path), cand);
                continue;
            }
            snprintf(cand, sizeof(cand), "%s/.sklad/cache/git/%s", pkg.dir, d->name);
            if (access(cand, F_OK) == 0) {
                copy_clamped(d->path, sizeof(d->path), cand);
                continue;
            }
            fprintf(stderr, "zovc: error: dependency '%s' requires network access. "
                            "Run 'sklad fetch' or 'sklad build' to fetch dependencies, "
                            "or map locally with --pkg\n", d->name);
            pkg_free(&pkg);
            return 1;
        }
    }

    for (size_t i = 0; i < pkg.include_dirs.len; i++)
        cimport_add_include_dir(pkg.include_dirs.items[i]);
    char entry[1024];
    snprintf(entry, sizeof(entry), "%s/%s", pkg.dir, pkg.entry);
    char *src = read_file(entry);
    if (!src) {
        fprintf(stderr, "zovc: cannot open entry '%s'\n", entry);
        pkg_free(&pkg);
        return 1;
    }
    Program prog = {0};
    prog.freestanding = pkg.freestanding;
    prog.debug = !release;
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
    if (rc != 0) {
        pkg_free(&pkg);
        return rc;
    }
    if (test_mode) {
        if (pkg_write_lock(&pkg) != 0) {
            pkg_free(&pkg);
            return 1;
        }
        char cmd[1200];
        snprintf(cmd, sizeof(cmd), "'%s'%s", out,
                 run_args ? run_args : "");
        int st = system(cmd);
        pkg_free(&pkg);
        return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    }
    if (pkg_write_lock(&pkg) != 0) {
        pkg_free(&pkg);
        return 1;
    }
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
            "  explain <CODE>     explain a diagnostic code\n"
            "  diagnostics        list every diagnostic code\n"
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
    int release = 0;
    const char *cli_edition = NULL;
    DA(CliPkg) cli_pkgs = {0};

    if (argc >= 2 && strcmp(argv[1], "init") == 0) {
        const char *dir = ".";
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-C") == 0 && i + 1 < argc)
                dir = argv[++i];
            else if (argv[i][0] != '-')
                dir = argv[i];
        }
        return cmd_init(dir);
    }
    if (argc >= 2 && strcmp(argv[1], "explain") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: zovc explain <CODE>\n");
            return 1;
        }
        return diag_print_explanation(argv[2]);
    }
    if (argc >= 2 && strcmp(argv[1], "diagnostics") == 0) {
        diag_list_codes(stdout);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "cc") == 0)
        return driver_run_cc(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "build") == 0) {
        const char *dir = ".";
        int rel = 0;
        const char *edition = NULL;
        DA(CliPkg) pkgs = {0};
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
                dir = argv[++i];
            } else if (strcmp(argv[i], "--release") == 0 || strcmp(argv[i], "--prod") == 0) {
                rel = 1;
            } else if (strcmp(argv[i], "--edition") == 0 && i + 1 < argc) {
                edition = argv[++i];
            } else if (strncmp(argv[i], "--edition=", 10) == 0) {
                edition = argv[i] + 10;
            } else if (strcmp(argv[i], "--pkg") == 0 && i + 1 < argc) {
                CliPkg cp;
                if (parse_cli_pkg(argv[++i], &cp))
                    da_push(&pkgs, cp);
            } else if (strncmp(argv[i], "--pkg=", 6) == 0) {
                CliPkg cp;
                if (parse_cli_pkg(argv[i] + 6, &cp))
                    da_push(&pkgs, cp);
            }
        }
        int rc = cmd_project(dir, rel, 0, NULL, 0, edition, pkgs.items, pkgs.len);
        free(pkgs.items);
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "run") == 0) {
        const char *target_file = NULL;
        const char *dir = ".";
        int rel = 0;
        const char *edition = NULL;
        DA(CliPkg) pkgs = {0};
        char runargs[1024] = "";
        int forwarding = 0;
        for (int i = 2; i < argc; i++) {
            if (forwarding) {
                size_t len = strlen(runargs);
                snprintf(runargs + len, sizeof(runargs) - len, " %s", argv[i]);
                continue;
            }
            if (strcmp(argv[i], "--") == 0) {
                forwarding = 1;
                continue;
            }
            if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
                dir = argv[++i];
            } else if (strcmp(argv[i], "--release") == 0 || strcmp(argv[i], "--prod") == 0) {
                rel = 1;
            } else if (strcmp(argv[i], "--edition") == 0 && i + 1 < argc) {
                edition = argv[++i];
            } else if (strncmp(argv[i], "--edition=", 10) == 0) {
                edition = argv[i] + 10;
            } else if (strcmp(argv[i], "--pkg") == 0 && i + 1 < argc) {
                CliPkg cp;
                if (parse_cli_pkg(argv[++i], &cp))
                    da_push(&pkgs, cp);
            } else if (strncmp(argv[i], "--pkg=", 6) == 0) {
                CliPkg cp;
                if (parse_cli_pkg(argv[i] + 6, &cp))
                    da_push(&pkgs, cp);
            } else if (!target_file && strstr(argv[i], ".zov")) {
                target_file = argv[i];
            } else {
                size_t len = strlen(runargs);
                snprintf(runargs + len, sizeof(runargs) - len, " %s", argv[i]);
            }
        }
        if (target_file) {
            check_edition(edition);
            char tmpbin[128];
            snprintf(tmpbin, sizeof(tmpbin), "/tmp/zov_run_%d", (int)getpid());
            char *src = read_file(target_file);
            if (!src) {
                fprintf(stderr, "zovc: cannot open '%s'\n", target_file);
                free(pkgs.items);
                return 1;
            }
            Program prog = {0};
            prog.debug = !rel;
            parse_program(src, target_file, &prog);
            Pkg fake_pkg = {0};
            for (size_t pi = 0; pi < pkgs.len; pi++) {
                PkgDep d = {0};
                snprintf(d.name, sizeof(d.name), "%s", pkgs.items[pi].name);
                snprintf(d.path, sizeof(d.path), "%s", pkgs.items[pi].path);
                da_push(&fake_pkg.deps, d);
            }
            modules_load(&prog, target_file, &fake_pkg);
            sema_check(&prog, 0);
            borrow_check(&prog);
            char *c_src = codegen_generate_mode(&prog, 0);
            int rc = driver_compile_ex(c_src, tmpbin, 0, rel, NULL, 0,
                                       NULL, 0, 0);
            free(c_src);
            free(src);
            pkg_free(&fake_pkg);
            free(pkgs.items);
            if (rc != 0) {
                remove(tmpbin);
                return 1;
            }
            char cmd[2048] = "";
            snprintf(cmd, sizeof(cmd), "'%s'%s", tmpbin, runargs);
            int st = system(cmd);
            remove(tmpbin);
            if (WIFEXITED(st))
                return WEXITSTATUS(st);
            return 1;
        }
        int rc = cmd_project(dir, rel, 0, runargs, 0, edition, pkgs.items, pkgs.len);
        free(pkgs.items);
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "check") == 0) {
        const char *dir = ".";
        const char *edition = NULL;
        DA(CliPkg) pkgs = {0};
        const char *target_file = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
                dir = argv[++i];
            } else if (strcmp(argv[i], "--edition") == 0 && i + 1 < argc) {
                edition = argv[++i];
            } else if (strncmp(argv[i], "--edition=", 10) == 0) {
                edition = argv[i] + 10;
            } else if (strcmp(argv[i], "--pkg") == 0 && i + 1 < argc) {
                CliPkg cp;
                if (parse_cli_pkg(argv[++i], &cp))
                    da_push(&pkgs, cp);
            } else if (strncmp(argv[i], "--pkg=", 6) == 0) {
                CliPkg cp;
                if (parse_cli_pkg(argv[i] + 6, &cp))
                    da_push(&pkgs, cp);
            } else if (strstr(argv[i], ".zov")) {
                target_file = argv[i];
            }
        }
        if (target_file) {
            check_edition(edition);
            char *src = read_file(target_file);
            if (!src) {
                fprintf(stderr, "zovc: cannot open '%s'\n", target_file);
                free(pkgs.items);
                return 1;
            }
            Program prog = {0};
            prog.debug = 1;
            parse_program(src, target_file, &prog);
            Pkg fake_pkg = {0};
            for (size_t pi = 0; pi < pkgs.len; pi++) {
                PkgDep d = {0};
                snprintf(d.name, sizeof(d.name), "%s", pkgs.items[pi].name);
                snprintf(d.path, sizeof(d.path), "%s", pkgs.items[pi].path);
                da_push(&fake_pkg.deps, d);
            }
            modules_load(&prog, target_file, &fake_pkg);
            sema_check(&prog, 0);
            borrow_check(&prog);
            free(src);
            pkg_free(&fake_pkg);
            free(pkgs.items);
            return 0;
        } else {
            int rc = cmd_project(dir, 0, 0, NULL, 1, edition, pkgs.items, pkgs.len);
            free(pkgs.items);
            return rc;
        }
    }
    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        if (argc >= 3 && strstr(argv[2], ".zov")) {
            file = argv[2];
            out = ".zov-test-bin";
            test_mode = 1;
        } else {
            const char *dir = ".";
            const char *edition = NULL;
            DA(CliPkg) pkgs = {0};
            for (int i = 2; i < argc; i++) {
                if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
                    dir = argv[++i];
                } else if (strcmp(argv[i], "--edition") == 0 && i + 1 < argc) {
                    edition = argv[++i];
                } else if (strncmp(argv[i], "--edition=", 10) == 0) {
                    edition = argv[i] + 10;
                } else if (strcmp(argv[i], "--pkg") == 0 && i + 1 < argc) {
                    CliPkg cp;
                    if (parse_cli_pkg(argv[++i], &cp))
                        da_push(&pkgs, cp);
                } else if (strncmp(argv[i], "--pkg=", 6) == 0) {
                    CliPkg cp;
                    if (parse_cli_pkg(argv[i] + 6, &cp))
                        da_push(&pkgs, cp);
                }
            }
            int rc = cmd_project(dir, 0, 1, NULL, 0, edition, pkgs.items, pkgs.len);
            free(pkgs.items);
            return rc;
        }
    }

    if (argc >= 2 && (strcmp(argv[1], "--version") == 0
                       || strcmp(argv[1], "-V") == 0)) {
        printf("zovc 0.10.0\n");
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
        } else if (strcmp(argv[i], "--release") == 0
                   || strcmp(argv[i], "--prod") == 0) {
            release = 1;
            force_cc = 1;
        } else if (strcmp(argv[i], "--edition") == 0 && i + 1 < argc) {
            cli_edition = argv[++i];
        } else if (strncmp(argv[i], "--edition=", 10) == 0) {
            cli_edition = argv[i] + 10;
        } else if (strcmp(argv[i], "--pkg") == 0 && i + 1 < argc) {
            CliPkg cp;
            if (parse_cli_pkg(argv[++i], &cp))
                da_push(&cli_pkgs, cp);
        } else if (strncmp(argv[i], "--pkg=", 6) == 0) {
            CliPkg cp;
            if (parse_cli_pkg(argv[i] + 6, &cp))
                da_push(&cli_pkgs, cp);
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                free(cli_pkgs.items);
                return 1;
            }
            out = argv[i];
        } else if (strcmp(argv[i], "-h") == 0
                   || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            free(cli_pkgs.items);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "zovc: unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            free(cli_pkgs.items);
            return 1;
        } else if (!file) {
            file = argv[i];
        } else if (strcmp(argv[i], file) != 0) {
            fprintf(stderr, "zovc: multiple input files are unsupported\n");
            free(cli_pkgs.items);
            return 1;
        }
    }
    if (!file) {
        usage(argv[0]);
        free(cli_pkgs.items);
        return 1;
    }

    check_edition(cli_edition);

    char *src = read_file(file);
    Program prog = {0};
    prog.freestanding = freestanding;
    prog.debug = !release;
    parse_program(src, file, &prog);
    Pkg fake_pkg = {0};
    for (size_t pi = 0; pi < cli_pkgs.len; pi++) {
        PkgDep d = {0};
        snprintf(d.name, sizeof(d.name), "%s", cli_pkgs.items[pi].name);
        snprintf(d.path, sizeof(d.path), "%s", cli_pkgs.items[pi].path);
        da_push(&fake_pkg.deps, d);
    }
    modules_load(&prog, file, &fake_pkg);
    sema_check(&prog, test_mode);
    borrow_check(&prog);
    if (check_only) {
        free(src);
        pkg_free(&fake_pkg);
        free(cli_pkgs.items);
        return 0;
    }
    char *c_src = codegen_generate_mode(&prog, test_mode);
    int rc = driver_compile_ex(c_src, out, emit_c, force_cc, NULL, 0,
                               NULL, 0, freestanding);
    free(c_src);
    free(src);
    pkg_free(&fake_pkg);
    free(cli_pkgs.items);
    if (rc == 0 && test_mode && !emit_c) {
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
