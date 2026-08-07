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

int ext_load(Chrome *c) {
    if (!g_n) return 0;
    WS ws;
    if (chrome_browser_ws(c, &ws) < 0) {
        fprintf(stderr, "web: no browser socket; extensions were not loaded\n");
        return 0;
    }
    for (int i = 0; i < g_n; i++) {
        Buf b = {0};
        buf_addf(&b, "{\"id\":%d,\"method\":\"Extensions.loadUnpacked\","
                     "\"params\":{\"path\":\"", i + 1);
        json_escape_buf(&b, g_ext[i]);
        buf_add(&b, "\"}}", 3);
        ws_send_text(&ws, b.p, b.len);
        buf_free(&b);
    }

    int seen = 0, ok = 0;
    double until = now_sec() + 15.0;
    while (seen < g_n && now_sec() < until && !ws.closed) {
        if (ws_fill(&ws) < 0) break;
        char *msg;
        size_t n;
        while (ws_next(&ws, &msg, &n) == 1) {
            char reply[2048];
            size_t cap = n < sizeof reply - 1 ? n : sizeof reply - 1;
            snprintf(reply, sizeof reply, "%.*s", (int)cap, msg);
            if (!strstr(reply, "\"id\":")) continue;
            seen++;
            size_t len;
            const char *why = json_str(reply, "message", &len);
            if (why) fprintf(stderr, "web: extension: %.*s\n", (int)len, why);
            else     ok++;
        }
        struct timespec ts = {0, 20 * 1000000};
        nanosleep(&ts, NULL);
    }
    ws_close(&ws);

    for (int i = 0; ok && i < g_n; i++) {
        char name[128];
        ext_name(g_ext[i], name, sizeof name);
        term_log("extension %s (%s)", name, g_ext[i]);
    }
    return ok;
}
