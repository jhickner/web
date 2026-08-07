#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

// label alphabet
#define HINT_CHARS "asdfghjkl"

// seconds to wait for the page to say the labels are up
#define HINT_WAIT 2.0

// ------------------------------------------------------------------ the page

// hint script for the isolated world; no backslash or double quote anywhere in it
static const char HINT_JS[] =
    "(function(){"
    "if(window.__webhints)return;window.__webhints=1;"

    // everything a click could be aimed at
    "var SEL='a[href],button,input,select,textarea,summary,details,label[for],"
    "[role=button],[role=link],[role=checkbox],[role=radio],[role=tab],"
    "[role=menuitem],[role=option],[onclick],[contenteditable],[tabindex]';"
    "var box=null,labels=[],typed='',kind=0;"
    // per-site rules: the set to look in, and the part of it to leave alone
    "var sel=SEL,skipsel='';"

    "function say(s){try{__webhint(s);}catch(e){}}"
    "function set(e,k,v){e.style.setProperty(k,v,'important');}"

    // candidates in this document, open shadow roots and same-origin frames, with frame offsets
    "function collect(root,dx,dy,out){"
    "var all=root.querySelectorAll(sel),i;"
    "for(i=0;i<all.length;i++)out.push([all[i],dx,dy]);"
    "var hosts=root.querySelectorAll('*');"
    "for(i=0;i<hosts.length;i++)if(hosts[i].shadowRoot)collect(hosts[i].shadowRoot,dx,dy,out);"
    "var fr=root.querySelectorAll('iframe');"
    "for(i=0;i<fr.length;i++){try{"
    "var d=fr[i].contentDocument;if(!d)continue;"
    "var r=fr[i].getBoundingClientRect();collect(d,dx+r.left,dy+r.top,out);"
    "}catch(e){}}}"

    // in view, big enough to aim at, and on top at the aim point
    "function pick(el,dx,dy){"
    "if(el.disabled)return null;"
    // a hint-skip match takes everything under it with it
    "if(skipsel&&el.closest&&el.closest(skipsel))return null;"
    "if(el.getAttribute&&el.getAttribute('tabindex')==='-1'&&!el.href)return null;"
    "if(el.type==='hidden')return null;"
    "var r=el.getBoundingClientRect();"
    "var x=r.left+dx,y=r.top+dy;"
    "if(r.width<3||r.height<3)return null;"
    "if(x+r.width<=0||y+r.height<=0)return null;"
    "if(x>=innerWidth||y>=innerHeight)return null;"
    "var cs=getComputedStyle(el);"
    "if(cs.visibility==='hidden'||cs.display==='none')return null;"
    "if(parseFloat(cs.opacity)<0.05)return null;"
    "var cx=Math.min(Math.max(x+r.width/2,1),innerWidth-1);"
    "var cy=Math.min(Math.max(y+r.height/2,1),innerHeight-1);"
    "var hit=document.elementFromPoint(cx,cy);"
    "while(hit&&hit.shadowRoot){"
    "var deep=hit.shadowRoot.elementFromPoint(cx,cy);"
    "if(!deep||deep===hit)break;hit=deep;}"
    "if(!hit)return null;"
    "if(hit!==el&&!el.contains(hit)&&!hit.contains(el)){"
    // the middle is covered, try the near corner
    "cx=Math.min(Math.max(x+4,1),innerWidth-1);"
    "cy=Math.min(Math.max(y+Math.min(r.height/2,8),1),innerHeight-1);"
    "hit=document.elementFromPoint(cx,cy);"
    "if(!hit||(hit!==el&&!el.contains(hit)&&!hit.contains(el)))return null;}"
    "return {el:el,x:cx,y:cy,top:y,left:x};}"

    // prefix-free labels, short ones first: of k chars, t are held back as first
    // characters and the k-t left over stand alone, so k-t+t*k covers n
    "function names(n,chars){"
    "var k=chars.length,out=[],i;"
    "if(n<=k){for(i=0;i<n;i++)out.push(chars[i]);return out;}"
    "var t=Math.ceil((n-k)/(k-1));if(t>k)t=k;"
    "for(i=t;i<k;i++)out.push(chars[i]);"
    "var per=names(Math.ceil((n-out.length)/t),chars);"
    "for(var p=0;p<t&&out.length<n;p++)"
    "for(var q=0;q<per.length&&out.length<n;q++)out.push(chars[p]+per[q]);"
    "out.sort(function(a,b){return a.length-b.length;});"
    "return out;}"

    "function clear(){if(box)box.remove();box=null;labels=[];typed='';}"

    "function build(k,chars,only,skip){"
    "clear();kind=k;sel=SEL;skipsel='';"
    // each selector tried once before anything is walked with it
    "var bad=0;"
    "if(only){try{document.querySelector(only);sel=only;}catch(e){bad=1;}}"
    "if(skip){try{document.querySelector(skip);skipsel=skip;}catch(e){bad=1;}}"
    "if(bad)say('badsel');"
    "var raw=[],hits=[],i;"
    "collect(document,0,0,raw);"
    "for(i=0;i<raw.length;i++){var h=pick(raw[i][0],raw[i][1],raw[i][2]);if(h)hits.push(h);}"
    "hits.sort(function(a,b){return (a.top-b.top)||(a.left-b.left);});"
    "if(!hits.length){say('ready 0');return;}"
    "var nm=names(hits.length,chars.split(''));"
    "box=document.createElement('div');"
    "set(box,'position','fixed');set(box,'inset','0');"
    "set(box,'pointer-events','none');set(box,'z-index','2147483647');"
    "var sh=box.attachShadow({mode:'open'});"
    "document.documentElement.appendChild(box);"
    "for(i=0;i<hits.length;i++){"
    "var t=document.createElement('span');"
    "t.textContent=nm[i].toUpperCase();"
    "set(t,'position','fixed');"
    "set(t,'left',Math.max(0,hits[i].left-4)+'px');"
    "set(t,'top',Math.max(0,hits[i].top-4)+'px');"
    "set(t,'font','bold 11px/1.1 monospace');"
    "set(t,'padding','1px 3px');set(t,'color','#000');"
    "set(t,'background','#fbe14d');set(t,'border','1px solid #8a6d00');"
    "set(t,'border-radius','3px');set(t,'letter-spacing','1px');"
    "set(t,'box-shadow','0 1px 2px rgba(0,0,0,.4)');"
    "sh.appendChild(t);"
    "labels.push({name:nm[i],node:t,hit:hits[i]});}"
    "say('ready '+labels.length);}"

    "function filter(){"
    "var live=0,exact=null;"
    "for(var i=0;i<labels.length;i++){"
    "var m=labels[i].name.indexOf(typed)===0;"
    "set(labels[i].node,'display',m?'block':'none');"
    "if(m){live++;if(labels[i].name===typed)exact=labels[i];}}"
    "return {live:live,exact:exact};}"

    "function type(ch){"
    "typed+=ch;"
    "var r=filter();"
    "if(!r.live){clear();say('cancel');return;}"
    "if(r.exact&&r.live===1)go(r.exact);}"

    "function back(){"
    "if(!typed){clear();say('cancel');return;}"
    "typed=typed.slice(0,-1);filter();}"

    // reports where to click; the window sends the click itself
    "function go(l){"
    "var href=l.hit.el.getAttribute('href')||'';"
    "var abs='';try{abs=href?new URL(href,location.href).href:'';}catch(e){}"
    "var x=Math.round(l.hit.x),y=Math.round(l.hit.y);"
    "clear();"                     // off the page before the click lands
    "say('click '+x+' '+y+' '+kind+' '+abs);}"

    // commands arrive in data-webhint; the event is the nudge to read it
    "document.addEventListener('webhint',function(){"
    "var e=document.documentElement;"
    "var d=e.getAttribute('data-webhint')||'';"
    "var p=d.split(' ');"
    // the selectors ride on attributes of their own: they hold spaces
    "if(p[0]==='s')build(parseInt(p[1],10)||0,p[2]||'asdfghjkl',"
    "e.getAttribute('data-webhint-only')||'',"
    "e.getAttribute('data-webhint-skip')||'');"
    "else if(p[0]==='t')type(p[1]||'');"
    "else if(p[0]==='b')back();"
    "else clear();"
    "},true);"
    "})()";

