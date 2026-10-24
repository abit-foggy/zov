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

#include "pkg.h"

#include <dirent.h>
#include <sys/stat.h>

typedef enum PkgSection {
    SEC_NONE = 0,
    SEC_PACKAGE,
    SEC_DEPS,
    SEC_CLIBS,
    SEC_LOCK,
} PkgSection;

static void copy_clamped(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void pkg_die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "zovc: manifest error: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static char *dup_str(const char *s)
{
    char *d = malloc(strlen(s) + 1);
    if (!d)
        pkg_die("out of memory");
    strcpy(d, s);
    return d;
}

static void trim(char **ps, char *end)
{
    char *s = *ps;
    while (s < end && (*s == ' ' || *s == '\t'))
        s++;
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    *end = '\0';
    *ps = s;
}

static int parse_quoted(char **pv, char *end, char *out, size_t outcap)
{
    char *s = *pv;
    trim(&s, end);
    if (s == end || *s != '"')
        return 0;
    s++;
    size_t n = 0;
    while (s < end && *s != '"') {
        if (n + 1 >= outcap)
            return 0;
        out[n++] = *s++;
    }
    if (s == end)
        return 0;
    out[n] = '\0';
    *pv = s + 1;
    return 1;
}

typedef DA(char *) StrPtrList;

/* ["a", "b"] -> malloc'd string list */
static int parse_str_array(char **pv, char *end, StrPtrList *out)
{
    char *s = *pv;
    trim(&s, end);
    if (s == end || *s != '[')
        return 0;
    s++;
    for (;;) {
        char tmp[512];
        while (s < end && (*s == ' ' || *s == '\t' || *s == ','))
            s++;
        if (s < end && *s == ']')
            return 1;
        if (!parse_quoted(&s, end, tmp, sizeof(tmp)))
            return 0;
        da_push(out, dup_str(tmp));
    }
}

/* { path = "..", git = "..." } -> dep fields */
static void parse_dep_table(char *s, char *end, PkgDep *dep)
{
    while (s < end) {
        while (s < end && (*s == ' ' || *s == '\t' || *s == ','))
            s++;
        if (s >= end || *s == '}')
            return;
        char *keq = memchr(s, '=', (size_t)(end - s));
        if (!keq)
            return;
        char *ks = s;
        char *ke = keq;
        trim(&ks, ke);
        *ke = '\0';
        char tmp[512];
        char *v = keq + 1;
        if (!parse_quoted(&v, end, tmp, sizeof(tmp)))
            return;
        if (strcmp(ks, "path") == 0)
            copy_clamped(dep->path, sizeof(dep->path), tmp);
        if (strcmp(ks, "git") == 0)
            copy_clamped(dep->git, sizeof(dep->git), tmp);
        if (strcmp(ks, "rev") == 0)
            copy_clamped(dep->rev, sizeof(dep->rev), tmp);
        s = v;
    }
}

static void section_from_name(const char *name, PkgSection *sec)
{
    if (strcmp(name, "package") == 0)
        *sec = SEC_PACKAGE;
    else if (strcmp(name, "dependencies") == 0)
        *sec = SEC_DEPS;
    else if (strcmp(name, "c-libraries") == 0)
        *sec = SEC_CLIBS;
    else if (strcmp(name, "lock") == 0)
        *sec = SEC_LOCK;
    else
        *sec = SEC_NONE;
}

int pkg_load(Pkg *p, const char *toml_path)
{
    memset(p, 0, sizeof(*p));
    FILE *f = fopen(toml_path, "rb");
    if (!f)
        return 1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 1;
    }
    long n = ftell(f);
    rewind(f);
    char *text = malloc((size_t)n + 2);
    if (!text || fread(text, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(text);
        return 1;
    }
    text[n] = '\n';
    text[n + 1] = '\0';
    fclose(f);

    const char *slash = strrchr(toml_path, '/');
    if (slash)
        snprintf(p->dir, sizeof(p->dir), "%.*s", (int)(slash - toml_path),
                 toml_path);
    else
        snprintf(p->dir, sizeof(p->dir), ".");

    PkgSection sec = SEC_NONE;
    PkgDep cur_dep;
    int have_cur_dep = 0;
    char *line = text;
    while (*line) {
        char *nl = strchr(line, '\n');
        char *end = nl;
        char *hash = memchr(line, '#', (size_t)(end - line));
        if (hash && hash != line)
            end = hash; /* trailing comments only */
        char *s = line;
        line = nl ? nl + 1 : line + strlen(line);
        trim(&s, end);
        if (!*s)
            continue;
        if (*s == '[') {
            if (have_cur_dep) {
                da_push(&p->deps, cur_dep);
                have_cur_dep = 0;
            }
            char *cb = strchr(s, ']');
            if (!cb)
                pkg_die("unterminated section in manifest");
            *cb = '\0';
            char name[128];
            snprintf(name, sizeof(name), "%s", s + 1);
            char *ns = name;
            size_t nl2 = strlen(ns);
            while (nl2 && (ns[nl2 - 1] == ' ' || ns[nl2 - 1] == '\t'))
                ns[--nl2] = '\0';
            section_from_name(ns, &sec);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq)
            pkg_die("malformed manifest line: '%s'", s);
        *eq = '\0';
        char *ks = s;
        char *ke = eq;
        trim(&ks, ke);
        char *v = eq + 1;
        if (sec == SEC_PACKAGE) {
            char tmp[512];
            if (!parse_quoted(&v, end, tmp, sizeof(tmp)))
                pkg_die("package.%s must be a string", ks);
            if (strcmp(ks, "name") == 0)
                copy_clamped(p->name, sizeof(p->name), tmp);
            if (strcmp(ks, "version") == 0)
                copy_clamped(p->version, sizeof(p->version), tmp);
            if (strcmp(ks, "entry") == 0)
                copy_clamped(p->entry, sizeof(p->entry), tmp);
        } else if (sec == SEC_DEPS) {
            if (have_cur_dep) {
                da_push(&p->deps, cur_dep);
                have_cur_dep = 0;
            }
            memset(&cur_dep, 0, sizeof(cur_dep));
            copy_clamped(cur_dep.name, sizeof(cur_dep.name), ks);
            char *v0 = v;
            trim(&v0, end);
            v = v0;
            if (*v == '{') {
                parse_dep_table(v + 1, end, &cur_dep);
            } else {
                char tmp[512];
                if (parse_quoted(&v, end, tmp, sizeof(tmp)))
                    copy_clamped(cur_dep.path, sizeof(cur_dep.path), tmp);
            }
            have_cur_dep = 1;
        } else if (sec == SEC_CLIBS) {
            if (strcmp(ks, "links") == 0) {
                if (!parse_str_array(&v, end, (StrPtrList *)&p->links))
                    pkg_die("links must be a string array");
            } else if (strcmp(ks, "include_dirs") == 0) {
                if (!parse_str_array(&v, end, (StrPtrList *)&p->include_dirs))
                    pkg_die("include_dirs must be a string array");
            }
        }
        /* SEC_LOCK entries are regenerated, never consumed */
    }
    if (have_cur_dep)
        da_push(&p->deps, cur_dep);
    free(text);
    if (!p->name[0])
        snprintf(p->name, sizeof(p->name), "app");
    if (!p->entry[0])
        snprintf(p->entry, sizeof(p->entry), "src/main.zov");
    return 0;
}

PkgDep *pkg_find_dep(Pkg *p, const char *name)
{
    for (size_t i = 0; i < p->deps.len; i++)
        if (strcmp(p->deps.items[i].name, name) == 0)
            return &p->deps.items[i];
    return NULL;
}

/* ---- deterministic content hashing ---- */

static uint64_t fnv1a64(const char *data, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t hash_file(const char *path, uint64_t h)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return h;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        h = fnv1a64(buf, n, h);
    fclose(f);
    return h;
}

/* collect *.zov under root deterministically (sorted), depth-capped */
static void collect_zov(const char *root, int depth, StrPtrList *out)
{
    if (depth > 6)
        return;
    DIR *d = opendir(root);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        size_t n = strlen(root) + strlen(e->d_name) + 2;
        char *full = malloc(n);
        if (!full)
            pkg_die("out of memory");
        snprintf(full, n, "%s/%s", root, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            collect_zov(full, depth + 1, out);
            free(full);
            continue;
        }
        size_t ln = strlen(e->d_name);
        if (ln > 5 && strcmp(e->d_name + ln - 4, ".zov") == 0)
            da_push(out, full);
        else
            free(full);
    }
    closedir(d);
}

