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

#ifndef ZOV_PKG_H
#define ZOV_PKG_H

#include "common.h"

typedef struct PkgDep {
    char name[128];
    char path[512];
    char git[512];
    char rev[64];
    char url[512];
    char sha256[128];
} PkgDep;

typedef struct Pkg {
    char name[128];
    char version[64];
    char edition[64];
    char min_zovc[64];
    char entry[256];
    int freestanding;  /* [package] freestanding = true: bare-metal build */
    DA(PkgDep) deps;
    DA(char *) links;        /* [c-libraries] links, malloc'd strings */
    DA(char *) include_dirs;  /* malloc'd strings */
    char dir[512]; /* directory containing zov.toml */
} Pkg;

/* Returns 0 on success; manifest dir is recorded for path resolution. */
int pkg_load(Pkg *p, const char *toml_path);
PkgDep *pkg_find_dep(Pkg *p, const char *name);
/* Recomputes checksums and rewrites zov.lock; 0 on success. */
int pkg_write_lock(Pkg *p);
void pkg_free(Pkg *p);

#endif /* ZOV_PKG_H */
