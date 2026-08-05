#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

// Clicking without a mouse. `f` puts a label on everything in view worth
// clicking and the label typed is what gets clicked: two keystrokes to a link,
// on a page the terminal can only draw.
//
// The labels are elements of the page, so they arrive inside the picture like
// the rest of it - nothing here draws anything, and a label sits over its link
// wherever the link is rather than snapping to the character grid, which on the
// dense pages this is for is the difference between a list of links and a row
// of labels on top of each other.
//
// They are put there from WEB_WORLD, the world the focus watcher lives in: the
// page cannot see what we define and cannot style what we add, since the labels
// hang in a shadow root of their own. The traffic runs both ways round the same
// two channels everything else here uses - a `__webhint` binding for what the
// page has to say, and an attribute plus an event for what we have to say to
// it, because a world can only be spoken to through the DOM the two of them
// share.

// The label alphabet: nine of the home row. Two characters cover 81 links and
// three cover 729, which is past what a viewport holds.
#define HINT_CHARS "asdfghjkl"

// How long the page has to say the labels are up before they are given back. A
// page whose script has wedged would otherwise leave the keyboard in a mode
// that swallows everything.
#define HINT_WAIT 2.0

// ------------------------------------------------------------------ the page

// Installed once per document, in the isolated world. Everything it needs is
// its own: the page's own `f` key, its own CSS and its own MutationObserver
// see none of this.
//
// Written without a backslash anywhere in it, and without a double quote: it
// travels as a JSON string inside a CDP call, and one escape missed there is a
// script that silently never runs.
static const char HINT_JS[] =
    "(function(){"
    "if(window.__webhints)return;window.__webhints=1;"

    // Everything a click could reasonably be aimed at. Kept in one string so
    // the walk below is a single querySelectorAll per root.
    "var SEL='a[href],button,input,select,textarea,summary,details,label[for],"
    "[role=button],[role=link],[role=checkbox],[role=radio],[role=tab],"
    "[role=menuitem],[role=option],[onclick],[contenteditable],[tabindex]';"
    "var box=null,labels=[],typed='',kind=0;"
    // What the file's site rules made of the above for this page: the set to
    // look in, and the part of it to leave alone. Both are settled once in
    // build and read from there down, since every element found walks past them.
    "var sel=SEL,skipsel='';"

    "function say(s){try{__webhint(s);}catch(e){}}"
    // Through the CSSOM rather than a style attribute or a style element:
    // both of those are what a strict site's CSP refuses, and this is not.
    "function set(e,k,v){e.style.setProperty(k,v,'important');}"

    // Every candidate in this document, in any open shadow root under it, and
    // in any frame of the same origin, each carrying the offset of the frame
    // it was found in. A closed root and a cross-origin frame are the two
    // blind spots, and neither can be reached from here at all.
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

    // In view, big enough to aim at, and actually on top where it is aimed at.
    // The last of those is the whole difficulty: a rectangle says nothing about
    // the cookie banner over it, and labelling something a click cannot reach
    // is worse than not labelling it.
    "function pick(el,dx,dy){"
    "if(el.disabled)return null;"
    // A `hint-skip` rule names the part of the page to leave alone, so what it
    // matches takes everything under it with it - a rule is written against the
    // container a reader can see, not against the anchors inside it.
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
    // The middle is covered, so try the near corner: a wide link with an image
    // overlay across the centre of it is still clickable at its edge.
    "cx=Math.min(Math.max(x+4,1),innerWidth-1);"
    "cy=Math.min(Math.max(y+Math.min(r.height/2,8),1),innerHeight-1);"
    "hit=document.elementFromPoint(cx,cy);"
    "if(!hit||(hit!==el&&!el.contains(hit)&&!hit.contains(el)))return null;}"
    "return {el:el,x:cx,y:cy,top:y,left:x};}"

    // Labels no one of which is the start of another, short ones first: with k
    // characters and n targets, t of them are held back as first characters and
    // the k-t left over stand alone, so k-t+t*k covers n. The short labels then
    // go to the links nearest the top, which is where the eye already is.
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
    // Each rule tried on the document once before anything is walked with it: a
    // selector the file got wrong throws here, where it costs one call and can
    // be said out loud, rather than inside the walk, where it would throw the
    // labels away and look like a page that never answered.
    "var bad=0;"
    "if(only){try{document.querySelector(only);sel=only;}catch(e){bad=1;}}"
    "if(skip){try{document.querySelector(skip);skipsel=skip;}catch(e){bad=1;}}"
    "if(bad)say('badsel');"
    "var raw=[],hits=[],i;"
    "collect(document,0,0,raw);"
    "for(i=0;i<raw.length;i++){var h=pick(raw[i][0],raw[i][1],raw[i][2]);if(h)hits.push(h);}"
    // Reading order, so a link keeps roughly the same label from one press of
    // `f` to the next and the fingers start to remember it.
    "hits.sort(function(a,b){return (a.top-b.top)||(a.left-b.left);});"
    "if(!hits.length){say('ready 0');return;}"
    "var nm=names(hits.length,chars.split(''));"
    // On documentElement rather than body: a page whose framework owns the body
    // reconciles anything appended to it straight back out again.
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

    // The labels still in play are the ones that start with what has been
    // typed; the rest go away rather than dim, because a dimmed label is still
    // something to read on a page that is now mostly labels.
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

    // The click itself is not made here: an element clicked by script is not an
    // element clicked by a person - no hover, no user gesture, and a page that
    // listens for pointerdown never hears it. What goes back is where to click,
    // and the window sends a real one.
    "function go(l){"
    "var href=l.hit.el.getAttribute('href')||'';"
    "var abs='';try{abs=href?new URL(href,location.href).href:'';}catch(e){}"
    "var x=Math.round(l.hit.x),y=Math.round(l.hit.y);"
    "clear();"                     // off the page before the click lands on it
    "say('click '+x+' '+y+' '+kind+' '+abs);}"

    // What the window has to say arrives as an attribute, because that is the
    // one thing two worlds of the same page can both read. The event is only
    // the nudge to go and look.
    "document.addEventListener('webhint',function(){"
    "var e=document.documentElement;"
    "var d=e.getAttribute('data-webhint')||'';"
    "var p=d.split(' ');"
    // The two selectors ride on attributes of their own rather than in the
    // command: they hold spaces, which the line above splits on, and they are
    // longer than everything else here put together.
    "if(p[0]==='s')build(parseInt(p[1],10)||0,p[2]||'asdfghjkl',"
    "e.getAttribute('data-webhint-only')||'',"
    "e.getAttribute('data-webhint-skip')||'');"
    "else if(p[0]==='t')type(p[1]||'');"
    "else if(p[0]==='b')back();"
    "else clear();"
    "},true);"
    "})()";

