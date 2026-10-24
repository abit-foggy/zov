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

#include "modules.h"
#include "parser.h"
#include "pkg.h"

#include <unistd.h>

typedef DA(char *) PathList;

typedef struct Loader {
    Program *prog;
    struct Pkg *pkg; /* [dependencies] mapping: use <dep>; -> dep src */
    DA(struct Decl *) staged; /* imported decls; prepended before sema */
    PathList visited; /* canonical paths already merged (diamond dedup) */
    PathList loading; /* in-flight imports: cycle detection */
} Loader;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "zovc: error: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf)
        die("out of memory");
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* std root candidates: $ZOV_STD_PATH, ./lib/std, <exe dir>/lib/std */
static int std_root(char *out, size_t cap)
{
    const char *env = getenv("ZOV_STD_PATH");
    if (env) {
        snprintf(out, cap, "%s", env);
        return 1;
    }
    snprintf(out, cap, "lib/std");
    if (access(out, F_OK) == 0)
        return 1;
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            int w = snprintf(out, cap, "%s/../lib/std", exe);
            if (w > 0 && (size_t)w < cap)
                return 1;
            return 0;
        }
    }
    return 0;
}

static char *dirname_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return strdup(".");
    size_t n = (size_t)(slash - path);
    char *s = malloc(n + 1);
    if (!s)
        die("out of memory");
    memcpy(s, path, n);
    s[n] = '\0';
    return s;
}

/* "std::vec" -> <stdroot>/vec.zov ; "a::b" -> <dir>/a/b.zov */
static char *resolve_module(const char *path, const char *dir)
{
    char body[256];
    size_t n = 0;
    int is_std = strncmp(path, "std::", 5) == 0;
    const char *p = is_std ? path + 5 : path;
    for (const char *q = p; *q && n + 1 < sizeof(body); q++)
        body[n++] = (*q == ':') ? '/' : *q;
    body[n] = '\0';
    if (is_std) {
        char root[1024];
        if (!std_root(root, sizeof(root)))
            die("cannot locate the std library root");
        size_t len = strlen(root) + n + 16;
        char *s = malloc(len);
        snprintf(s, len, "%s/%s.zov", root, body);
        return s;
    }
    /* each "::" segment is a directory: a::b::c -> a/b/c.zov */
    const char *seg = p;
    int depth = 0;
    for (const char *q = p; *q; q++)
        if (*q == ':' && q[1] == ':')
            depth++;
    if (depth > 0) {
        char rel[256];
        size_t rn = 0;
        for (const char *q = p; *q; q++) {
            if (q[0] == ':' && q[1] == ':') {
                rel[rn++] = '/';
                q++;
                continue;
            }
            rel[rn++] = *q;
        }
        rel[rn] = '\0';
        size_t len = strlen(dir) + rn + 16;
        char *s = malloc(len);
        snprintf(s, len, "%s/%s.zov", dir, rel);
        (void)seg;
        return s;
    }
    /* single-segment module: <dir>/<name>.zov */
    {
        size_t len = strlen(dir) + strlen(body) + 16;
        char *s = malloc(len);
        if (!s)
            die("out of memory");
        snprintf(s, len, "%s/%s.zov", dir, body[0] ? body : path);
        return s;
    }
}

static int list_has(PathList *l, const char *s)
{
    for (size_t i = 0; i < l->len; i++)
        if (strcmp(l->items[i], s) == 0)
            return 1;
    return 0;
}

/* depth-first merge: deps first, then the module's exported decls */
/* dependency mapping: use <dep>; -> <dep>/src/lib.zov and
 * use <dep>::sub; -> <dep>/src/sub.zov */
static char *resolve_via_pkg(Loader *ld, const char *path,
                             const char *srcdir)
{
    if (!ld->pkg)
        return NULL;
    char first[128];
    size_t n = 0;
    const char *sep = strstr(path, "::");
    const char *rest = NULL;
    if (sep) {
        rest = sep + 2;
        n = (size_t)(sep - path);
    } else {
        n = strlen(path);
    }
    if (n == 0 || n >= sizeof(first))
        return NULL;
    memcpy(first, path, n);
    first[n] = '\0';
    PkgDep *dep = pkg_find_dep(ld->pkg, first);
    if (!dep || !dep->path[0])
        return NULL;
    char dep_dir[1024];
    snprintf(dep_dir, sizeof(dep_dir), "%s/%s", ld->pkg->dir, dep->path);
    char out[1536];
    if (rest)
        snprintf(out, sizeof(out), "%s/src/%s.zov", dep_dir, rest);
    else
        snprintf(out, sizeof(out), "%s/src/lib.zov", dep_dir);
    if (access(out, F_OK) != 0)
        snprintf(out, sizeof(out), "%s/%s.zov", dep_dir, first);
    (void)srcdir;
    return strdup(out);
}