// ------------------------------------------------------------------ the window

// dispatchEvent runs the listener before it returns
static void hint_cmd(App *a, const char *cmd) {
    char js[256];
    snprintf(js, sizeof js,
             "(function(d){d.setAttribute('data-webhint','%s');"
             "document.dispatchEvent(new Event('webhint'));"
             "d.removeAttribute('data-webhint');})(document.documentElement)",
             cmd);
    run_js(a, js);
}

// into a single quoted js string
static void js_quote(Buf *b, const char *s) {
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20) continue;
        if (*s == '\\' || *s == '\'') buf_add(b, "\\", 1);
        buf_add(b, s, 1);
    }
}

static void hint_start(App *a, int kind, const char *only, const char *skip) {
    Buf js = {0};
    buf_addf(&js, "(function(d){d.setAttribute('data-webhint','s %d %s');",
             kind, HINT_CHARS);
    if (only) {
        buf_addf(&js, "d.setAttribute('data-webhint-only','");
        js_quote(&js, only);
        buf_addf(&js, "');");
    }
    if (skip) {
        buf_addf(&js, "d.setAttribute('data-webhint-skip','");
        js_quote(&js, skip);
        buf_addf(&js, "');");
    }
    buf_addf(&js, "document.dispatchEvent(new Event('webhint'));"
                  "d.removeAttribute('data-webhint');"
                  "d.removeAttribute('data-webhint-only');"
                  "d.removeAttribute('data-webhint-skip');"
                  "})(document.documentElement)");
    buf_add(&js, "", 1);

    Buf esc = {0};
    json_escape_buf(&esc, js.p);
    buf_add(&esc, "", 1);
    app_cdp(a, "Runtime.evaluate", "\"expression\":\"%s\"", esc.p);
    buf_free(&esc);
    buf_free(&js);
}