// ------------------------------------------------------------------ the window

// One command to the labels. The attribute is put on, the event sent, and the
// attribute taken off again: dispatchEvent runs the listener before it returns,
// so nothing of ours is left on the page between one keystroke and the next.
static void hint_cmd(App *a, const char *cmd) {
    char js[256];
    snprintf(js, sizeof js,
             "(function(d){d.setAttribute('data-webhint','%s');"
             "document.dispatchEvent(new Event('webhint'));"
             "d.removeAttribute('data-webhint');})(document.documentElement)",
             cmd);
    run_js(a, js);
}

// Into a single quoted JS string. What goes out is escaped once more for the
// JSON it travels in, which is what takes care of the backslash this leaves
// behind; what that escape does not touch is the quote, and a selector is
// entitled to one - `a[href^='/item']` is a selector someone will write.
static void js_quote(Buf *b, const char *s) {
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20) continue;
        if (*s == '\\' || *s == '\'') buf_add(b, "\\", 1);
        buf_add(b, s, 1);
    }
}

// The one command carrying more than a few characters, and the only one that
// has to be built rather than printed: either selector may be as long as
// everything else in the message. The attributes go on and come off around the
// event exactly as the command's own does.
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
    // The binding is the session's and the script is the page's: a tab switched
    // back to needs the first again and already carries the second.
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
    // A PDF is drawn by the viewer extension, in a frame of another process
    // that nothing this document is asked ever reaches. There is no document
    // here to label.
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

// A page that never answered. Everything else about the labels is driven by
// what the page says, so this is the one way out that does not depend on it.
void hint_tick(App *a) {
    if (!a->hint_on || a->hint_deadline == 0) return;
    if (now_sec() < a->hint_deadline) return;
    a->hint_deadline = 0;
    hint_cancel(a);
    notify(a, "no labels came back");
}

bool hint_key(App *a, Event *ev) {
    if (!a->hint_on || ev->type != EV_KEY) return false;

    // Quit is quit from here as much as from anywhere; anything else with a
    // modifier on it puts the labels away and is then acted on normally, since
    // a chord is never part of a label.
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
    // Only the label alphabet gets through. A key that is not part of one is
    // not a mistyped label, it is somewhere else the user meant to be.
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

// Space separated rather than JSON: the address is the last field and the three
// numbers before it hold no spaces, so one walk of the string is the whole of
// the parsing.
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
        still_soon(a);              // the labels want the sharp picture
        return;
    }
    // Said on the way to the labels rather than instead of them: the rule the
    // file got wrong is dropped and the rest of the page is labelled as it
    // always was, so `f` still works while the line is being found and fixed.
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
    // A new tab needs somewhere to go, and only a link has one: a button that
    // opens a menu has nothing to put in a tab, so it is followed where it is.
    if (kind == 1 && href[0]) {
        tab_open_url(a, href);
        return;
    }

    // A move, then a press and a release, which is what a click on the page is
    // made of: hover styles settle, focus lands, and a window the click opens is
    // one the popup blocker lets through, because as far as Chrome is concerned
    // a person did this.
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
