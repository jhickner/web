#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "web.h"

// ------------------------------------------------------------------ output

static void out_line(App *a, const char *payload) {
    Script *s = &a->script;
    Buf b = {0};
    if (s->json) {
        buf_add(&b, "{\"js\":\"", 7);
        json_escape_buf(&b, s->line);
        buf_add(&b, "\",\"value\":\"", 11);
        json_escape_buf(&b, payload);
        buf_add(&b, "\"}\n", 3);
    } else {
        buf_add(&b, payload, strlen(payload));
        buf_add(&b, "\n", 1);
    }
    if (a->shot_stdout)                            writeall(STDERR_FILENO, b.p, b.len);
    else if (!a->stdout_tty || s->drain_exit)      writeall(STDOUT_FILENO, b.p, b.len);
    if (a->console_open) console_log(a, payload);
    else if (a->stdout_tty && !s->drain_exit) notify(a, payload);
    buf_free(&b);
}

static void fail(App *a, const char *why) {
    Script *s = &a->script;
    char msg[4608];
    snprintf(msg, sizeof msg, "error: %s", why);
    if (a->console_open) console_log(a, msg);
    else fprintf(stderr, "web: line %d: %s: %s\n", s->lineno, s->line, why);
    s->failures++;
    s->state = SC_IDLE;
    s->next_at = now_sec() + s->delay;
    if (s->from_file) {
        s->queue.len = 0;
        if (s->queue.p) s->queue.p[0] = 0;
    }
}

// ------------------------------------------------------------------ queue

void script_init(App *a) {
    Script *s = &a->script;
    s->timeout = 5.0;
    s->state = SC_IDLE;
}

void script_free(App *a) {
    buf_free(&a->script.queue);
}

bool script_busy(const App *a) {
    return a->script.state != SC_IDLE || a->script.queue.len > 0;
}

void script_push(App *a, const char *line) {
    Script *s = &a->script;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#') return;
    buf_add(&s->queue, line, strlen(line));
    buf_add(&s->queue, "\n", 1);
}

int script_load(App *a, const char *path) {
    int fd = STDIN_FILENO;
    if (path && strcmp(path, "-")) {
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "web: %s: %s\n", path, strerror(errno));
            return -1;
        }
    }
    Buf all = {0};
    char tmp[4096];
    ssize_t n;
    while ((n = read(fd, tmp, sizeof tmp)) > 0) buf_add(&all, tmp, (size_t)n);
    if (fd != STDIN_FILENO) close(fd);

    for (char *p = all.p; p && *p; ) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        script_push(a, p);
        if (!nl) break;
        p = nl + 1;
    }
    buf_free(&all);
    a->script.from_file = true;
    a->script.drain_exit = true;
    return 0;
}

// ------------------------------------------------------------------- run

static void done(App *a) {
    Script *s = &a->script;
    s->state = SC_IDLE;
    s->next_at = now_sec() + s->delay;
    if (s->step) s->stepping = true;
}

static void run(App *a) {
    Script *s = &a->script;
    Buf e = {0};
    buf_addf(&e, "Promise.resolve(eval(\"");
    json_escape_buf(&e, s->line);
    buf_addf(&e, "\")).then(String)");

    Buf esc = {0};
    json_escape_buf(&esc, e.p);
    app_req_note(a, app_cdp(a, "Runtime.evaluate",
        "\"expression\":\"%s\",\"returnByValue\":true,\"awaitPromise\":true",
        esc.p), RQ_SCRIPT);
    buf_free(&esc);
    buf_free(&e);

    s->deadline = now_sec() + s->timeout;
    s->state = SC_WAIT;
}

// ------------------------------------------------------------------ replies

void script_reply(App *a, const char *msg) {
    Script *s = &a->script;
    if (s->state != SC_WAIT) return;

    size_t n = 0;
    const char *v = json_eval_str(msg, &n);
    if (!v) {
        // description holds the message then the stack; first line is the message
        size_t dn = 0;
        const char *d = json_str(msg, "description", &dn);
        char why[400] = "the page threw";
        if (d && dn) {
            char raw[400];
            json_unescape(raw, sizeof raw, d, dn);
            char *nl = strchr(raw, '\n');
            if (nl) *nl = 0;
            snprintf(why, sizeof why, "%s", raw);
        }
        fail(a, why);
        return;
    }

    Buf out = {0};
    buf_reserve(&out, n + 4);
    json_unescape(out.p, n + 4, v, n);
    out_line(a, out.p);
    buf_free(&out);

    if (a->loading) {
        s->nav_seq = a->load_seq;
        s->deadline = now_sec() + s->timeout;
        s->state = SC_NAV;
        return;
    }
    done(a);
}

// ------------------------------------------------------------------ loop

void script_step(App *a) {
    Script *s = &a->script;
    double t = now_sec();

    switch (s->state) {
    case SC_IDLE:
        if (!s->queue.len || s->stepping || t < s->next_at) return;
        {
            char *nl = strchr(s->queue.p, '\n');
            size_t len = nl ? (size_t)(nl - s->queue.p) : s->queue.len;
            if (len >= sizeof s->line) len = sizeof s->line - 1;
            memcpy(s->line, s->queue.p, len);
            s->line[len] = 0;
            buf_consume(&s->queue, nl ? len + 1 : len);
        }
        s->lineno++;
        run(a);
        return;

    case SC_WAIT:
        if (t > s->deadline) fail(a, "timed out");
        return;

    case SC_NAV:
        if (a->load_seq != s->nav_seq || t > s->deadline) done(a);
        return;
    }
}

int script_wait_ms(const App *a) {
    const Script *s = &a->script;
    if (s->state == SC_IDLE && !s->queue.len) return -1;
    if (s->stepping) return -1;

    double t = (s->state == SC_IDLE) ? s->next_at : s->deadline;
    double ms = (t - now_sec()) * 1000.0;
    if (ms < 5) ms = 5;
    if (ms > 100) ms = 100;
    return (int)ms;
}