void hint_install(App *a, bool fresh) {
    // the binding is per session, the script per document
    app_cdp(a, "Runtime.addBinding",
            "\"name\":\"__webhint\",\"executionContextName\":\"%s\"", WEB_WORLD);
    if (!fresh) return;
    Buf esc = {0};
    json_escape_buf(&esc, HINT_JS);
    buf_add(&esc, "", 1);
    app_cdp(a, "Page.addScriptToEvaluateOnNewDocument",
            "\"source\":\"%s\",\"worldName\":\"%s\",\"runImmediately\":true",
            esc.p, WEB_WORLD);
    buf_free(&esc);
}

void hint_show(App *a, int kind, bool all) {
    if (!a->has_tty) return;
    if (a->pdf) {
        notify(a, "no labels in a pdf");
        return;
    }
    hint_start(a, kind, all ? NULL : hint_selector(a->url, false),
                             all ? NULL : hint_selector(a->url, true));
    a->hint_on = true;
    a->hint_kind = kind;
    a->hint_n = 0;
    a->hint_typed[0] = 0;
    a->hint_deadline = now_sec() + HINT_WAIT;
}

void hint_cancel(App *a) {
    if (!a->hint_on) return;
    a->hint_on = false;
    a->hint_n = 0;
    a->hint_typed[0] = 0;
    a->hint_deadline = 0;
    hint_cmd(a, "c");
    still_soon(a);
}

