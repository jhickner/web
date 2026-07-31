#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "web.h"

// Oklab chroma the tint mode paints with. Enough to read clearly as a colour
// wash; much more and it stops looking like the terminal's own background.
#define TINT_CHROMA 0.08

// Samples in each transfer table. The ramps are smooth and SVG interpolates
// between the entries, so this is far more than the eye can pick apart.
#define RAMP_N 33

// The ids the injected markup uses. Prefixed so nothing on the page collides
// with them, and stable so a second injection replaces the first.
#define HOLDER_ID "__web_recolor"
#define STYLE_ID  "__web_recolor_css"
#define FILTER_ID "__web_recolor_f"

// Rec. 709 weights, applied to gamma-encoded channels: the tables below are
// built against the same measure, so the pair round-trips exactly for greys.
#define LUMA_ROW "0.2126 0.7152 0.0722 0 0 "

typedef struct { double L, a, b; } Lab;

// ------------------------------------------------------------ colour space

static double srgb_to_linear(double c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double linear_to_srgb(double c) {
    if (c <= 0.0) return 0.0;
    if (c >= 1.0) return 1.0;
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// Ottosson's Oklab. Operates on linear sRGB.
static Lab rgb_to_lab(double r, double g, double b) {
    r = srgb_to_linear(r);
    g = srgb_to_linear(g);
    b = srgb_to_linear(b);
    double l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
    double m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
    double s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;
    double l_ = cbrt(l), m_ = cbrt(m), s_ = cbrt(s);
    Lab o;
    o.L = 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_;
    o.a = 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_;
    o.b = 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_;
    return o;
}

static void lab_to_rgb(Lab c, double out[3]) {
    double l_ = c.L + 0.3963377774 * c.a + 0.2158037573 * c.b;
    double m_ = c.L - 0.1055613458 * c.a - 0.0638541728 * c.b;
    double s_ = c.L - 0.0894841775 * c.a - 1.2914855480 * c.b;
    double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    out[0] = linear_to_srgb( 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s);
    out[1] = linear_to_srgb(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s);
    out[2] = linear_to_srgb(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
}

static Lab lab_of_u8(const uint8_t c[3]) {
    return rgb_to_lab(c[0] / 255.0, c[1] / 255.0, c[2] / 255.0);
}

static Lab lab_mix(Lab p, Lab q, double t) {
    Lab o;
    o.L = p.L + (q.L - p.L) * t;
    o.a = p.a + (q.a - p.a) * t;
    o.b = p.b + (q.b - p.b) * t;
    return o;
}

// ------------------------------------------------------------------ modes

// Tango, used when the terminal will not say what it is drawing with.
void theme_fallback(Theme *t) {
    memset(t, 0, sizeof *t);
    t->fg[0] = 0xd3; t->fg[1] = 0xd7; t->fg[2] = 0xcf;
    t->bg[0] = 0x00; t->bg[1] = 0x00; t->bg[2] = 0x00;
}

static const char *mode_names[RECOLOR_COUNT] = {"off", "hue", "duotone", "tint"};

const char *recolor_mode_name(int mode) {
    return (mode >= 0 && mode < RECOLOR_COUNT) ? mode_names[mode] : "off";
}

int recolor_mode_from_name(const char *s) {
    if (!s) return -1;
    for (int i = 0; i < RECOLOR_COUNT; i++)
        if (!strcasecmp(s, mode_names[i])) return i;
    return -1;
}

// Where a neutral of this luma lands. Every mode here is a function of
// lightness alone, which is what lets three per-channel transfer tables stand
// in for the whole mapping - and lets Chrome do it in the compositor rather
// than us doing it to every frame on its way to the terminal.
static void ramp_color(int mode, const Theme *t, double luma, double out[3]) {
    Lab bg = lab_of_u8(t->bg), fg = lab_of_u8(t->fg);
    // Oklab lightness of the grey with that luma: for a neutral the three cone
    // responses are all the linear luminance, so the cube roots collapse.
    double L = cbrt(srgb_to_linear(luma));

    Lab o;
    if (mode == RECOLOR_TINT) {
        // Every colour becomes a tone of the background: its hue over the
        // source's own lightness, so shading and structure survive while the
        // palette collapses to one colour.
        o.L = L;
        // The background's own chroma is no use as the tint strength. Terminal
        // backgrounds are nearly neutral - a dark blue-grey like #1e1e2e
        // carries a chroma of about 0.01, which is invisible - so take only the
        // hue and give it a chroma that reads. Tapered towards black and white,
        // where a saturated tone would clip on the way back to sRGB.
        double bc = hypot(bg.a, bg.b);
        if (bc > 1e-4) {
            double taper = 2.0 * (1.0 - fabs(2.0 * L - 1.0));
            if (taper > 1.0) taper = 1.0;
            double c = TINT_CHROMA * taper;
            o.a = bg.a / bc * c;
            o.b = bg.b / bc * c;
        } else {
            o.a = o.b = 0.0;        // a grey background can only mean grey
        }
    } else {
        double span = fg.L - bg.L;
        if (fabs(span) < 1e-6) span = 1.0;
        double tt = (L - bg.L) / span;
        if (tt < 0.0) tt = 0.0;
        if (tt > 1.0) tt = 1.0;
        o = lab_mix(bg, fg, tt);
    }
    lab_to_rgb(o, out);
}

static void emit_tables(Buf *b, int mode, const Theme *t) {
    static const char *fn[3] = {"R", "G", "B"};
    for (int ch = 0; ch < 3; ch++) {
        buf_addf(b, "<feFunc%s type=\"table\" tableValues=\"", fn[ch]);
        for (int i = 0; i < RAMP_N; i++) {
            double rgb[3];
            ramp_color(mode, t, (double)i / (RAMP_N - 1), rgb);
            buf_addf(b, "%s%.4f", i ? " " : "", rgb[ch]);
        }
        buf_add(b, "\"/>", 3);
    }
}

static void emit_filter(Buf *b, int mode, double strength, const Theme *t) {
    buf_addf(b, "<svg xmlns=\"http://www.w3.org/2000/svg\">"
                "<filter id=\"" FILTER_ID "\" color-interpolation-filters=\"sRGB\" "
                "x=\"0%%\" y=\"0%%\" width=\"100%%\" height=\"100%%\">");

    buf_addf(b, "<feColorMatrix type=\"matrix\" values=\""
                LUMA_ROW LUMA_ROW LUMA_ROW "0 0 0 1 0\" result=\"g\"/>"
                "<feComponentTransfer in=\"g\" result=\"ramp\">");
    emit_tables(b, mode, t);
    buf_addf(b, "</feComponentTransfer>");

    const char *last = "ramp";
    if (mode == RECOLOR_HUE) {
        // How far the pixel sits from the grey of its own luma, summed over
        // the channels: a saturation measure that costs two primitives, where
        // a true one would need a per-channel maximum and minimum. Anything
        // carrying real colour keeps it; everything else - which on a page
        // means the text and the space around it - rides the ramp.
        buf_addf(b,
            "<feBlend in=\"SourceGraphic\" in2=\"g\" mode=\"difference\" result=\"d\"/>"
            "<feColorMatrix in=\"d\" type=\"matrix\" values=\""
            "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 0 0\" result=\"m0\"/>"
            "<feComponentTransfer in=\"m0\" result=\"mask\">"
            "<feFuncA type=\"table\" tableValues=\"0 0.5 0.9 1 1\"/>"
            "</feComponentTransfer>"
            // The mask lives in the alpha channel, so keeping the source
            // through it and laying that over the ramp is one operation each
            // and gets the edges right without any arithmetic on alpha.
            "<feComposite in=\"SourceGraphic\" in2=\"mask\" operator=\"in\" result=\"kept\"/>"
            "<feComposite in=\"kept\" in2=\"ramp\" operator=\"over\" result=\"mixed\"/>");
        last = "mixed";
    }

    if (strength < 0.999)
        buf_addf(b, "<feComposite in=\"%s\" in2=\"SourceGraphic\" "
                    "operator=\"arithmetic\" k1=\"0\" k2=\"%.4f\" k3=\"%.4f\" k4=\"0\"/>",
                 last, strength, 1.0 - strength);

    buf_addf(b, "</filter></svg>");
}

void recolor_script(Buf *out, int mode, double strength, const Theme *t) {
    Buf svg = {0};
    if (mode != RECOLOR_OFF) emit_filter(&svg, mode, strength, t);

    // Chrome resolves a CSS filter only against a definition in the same
    // document, so the SVG has to be a real element rather than a data URL.
    // It hangs off <html> rather than <body> because a single-page app that
    // replaces the body would otherwise take the filter down with it.
    buf_addf(out,
        "(function(){"
        "var SVG=%s%s%s;"
        // The page's background belongs to the canvas rather than to any
        // element, and the canvas is painted outside the root element's filter:
        // left alone, a white page keeps a white background while everything
        // drawn on it is recoloured. Restating it as a pseudo-element of <html>
        // puts it back inside the filter, where the rest of the page already is.
        "function bg(){"
        "var t=/^(transparent|rgba\\(0, 0, 0, 0\\))$/;"
        "var c=getComputedStyle(document.documentElement).backgroundColor;"
        "if(t.test(c)&&document.body)c=getComputedStyle(document.body).backgroundColor;"
        "return t.test(c)?'Canvas':c;}"
        "function go(){"
        "var d=document.documentElement;if(!d)return false;"
        "var h=document.getElementById('" HOLDER_ID "');"
        "var s=document.getElementById('" STYLE_ID "');"
        "if(!SVG){if(h)h.remove();if(s)s.remove();return true;}"
        "if(!h){h=document.createElement('div');h.id='" HOLDER_ID "';"
        "h.style.cssText='position:absolute;width:0;height:0;overflow:hidden';"
        "d.appendChild(h);}"
        // Rebuilding the markup drops and remakes the filter, which the page
        // flickers through, so it happens only when the mapping has changed.
        "if(h.__rc!==SVG){h.__rc=SVG;h.innerHTML=SVG;}"
        "if(!s){s=document.createElement('style');s.id='" STYLE_ID "';d.appendChild(s);}"
        "s.textContent=':root::before{content:\"\";position:fixed;inset:0;"
        "pointer-events:none;z-index:-2147483647;background:'+bg()+'}"
        ":root{filter:url(#" FILTER_ID ")!important}';"
        "return true;}"
        // At document start there is nothing to hang anything off yet, so the
        // first attempt usually fails and the readystate change does the work.
        "if(!go())document.addEventListener('readystatechange',go);"
        "})()",
        svg.len ? "'" : "", svg.len ? svg.p : "''", svg.len ? "'" : "");
    buf_free(&svg);
}
