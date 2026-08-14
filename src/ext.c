#include "web.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define EXT_MAX 8

static char g_ext[EXT_MAX][PATH_MAX];
static char g_err[EXT_MAX][256];
static int  g_n;

static bool has_manifest(const char *dir) {
    char p[PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/manifest.json", dir);
    return access(p, R_OK) == 0;
}

bool ext_add(const char *path) {
    if (g_n >= EXT_MAX) {
        fprintf(stderr, "web: no room for more than %d extensions\n", EXT_MAX);
        return false;
    }
    char real[PATH_MAX];
    if (!realpath(path, real)) {
        fprintf(stderr, "web: %s: %s\n", path, strerror(errno));
        return false;
    }
    if (!has_manifest(real)) {
        fprintf(stderr, "web: %s holds no manifest.json\n", real);
        return false;
    }
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_ext[i], real)) return true;
    snprintf(g_ext[g_n++], PATH_MAX, "%s", real);
    return true;
}

void ext_dir(char *out, size_t cap) {
    char dir[PATH_MAX];
    config_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/extensions", dir);
}

void ext_scan(void) {
    char root[PATH_MAX];
    ext_dir(root, sizeof root);
    DIR *d = opendir(root);
    if (!d) return;
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char sub[PATH_MAX];
        if (snprintf(sub, sizeof sub, "%s/%s", root, e->d_name) >= (int)sizeof sub)
            continue;
        if (has_manifest(sub)) ext_add(sub);
    }
    closedir(d);
}

int ext_count(void) { return g_n; }

static void ext_name(const char *dir, char *out, size_t cap) {
    const char *slash = strrchr(dir, '/');
    snprintf(out, cap, "%s", slash ? slash + 1 : dir);
    char p[PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/manifest.json", dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    size_t len;
    const char *v = json_str(buf, "name", &len);
    // a store build names itself __MSG_x__ and answers it out of _locales
    if (v && len && len < cap && (len < 6 || memcmp(v, "__MSG_", 6) != 0))
        snprintf(out, cap, "%.*s", (int)len, v);
}

// loadUnpacked for each of the n extensions named in idx, ok[] filled per
// position. Returns how many loaded, or -1 if the browser never answered.
static int ext_try(Chrome *c, const int *idx, int n, bool *ok) {
    WS ws;
    if (chrome_browser_ws(c, &ws) < 0) return -1;
    for (int i = 0; i < n; i++) {
        Buf b = {0};
        buf_addf(&b, "{\"id\":%d,\"method\":\"Extensions.loadUnpacked\","
                     "\"params\":{\"path\":\"", i + 1);
        json_escape_buf(&b, g_ext[idx[i]]);
        buf_add(&b, "\"}}", 3);
        ws_send_text(&ws, b.p, b.len);
        buf_free(&b);
        ok[i] = false;
    }

    int seen = 0, loaded = 0;
    double until = now_sec() + 30.0;
    while (seen < n && now_sec() < until && !ws.closed) {
        if (ws_fill(&ws) < 0) break;
        char *msg;
        size_t len;
        while (ws_next(&ws, &msg, &len) == 1) {
            char reply[2048];
            size_t cap = len < sizeof reply - 1 ? len : sizeof reply - 1;
            snprintf(reply, sizeof reply, "%.*s", (int)cap, msg);
            int id = (int)json_num(reply, "id", 0);
            if (id < 1 || id > n) continue;
            seen++;
            size_t wlen;
            const char *why = json_str(reply, "message", &wlen);
            if (why) {
                snprintf(g_err[idx[id - 1]], sizeof g_err[0], "%.*s",
                         (int)wlen, why);
            } else {
                ok[id - 1] = true;
                loaded++;
            }
        }
        struct timespec ts = {0, 20 * 1000000};
        nanosleep(&ts, NULL);
    }
    ws_close(&ws);
    return loaded;
}

int ext_load(Chrome *c) {
    if (!g_n) return 0;

    int idx[EXT_MAX];
    bool ok[EXT_MAX];
    for (int i = 0; i < g_n; i++) idx[i] = i;
    int loaded = ext_try(c, idx, g_n, ok);
    if (loaded < 0) {
        fprintf(stderr, "web: no browser socket; extensions were not loaded\n");
        return 0;
    }

    // chrome indexes an unpacked extension's rulesets into _metadata inside
    // the directory and will not load a directory holding a stale one. It
    // rebuilds the index, which is slow, so only clear it once a load has
    // actually failed rather than on every start.
    int retry[EXT_MAX], nretry = 0;
    for (int i = 0; i < g_n; i++) {
        if (ok[i]) continue;
        char meta[PATH_MAX + 16];
        snprintf(meta, sizeof meta, "%s/_metadata", g_ext[i]);
        if (access(meta, F_OK) != 0) continue;
        rm_tree(meta);
        retry[nretry++] = i;
    }
    if (nretry) {
        bool again[EXT_MAX];
        term_log("reindexing %d extension%s", nretry, nretry == 1 ? "" : "s");
        int more = ext_try(c, retry, nretry, again);
        for (int i = 0; i < nretry; i++)
            if (again[i]) ok[retry[i]] = true;
        if (more > 0) loaded += more;
    }

    for (int i = 0; i < g_n; i++) {
        if (!ok[i]) {
            fprintf(stderr, "web: extension: %s\n",
                    g_err[i][0] ? g_err[i] : "did not load");
            continue;
        }
        char name[128];
        ext_name(g_ext[i], name, sizeof name);
        term_log("extension %s (%s)", name, g_ext[i]);
    }
    return loaded;
}
