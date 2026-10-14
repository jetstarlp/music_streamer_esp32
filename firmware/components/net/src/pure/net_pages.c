/*
 * HTML pages of the WiFi setup portal (German). One small page, inline CSS
 * and a few lines of inline JavaScript (optional: without it the user types
 * the network name), no external resources. Dark theme with the brass accent
 * of the device.
 */
#include "net_pure.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void net_buf_init(net_buf_t *b, char *mem, size_t cap)
{
    b->p = mem;
    b->cap = cap;
    b->len = 0;
    b->overflow = (cap == 0);
    if (cap) {
        mem[0] = '\0';
    }
}

static void put_n(net_buf_t *b, const char *s, size_t n)
{
    if (b->cap == 0) {
        return;
    }
    if (b->len + n >= b->cap) {
        n = b->cap - 1 - b->len;
        b->overflow = true;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void net_buf_puts(net_buf_t *b, const char *s)
{
    put_n(b, s, strlen(s));
}

void net_buf_put_esc(net_buf_t *b, const char *s)
{
    char tmp[8];
    for (; s && *s; s++) {
        char one[2] = {*s, '\0'};
        if (!net_html_escape(one, tmp, sizeof(tmp))) {
            b->overflow = true;
            return;
        }
        net_buf_puts(b, tmp);
    }
}

void net_buf_printf(net_buf_t *b, const char *fmt, ...)
{
    if (b->cap == 0 || b->len + 1 >= b->cap) {
        b->overflow = true;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->p[b->len] = '\0';
        b->overflow = true;
    } else if ((size_t)n >= b->cap - b->len) {
        b->len = b->cap - 1;
        b->overflow = true;
    } else {
        b->len += (size_t)n;
    }
}

/* Marshall style: black, brass #C9A45C, cream #F2E8D5. */
static const char CSS[] =
    "body{margin:0;background:#000;color:#F2E8D5;font:16px/1.45 system-ui,-apple-system,Roboto,sans-serif}"
    "main{max-width:440px;margin:0 auto;padding:18px 16px 32px}"
    "h1{font-size:19px;font-weight:700;letter-spacing:.14em;text-transform:uppercase;color:#C9A45C;"
    "margin:4px 0 14px;padding-bottom:10px;border-bottom:2px solid #C9A45C}"
    "h2{font-size:15px;color:#C9A45C;font-weight:600;margin:18px 0 6px}"
    "p{margin:8px 0}"
    ".n{display:flex;width:100%;justify-content:space-between;align-items:center;box-sizing:border-box;"
    "padding:13px 14px;margin:6px 0;background:#111;border:1px solid #3a3326;border-radius:10px;"
    "color:#F2E8D5;font:inherit;text-align:left}"
    ".n:active,.n:focus{border-color:#C9A45C}"
    ".n span{color:#C9A45C;white-space:nowrap;margin-left:10px}"
    "label{display:block;margin-top:12px;color:#c8bfae;font-size:14px}"
    "input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:13px;margin-top:4px;"
    "background:#111;border:1px solid #5a4f3a;border-radius:10px;color:#F2E8D5;font:inherit}"
    "input:focus{outline:none;border-color:#C9A45C}"
    ".c{display:flex;align-items:center;gap:8px}"
    "button.b{display:block;width:100%;box-sizing:border-box;margin-top:18px;padding:15px;border:0;"
    "border-radius:10px;background:#C9A45C;color:#000;font:inherit;font-weight:700;letter-spacing:.06em}"
    "a.l{color:#C9A45C}"
    ".e{background:#2a1512;border:1px solid #a5523f;border-radius:10px;padding:12px;margin:10px 0}"
    ".ok{background:#16130c;border:1px solid #C9A45C;border-radius:10px;padding:14px;margin:10px 0}"
    "small{display:block;color:#8f8776;margin-top:22px;font-size:13px}";

static void head(net_buf_t *b, int refresh_s, const char *refresh_url)
{
    net_buf_puts(b, "<!DOCTYPE html><html lang=de><head><meta charset=utf-8>"
                    "<meta name=viewport content=\"width=device-width,initial-scale=1\">");
    if (refresh_s > 0) {
        net_buf_printf(b, "<meta http-equiv=refresh content=\"%d;url=%s\">", refresh_s, refresh_url);
    }
    net_buf_puts(b, "<title>Music Streamer einrichten</title><style>");
    net_buf_puts(b, CSS);
    net_buf_puts(b, "</style></head><body><main><h1>Music Streamer</h1>");
}

static void foot(net_buf_t *b, const net_page_t *pg)
{
    net_buf_puts(b, "<small>Einrichtungs-Hotspot");
    if (pg->ap_ssid && pg->ap_ssid[0]) {
        net_buf_puts(b, " &bdquo;");
        net_buf_put_esc(b, pg->ap_ssid);
        net_buf_puts(b, "&ldquo;");
    }
    net_buf_puts(b, " &middot; http://192.168.4.1/</small></main></body></html>");
}

static void network_list(net_buf_t *b, const net_page_t *pg)
{
    static const char *const BARS[] = {"", "&#9602;", "&#9602;&#9604;", "&#9602;&#9604;&#9606;",
                                       "&#9602;&#9604;&#9606;&#9608;"};
    net_buf_puts(b, "<h2>Gefundene WLANs</h2>");
    if (pg->n_aps == 0 || pg->aps == NULL) {
        net_buf_puts(b, pg->scanning ? "<p>Suche nach WLANs &hellip;</p>"
                                     : "<p>Kein WLAN gefunden. Name unten von Hand eingeben.</p>");
    }
    for (size_t i = 0; pg->aps && i < pg->n_aps; i++) {
        const net_ap_info_t *ap = &pg->aps[i];
        net_buf_puts(b, "<button type=button class=n onclick=\"pick(this)\" data-s=\"");
        net_buf_put_esc(b, ap->ssid);
        net_buf_puts(b, "\">");
        net_buf_put_esc(b, ap->ssid);
        net_buf_printf(b, "<span>%s%s</span></button>", BARS[net_rssi_bars(ap->rssi)],
                       ap->secure ? " &#128274;" : "");
    }
    net_buf_puts(b, "<a class=l href=/scan>Liste aktualisieren</a>");
}

static void form(net_buf_t *b, const net_page_t *pg)
{
    net_buf_puts(b, "<p>Wähle dein WLAN und gib das Passwort ein. Der Music Streamer merkt sich "
                    "bis zu fünf WLANs.</p>");
    if (pg->error && pg->error[0]) {
        net_buf_puts(b, "<div class=e>");
        net_buf_put_esc(b, pg->error);
        net_buf_puts(b, "</div>");
    }
    network_list(b, pg);
    net_buf_puts(b, "<form id=f method=post action=/connect>"
                    "<label for=s>WLAN-Name (SSID)</label>"
                    "<input type=text id=s name=s maxlength=32 autocapitalize=off autocorrect=off "
                    "spellcheck=false required value=\"");
    net_buf_put_esc(b, pg->ssid ? pg->ssid : "");
    net_buf_puts(b, "\"><label for=p>Passwort</label>"
                    "<input type=password id=p name=p maxlength=64 autocapitalize=off autocorrect=off "
                    "spellcheck=false>"
                    "<label class=c><input type=checkbox onclick=\"f.p.type=this.checked?'text':'password'\">"
                    "Passwort anzeigen</label>"
                    "<button class=b type=submit>Verbinden</button></form>"
                    "<script>function pick(e){f.s.value=e.dataset.s;f.p.value='';f.p.focus()}</script>");
}

void net_page_render(net_buf_t *b, const net_page_t *pg)
{
    switch (pg->kind) {
    case NET_PAGE_CONNECTING:
        head(b, 2, "/status");
        net_buf_puts(b, "<p>Verbinde mit &bdquo;");
        net_buf_put_esc(b, pg->ssid ? pg->ssid : "");
        net_buf_puts(b, "&ldquo; &hellip;</p><p>Das dauert bis zu 30 Sekunden. Die Seite aktualisiert sich "
                        "von selbst.</p><p>Falls sie hängen bleibt: Handy wieder mit dem WLAN &bdquo;");
        net_buf_put_esc(b, pg->ap_ssid ? pg->ap_ssid : "");
        net_buf_puts(b, "&ldquo; verbinden und <a class=l href=/status>hier tippen</a>.</p>");
        break;
    case NET_PAGE_CONNECTED:
        head(b, 0, NULL);
        net_buf_puts(b, "<div class=ok><b>Verbunden!</b><br>Der Music Streamer ist jetzt mit &bdquo;");
        net_buf_put_esc(b, pg->ssid ? pg->ssid : "");
        net_buf_puts(b, "&ldquo; verbunden");
        if (pg->ip && pg->ip[0]) {
            net_buf_puts(b, " (IP-Adresse ");
            net_buf_put_esc(b, pg->ip);
            net_buf_puts(b, ")");
        }
        net_buf_puts(b, pg->restarting ? ".<br>Das Ger&auml;t startet neu &hellip;</div>" : ".</div>");
        net_buf_puts(b, "<p>Der Einrichtungs-Hotspot schaltet sich gleich ab. Verbinde dein Handy "
                        "wieder mit deinem WLAN, &ouml;ffne die Spotify-App und w&auml;hle unter "
                        "&bdquo;Ger&auml;te&ldquo; den Lautsprecher &bdquo;");
        net_buf_put_esc(b, pg->device && pg->device[0] ? pg->device : "Music Streamer");
        net_buf_puts(b, "&ldquo;.</p>");
        break;
    case NET_PAGE_FORM:
    default:
        head(b, 0, NULL);   /* no auto-reload: it would wipe what the user typed */
        form(b, pg);
        break;
    }
    foot(b, pg);
}