static void load_module(Loader *ld, const char *path, const char *srcdir,
                        StrList *syms_out)
{
    char *file = resolve_via_pkg(ld, path, srcdir);
    if (!file)
        file = resolve_module(path, srcdir);
    if (list_has(&ld->loading, file))
        die("circular module import: '%s' is already being loaded", path);
    if (list_has(&ld->visited, file)) {
        /* diamond: already merged; re-expose its exported names */
        FILE *probe = fopen(file, "r");
        if (!probe)
            die("cannot open module '%s' (%s)", path, file);
        fclose(probe);
        /* symbol re-scan happens in the caller via decl names */
        free(file);
        return;
    }
    char *src = read_file(file);
    if (!src)
        die("cannot open module '%s' (%s)", path, file);
    da_push(&ld->loading, file);
    Program sub = {0};
    parse_program(src, file, &sub);
    /* recurse into the module's own imports relative to its directory */
    char *subdir = dirname_of(file);
    for (size_t i = 0; i < sub.decls.len; i++) {
        Decl *d = sub.decls.items[i];
        if (d->kind == DL_USE && !d->u.use.is_c_header)
            load_module(ld, d->u.use.path, subdir, &d->u.use.zsyms);
    }
    for (size_t i = 0; i < sub.decls.len; i++) {
        Decl *d = sub.decls.items[i];
        if (d->kind == DL_USE && !d->u.use.is_c_header
            && d->u.use.zsyms.len == 0)
            continue; /* module deps were resolved by recursion */
        if (d->kind != DL_USE && !d->exported)
            continue;
        /* staged ahead of importers: registration order is topological */
        da_push(&ld->staged, d);
        const char *nm = NULL;
        if (d->kind == DL_FN && !d->u.fn.recv)
            nm = d->u.fn.name;
        if (d->kind == DL_TYPE)
            nm = d->u.type.name;
        if (nm) {
            for (size_t k = 0; k < syms_out->len; k++)
                if (syms_out->items[k] == nm)
                    nm = NULL;
            if (nm)
                da_push(syms_out, nm);
        }
    }
    da_push(&ld->visited, ld->loading.items[ld->loading.len - 1]);
    ld->loading.len--;
    free(src);
    free(subdir);
}

void modules_load(Program *prog, const char *main_file, struct Pkg *pkg)
{
    Loader *ld = zalloc(sizeof(Loader));
    ld->prog = prog;
    ld->pkg = pkg;
    char *srcdir = dirname_of(main_file);
    size_t n = prog->decls.len;
    for (size_t i = 0; i < n; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind != DL_USE || d->u.use.is_c_header)
            continue;
        load_module(ld, d->u.use.path, srcdir, &d->u.use.zsyms);
    }
    /* diamond re-imports: re-expose symbols of already-merged modules */
    for (size_t i = 0; i < n; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind != DL_USE || d->u.use.is_c_header
            || d->u.use.zsyms.len > 0)
            continue;
        char *file = resolve_via_pkg(ld, d->u.use.path, srcdir);
        if (!file)
            file = resolve_module(d->u.use.path, srcdir);
        char *src = read_file(file);
        if (!src)
            die("cannot open module '%s'", d->u.use.path);
        Program sub = {0};
        parse_program(src, file, &sub);
        for (size_t j = 0; j < sub.decls.len; j++) {
            Decl *sd = sub.decls.items[j];
            if (sd->kind == DL_USE || !sd->exported)
                continue;
            const char *nm = NULL;
            if (sd->kind == DL_FN && !sd->u.fn.recv)
                nm = sd->u.fn.name;
            if (sd->kind == DL_TYPE)
                nm = sd->u.type.name;
            if (nm)
                da_push(&d->u.use.zsyms, nm);
        }
        free(src);
        free(file);
    }
    free(srcdir);
    /* imports precede importers so pass-1 type resolution is topological */
    if (ld->staged.len > 0) {
        size_t total = ld->staged.len + prog->decls.len;
        Decl **merged = malloc(total * sizeof(Decl *));
        if (!merged)
            die("out of memory");
        size_t k = 0;
        for (size_t i = 0; i < ld->staged.len; i++)
            merged[k++] = ld->staged.items[i];
        for (size_t i = 0; i < prog->decls.len; i++)
            merged[k++] = prog->decls.items[i];
        prog->decls.items = merged;
        prog->decls.len = total;
        prog->decls.cap = total;
    }
}