// the way out when the page never answered
void hint_tick(App *a) {
    if (!a->hint_on || a->hint_deadline == 0) return;
    if (now_sec() < a->hint_deadline) return;
    a->hint_deadline = 0;
    hint_cancel(a);
    notify(a, "no labels came back");
}

bool hint_key(App *a, Event *ev) {
    if (!a->hint_on || ev->type != EV_KEY) return false;

    if (ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)) {
        hint_cancel(a);
        return false;
    }
    if (ev->key == KEY_ESC) {
        hint_cancel(a);
        return true;
    }
    if (ev->key == KEY_BACKSPACE) {
        size_t n = strlen(a->hint_typed);
        if (n) a->hint_typed[n - 1] = 0;
        hint_cmd(a, "b");
        if (!n) {
            a->hint_on = false;      // the page is clearing them; nothing typed
            a->hint_deadline = 0;
        }
        return true;
    }
    if (ev->key > 0 && ev->key < 0x80 && strchr(HINT_CHARS, ev->key)) {
        size_t n = strlen(a->hint_typed);
        if (n + 1 < sizeof a->hint_typed) {
            a->hint_typed[n] = (char)ev->key;
            a->hint_typed[n + 1] = 0;
        }
        char cmd[8];
        snprintf(cmd, sizeof cmd, "t %c", (char)ev->key);
        hint_cmd(a, cmd);
        return true;
    }
    hint_cancel(a);
    return true;
}

// space separated, the address last and the three numbers before it space free
void hint_reply(App *a, char *payload) {
    char *rest = strchr(payload, ' ');
    if (rest) *rest++ = 0;

    if (!strcmp(payload, "ready")) {
        a->hint_deadline = 0;
        a->hint_n = rest ? atoi(rest) : 0;
        if (a->hint_n == 0) {
            a->hint_on = false;
            notify(a, "nothing to click in view");
        }
        still_soon(a);
        return;
    }
    if (!strcmp(payload, "badsel")) {
        notify(a, "hint rule is not a selector");
        return;
    }
    if (!strcmp(payload, "cancel")) {
        a->hint_on = false;
        a->hint_deadline = 0;
        a->hint_typed[0] = 0;
        still_soon(a);
        return;
    }
    if (strcmp(payload, "click") || !rest) return;

    char *f[4] = {0};
    for (int i = 0; i < 4 && rest; i++) {
        f[i] = rest;
        char *sp = strchr(rest, ' ');
        if (sp) { *sp = 0; rest = sp + 1; }
        else      rest = NULL;
    }
    int x = f[0] ? atoi(f[0]) : 0;
    int y = f[1] ? atoi(f[1]) : 0;
    int kind = f[2] ? atoi(f[2]) : 0;
    const char *href = f[3] ? f[3] : "";

    a->hint_on = false;
    a->hint_deadline = 0;
    a->hint_typed[0] = 0;

    if (kind == 2) {
        clipboard_put(href[0] ? href : a->url);
        notify(a, href[0] ? "link copied" : "address copied");
        return;
    }
    if (kind == 1 && href[0]) {
        tab_open_url(a, href);
        return;
    }

    app_cdp(a, "Input.dispatchMouseEvent",
            "\"type\":\"mouseMoved\",\"x\":%d,\"y\":%d,\"button\":\"none\","
            "\"buttons\":0", x, y);
    app_cdp(a, "Input.dispatchMouseEvent",
            "\"type\":\"mousePressed\",\"x\":%d,\"y\":%d,\"button\":\"left\","
            "\"buttons\":1,\"clickCount\":1", x, y);
    app_cdp(a, "Input.dispatchMouseEvent",
            "\"type\":\"mouseReleased\",\"x\":%d,\"y\":%d,\"button\":\"left\","
            "\"buttons\":0,\"clickCount\":1", x, y);
    a->expect_frame = now_sec() + 2.0;
}