static int str_ptr_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static uint64_t dep_checksum(PkgDep *dep, const char *dep_dir)
{
    uint64_t h = 1469598103934665603ULL;
    char manifest[1024];
    copy_clamped(manifest, sizeof(manifest), dep_dir);
    copy_clamped(manifest + strlen(manifest),
                 sizeof(manifest) - strlen(manifest), "/zov.toml");
    h = hash_file(manifest, h);
    StrPtrList files = {0};
    collect_zov(dep_dir, 0, &files);
    qsort(files.items, files.len, sizeof(char *), str_ptr_cmp);
    for (size_t i = 0; i < files.len; i++)
        h = hash_file(files.items[i], h);
    for (size_t i = 0; i < files.len; i++)
        free(files.items[i]);
    free(files.items);
    /* the declared source bounds the hash domain */
    h = fnv1a64(dep->git, strlen(dep->git), h);
    h = fnv1a64(dep->rev, strlen(dep->rev), h);
    return h;
}

int pkg_write_lock(Pkg *p)
{
    /* checksums are name-stable: sorted paths under each dep root */
    char lockpath[1024];
    snprintf(lockpath, sizeof(lockpath), "%s/zov.lock", p->dir);
    FILE *f = fopen(lockpath, "wb");
    if (!f)
        pkg_die("cannot write %s", lockpath);
    fprintf(f, "# zov.lock: generated by zovc, do not edit\n");
    for (size_t i = 0; i < p->deps.len; i++) {
        PkgDep *dep = &p->deps.items[i];
        char dep_dir[1024];
        if (dep->path[0])
            snprintf(dep_dir, sizeof(dep_dir), "%s/%s", p->dir, dep->path);
        else
            snprintf(dep_dir, sizeof(dep_dir), ".zov-cache/deps/%s",
                     dep->name);
        Pkg depkg;
        char dep_toml[1024];
        copy_clamped(dep_toml, sizeof(dep_toml), dep_dir);
        copy_clamped(dep_toml + strlen(dep_toml),
                     sizeof(dep_toml) - strlen(dep_toml), "/zov.toml");
        char ver[64] = "0.0.0";
        if (pkg_load(&depkg, dep_toml) == 0) {
            snprintf(ver, sizeof(ver), "%s", depkg.version);
            pkg_free(&depkg);
        }
        uint64_t sum = dep_checksum(dep, dep_dir);
        fprintf(f, "\n[lock]\nname = \"%s\"\nversion = \"%s\"\n", dep->name,
                ver);
        if (dep->path[0])
            fprintf(f, "source = \"path:%s\"\n", dep->path);
        else
            fprintf(f, "source = \"git:%s@%s\"\n", dep->git, dep->rev);
        fprintf(f, "checksum = \"%016llx\"\n",
                (unsigned long long)sum);
    }
    fclose(f);
    return 0;
}

void pkg_free(Pkg *p)
{
    for (size_t i = 0; i < p->links.len; i++)
        free(p->links.items[i]);
    for (size_t i = 0; i < p->include_dirs.len; i++)
        free(p->include_dirs.items[i]);
    free(p->links.items);
    free(p->include_dirs.items);
    free(p->deps.items);
    memset(p, 0, sizeof(*p));
}
