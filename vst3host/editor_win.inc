// The Windows editor (included by vst3_host.cpp after the Host type): plain GDI, 720x420, two tabs.
//   Plugin: the plugins on offer (click to switch), the Q-Link section (4 or 8 knobs, drag to turn; Mode, Bank < >, and the Next button that cycles the banks).
//   Manager: the OpenMPC catalog (Refresh), select a plugin and Install; it lands in <root>/sdcard/Synths and is switched to. (installer.h)
// Immediate mode: paint() records the clickable rectangles, the mouse handlers look them up. The skin view of the hosted plugin comes later (M4b).
#include "installer.h"
#include <map>
#include <algorithm>
using std::min; using std::max;
#include <gdiplus.h>

static std::wstring W(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static const int kMinW = 960, kMinH = 560;   // the generic tabs; a plugin with a skin gets the skin's own size 1:1 (see want_size)
enum HitKind { H_MENU, H_MENUCLOSE, H_BANK, H_SKINPAGE, H_TAB, H_PLUGIN, H_MODE, H_BANKPREV, H_BANKNEXT, H_NEXT, H_PAGEPREV, H_PAGENEXT, H_KNOB, H_ITEM, H_REFRESH, H_INSTALL };
struct Hit { RECT r; int kind, arg; };
struct QSnap { int mode = 4, bank = 0, nbanks = 1, page = 0, npages = 0; std::string pagename; double val[8] = {}; bool act[8] = {}; std::string name[8], disp[8]; };
struct TabSnap { bool ok = false; std::vector<std::string> names; int cur = -1; std::string pname, err; QSnap q; };   // what the generic tabs draw: taken under the emulator lock, drawn without it
static const int kQStripH = 112;   // the Q-Link strip: a 26 px button row, then four or eight horizontal faders
struct View {
    const PlugViewVtbl* vt; uint32_t refs = 1;
    Host* h; HWND wnd = nullptr; void* frame = nullptr;
    int tab = 2, msel = -1, mscroll = 0, drag = -1; double dragy = 0, dragv = 0;
    std::vector<Hit> hits;
    std::mutex sm; std::string status = "Press Refresh to read the catalog.";
    std::vector<pm::Item> items; std::atomic<bool> busy{false};
    TabSnap ts; std::map<int, std::string> ncache; int nc_cur = -2; std::string pname_s; RECT qtrack[8] = {};
    bool menu = false;   // the plugin list (a drop-down under the header) is open
    int vw = kMinW, vh = kMinH;   // the window's size in pixels
    std::shared_ptr<pm::Skin> csk; std::vector<float> cpv; std::vector<std::string> ctexts; int cpage_ = -1;   // the last snapshot of the skin page: drawn when the emulator is busy
    int spage = 0, sdrag = -1; double sdragy = 0, sdragv = 0; float sscale = 1;   // the skin tab: page, drag, scale
    std::map<std::string, Gdiplus::Bitmap*> bmps; std::string bmps_dir;   // the picture cache is for one plugin's skin folder; file names repeat between plugins (sh_bg_0.png)
    ~View() { for (auto& kv : bmps) delete kv.second; }
};
static Gdiplus::Bitmap* skin_image(View* v, const std::string& dir, const std::string& name) {
    if (name.empty()) return nullptr;
    if (v->bmps_dir != dir) { for (auto& kv : v->bmps) delete kv.second; v->bmps.clear(); v->bmps_dir = dir; }
    auto it = v->bmps.find(name);
    if (it != v->bmps.end()) return it->second;
    std::wstring w = W(dir + "/" + name);
    Gdiplus::Bitmap* b = new Gdiplus::Bitmap(w.c_str());
    if (b->GetLastStatus() != Gdiplus::Ok) { delete b; b = nullptr; }
    v->bmps[name] = b;
    return b;
}
#define VW (v->vw)
#define VH (v->vh)
static void view_release(View* v) { if (--v->refs == 0) { Host* h = v->h; delete v; release(h); } }
static void ui_edit(View* v, uint32_t id, double val, int phase) {   // phase 0 begin+set, 1 set, 2 set+end; the host hears it as an edit
    Host* h = v->h;
    { std::lock_guard<std::mutex> l(h->mu); apply_param_change(h, id, val); }
    if (IComponentHandler* hd = h->handler) {
        if (phase == 0) hd->v->beginEdit(hd, id);
        hd->v->performEdit(hd, id, val);
        if (phase == 2) hd->v->endEdit(hd, id);
    }
}
static void say(View* v, const std::string& s) { std::lock_guard<std::mutex> l(v->sm); v->status = s; }

static void do_refresh(View* v) {
    if (v->busy.exchange(true)) return;
    v->refs++;
    std::thread([v] {
        say(v, "Reading the catalog...");
        std::vector<pm::Item> items; std::string err;
        const char* e = std::getenv("OPENMPC_CATALOG");
        if (pm::load_catalog(e ? e : "https://open-mpc.github.io/catalog.json", "aarch64", items, err)) {
            std::lock_guard<std::mutex> l(v->sm);
            v->items = std::move(items);
            v->status = std::to_string(v->items.size()) + " plugins. Select one and press Install.";
        } else say(v, "Catalog: " + err);
        v->busy = false;
        view_release(v);
    }).detach();
}
static void do_install(View* v) {
    pm::Item it;
    { std::lock_guard<std::mutex> l(v->sm); if (v->msel < 0 || v->msel >= (int)v->items.size()) return; it = v->items[v->msel]; }
    if (v->busy.exchange(true)) return;
    v->refs++;
    std::thread([v, it] {
        say(v, "Installing " + it.name + " " + it.version + "...");
        std::string folder, err;
        if (!pm::install(it, user_dir(), folder, err)) say(v, "Install failed: " + err);
        else {
            Host* h = v->h;
            int idx = -1;
            {
                std::lock_guard<std::mutex> l(h->mu);
                if (h->sim) h->sim->write_settings();
                std::string keep = h->cur >= 0 ? h->entries[h->cur].name : "";
                h->entries = scan_plugins();
                for (int i = 0; i < (int)h->entries.size(); i++) if (h->entries[i].name == folder) idx = i;
                h->cur = -1;
                for (int i = 0; i < (int)h->entries.size(); i++) if (h->entries[i].name == keep && h->hp) h->cur = i;
            }
            if (h->handler) h->handler->v->restartComponent(h->handler, 1 | 4);
            if (idx >= 0) request_switch(h, idx);
            say(v, "Installed " + it.name + " " + it.version + ".");
        }
        v->busy = false;
        view_release(v);
    }).detach();
}

static void take_snap(View* v, Host* h) {   // the emulator lock is held: a few calls, nothing drawn
    TabSnap& S = v->ts;
    S.ok = true;
    S.names.clear();
    for (auto& e : h->entries) S.names.push_back(e.name);
    S.cur = h->cur;
    if (v->nc_cur != h->cur) { v->ncache.clear(); v->pname_s.clear(); v->nc_cur = h->cur; }
    if (h->hp && v->pname_s.empty()) v->pname_s = h->hp->name();
    S.pname = h->hp ? v->pname_s : h->load_error;
    S.err = h->hp && h->hp->dead() ? "The plugin stopped: " + h->hp->fault() + ". Switch away and back to restart it." : h->switch_error;
    QSnap& q = S.q;
    q.mode = h->qmode; q.bank = h->qbank; q.nbanks = nbanks(h); q.page = h->qpage; q.npages = (int)h->pages.size();
    q.pagename = q.npages ? (h->pages[h->qpage % q.npages].name.empty() ? "Page " + std::to_string(h->qpage + 1) : h->pages[h->qpage % q.npages].name) : "";
    for (int k = 0; k < 8; k++) {
        q.val[k] = h->qval[k]; q.act[k] = false; q.name[k] = "-"; q.disp[k].clear();
        if (k >= h->qmode) continue;
        int p = qparam(h, k);
        if (p < 0 || !h->hp) continue;
        q.act[k] = true;
        auto it = v->ncache.find(p);
        if (it == v->ncache.end()) it = v->ncache.emplace(p, h->hp->param_name(p)).first;
        q.name[k] = it->second;
        q.disp[k] = h->hp->param_display(p);
    }
}
// The window the hosted plugin's skin wants: its native pixels (a 1280x628 page) plus our header (44) and page tabs (42), never smaller than the generic tabs.
static void want_size(Host* h, int& w, int& hh) {
    w = kMinW; hh = kMinH;
    if (h->skin) { w = std::max(kMinW, h->skin->w + 16); hh = std::max(kMinH, 44 + h->skin->h + kQStripH + 42); }
}
static void resize_view(View* v, int w, int h) {   // ask the host for a new window size (it calls onSize); the window follows at once
    if (w == v->vw && h == v->vh) return;
    v->vw = w; v->vh = h;
    if (v->wnd) SetWindowPos(v->wnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
    if (v->frame) { ViewRect r{0, 0, w, h}; ((PlugFrameVtbl* const*)v->frame)[0]->resizeView(v->frame, &v->vt, &r); }
}
static void paint(View* v, HDC dc, RECT cr) {
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, VW, VH);
    SelectObject(m, bmp);
    const COLORREF bg = RGB(24, 26, 30), panel = RGB(36, 39, 46), fg = RGB(225, 228, 235), dim = RGB(130, 136, 148), acc = RGB(255, 140, 40);
    HBRUSH bb = CreateSolidBrush(bg); RECT all{0, 0, VW, VH}; FillRect(m, &all, bb); DeleteObject(bb);
    HFONT f = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HFONT fb = CreateFontA(-17, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    SelectObject(m, f); SetBkMode(m, TRANSPARENT);
    v->hits.clear();
    auto box = [&](RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(m, &r, b); DeleteObject(b); };
    auto text = [&](const std::string& s, RECT r, COLORREF c, UINT fl = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, HFONT ff = nullptr) {
        HGDIOBJ old = SelectObject(m, ff ? ff : f); SetTextColor(m, c);
        std::wstring w = W(s); DrawTextW(m, w.c_str(), (int)w.size(), &r, fl); SelectObject(m, old);
    };
    auto button = [&](RECT r, const std::string& s, int kind, int arg, bool on = false) {
        box(r, on ? acc : RGB(52, 56, 66));
        text(s, r, on ? RGB(20, 20, 20) : fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        v->hits.push_back({r, kind, arg});
    };

    Host* h = v->h;
    std::unique_lock<std::mutex> lk(h->mu, std::try_to_lock);
    if (lk.owns_lock()) take_snap(v, h);
    const TabSnap& S = v->ts;
    // one header, as on the MPC: the plugin (a drop-down list of what is installed) and the catalog; the skin's own page tabs are the only tab row
    text("OpenMPC Host", RECT{16, 8, 190, 36}, fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE, fb);
    if (v->tab == 1) button(RECT{200, 8, 340, 36}, "<  Back to plugin", H_TAB, 2);
    else {
        const std::string pl = S.ok ? (S.pname.empty() && S.cur >= 0 && S.cur < (int)S.names.size() ? S.names[S.cur] : S.pname) : "Loading...";
        button(RECT{200, 8, 520, 36}, pl + "   \xE2\x96\xBE", H_MENU, 0, v->menu);
        button(RECT{528, 8, 640, 36}, "Manager", H_TAB, 1);
    }
    auto qstrip = [&](int x, int y, int w) {   // the Q-Link section: a panel with mode, numbered bank buttons and Next, then 4 or 8 horizontal faders in one row
        const QSnap& q = S.q;
        const COLORREF pbg = RGB(31, 34, 41), line = RGB(52, 56, 66);
        const int ph = kQStripH - 8;
        box(RECT{x, y, x + w, y + ph}, pbg);
        box(RECT{x, y, x + w, y + 1}, line);
        const int pad = 14, ry = y + 10;
        int bx = x + pad;
        text("Q-LINKS", RECT{bx, ry, bx + 62, ry + 28}, dim, DT_LEFT | DT_VCENTER | DT_SINGLELINE); bx += 70;
        button(RECT{bx, ry, bx + 34, ry + 28}, "4", H_MODE, 4, q.mode == 4); bx += 36;
        button(RECT{bx, ry, bx + 34, ry + 28}, "8", H_MODE, 8, q.mode == 8); bx += 34 + 22;
        text("BANK", RECT{bx, ry, bx + 42, ry + 28}, dim, DT_LEFT | DT_VCENTER | DT_SINGLELINE); bx += 46;
        if (q.nbanks <= 8) {
            for (int b = 0; b < q.nbanks; b++) { button(RECT{bx, ry, bx + 38, ry + 28}, std::to_string(b + 1), H_BANK, b, b == q.bank); bx += 42; }
        } else {   // a plugin without a skin map has many banks: step through them
            button(RECT{bx, ry, bx + 30, ry + 28}, "<", H_BANKPREV, 0); bx += 34;
            text(std::to_string(q.bank + 1) + " / " + std::to_string(q.nbanks), RECT{bx, ry, bx + 64, ry + 28}, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE); bx += 68;
            button(RECT{bx, ry, bx + 30, ry + 28}, ">", H_BANKNEXT, 0); bx += 34;
        }
        bx += 8;
        button(RECT{bx, ry, bx + 76, ry + 28}, "Next \xE2\x96\xB8", H_NEXT, 0);   // the device's Q-Link button: the next bank
        if (!q.pagename.empty()) text(q.pagename, RECT{x + w - pad - 260, ry, x + w - pad, ry + 28}, dim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        const int n = q.mode, gap = 16, ix = x + pad, iw = w - 2 * pad, cw = (iw - (n - 1) * gap) / n, fy = ry + 28 + 14;
        for (int k = 0; k < n; k++) {
            const int cx = ix + k * (cw + gap), nw = cw * 6 / 10;
            text(std::to_string(k + 1) + "  " + q.name[k], RECT{cx, fy, cx + nw, fy + 18}, q.act[k] ? fg : dim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            text(q.disp[k], RECT{cx + nw, fy, cx + cw, fy + 18}, q.act[k] ? acc : dim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT tr{cx, fy + 24, cx + cw, fy + 36};
            box(tr, RGB(48, 52, 62));
            const int fw = (int)((tr.right - tr.left) * std::min(1.0, std::max(0.0, q.val[k])));
            box(RECT{tr.left, tr.top, tr.left + fw, tr.bottom}, q.act[k] ? acc : RGB(80, 85, 98));
            const int tx = std::min((int)tr.right - 8, std::max((int)tr.left, (int)tr.left + fw - 4));
            box(RECT{tx, tr.top - 3, tx + 8, tr.bottom + 3}, q.act[k] ? RGB(240, 242, 246) : RGB(120, 126, 138));
            v->hits.push_back({RECT{cx, fy - 2, cx + cw, fy + 40}, H_KNOB, k});
            v->qtrack[k] = tr;
        }
    };
    if (v->tab == 2) {
        if (lk.owns_lock()) {   // a snapshot, so the emulator is not held while drawing
            auto sk = h->skin; auto pv = h->pv; std::vector<std::string> texts;
            if (sk && v->spage >= (int)sk->pages.size()) v->spage = 0;
            if (sk) {   // the processor reads these back from the plugin, so buttons it resets itself show as off
                std::vector<int> w;
                for (auto& it : sk->pages[v->spage].items) { if (it.param >= 0) w.push_back(it.param); for (auto& c : it.cond) w.push_back(c.param); }
                std::sort(w.begin(), w.end()); w.erase(std::unique(w.begin(), w.end()), w.end());
                h->watch = std::move(w);
            }
            if (sk) for (auto& it : sk->pages[v->spage].items) {
                std::string t;
                if (it.kind == pm::SkinItem::Label) { if (it.name) t = it.text; else if (it.param >= 0 && it.param < h->nparams && h->hp) t = h->hp->param_display(it.param); }
                texts.push_back(t);
            }
            lk.unlock();
            v->csk = sk; v->cpv = pv; v->ctexts = texts; v->cpage_ = v->spage;
        }
        std::shared_ptr<pm::Skin> sk = v->csk; std::vector<float> pv = v->cpv; std::vector<std::string> texts = v->ctexts;
        if (sk && v->cpage_ != v->spage) sk = nullptr;   // the page changed since the snapshot: wait for a fresh one
        if (!sk) text("No skin to show (the plugin has none, or it is still loading).", RECT{16, 60, VW, 90}, dim);
        else {
            int nt = (int)sk->pages.size(), tw = (VW - 32) / nt;
            for (int i = 0; i < nt; i++) button(RECT{16 + i * tw, VH - 34, 16 + (i + 1) * tw - 4, VH - 8}, sk->pages[i].name, H_SKINPAGE, i, i == v->spage);   // the page tabs sit under the picture
            const int ox = 8, oy = 44;
            float s = std::min((VW - 2 * ox) / (float)sk->w, (VH - oy - kQStripH - 42) / (float)sk->h);
            v->sscale = s;
            auto val = [&](int p) { return p >= 0 && p < (int)pv.size() ? pv[p] : 0.0f; };
            auto opt = [&](int p, int n) { return n <= 1 ? (val(p) > 0.5f ? 1 : 0) : (int)lround(val(p) * (n - 1)); };
            Gdiplus::Graphics g(m);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
            g.SetClip(Gdiplus::Rect(ox, oy, (int)(sk->w * s), (int)(sk->h * s)));
            Gdiplus::SolidBrush black(Gdiplus::Color(255, 0, 0, 0));
            g.FillRectangle(&black, ox, oy, (int)(sk->w * s), (int)(sk->h * s));
            const auto& items = sk->pages[v->spage].items;
            for (size_t k = 0; k < items.size(); k++) {
                const pm::SkinItem& it = items[k];
                if (it.kind == pm::SkinItem::Hit) continue;
                bool shown = true;
                for (auto& c : it.cond) shown = shown && opt(c.param, c.count) == c.option;
                if (!shown) continue;
                Gdiplus::Rect dst(ox + (int)lround(it.x * s), oy + (int)lround(it.y * s), (int)lround(it.w * s), (int)lround(it.h * s));
                auto draw = [&](Gdiplus::Bitmap* b, int sy, int sh) { if (b) g.DrawImage(b, dst, 0, sy, (int)b->GetWidth(), sh, Gdiplus::UnitPixel); };
                if (it.kind == pm::SkinItem::Img) { Gdiplus::Bitmap* b = skin_image(v, sk->dir, it.src); if (b) draw(b, 0, (int)b->GetHeight()); }
                else if (it.kind == pm::SkinItem::Film) {
                    Gdiplus::Bitmap* b = skin_image(v, sk->dir, it.src);
                    if (b) {   // frames are image height / count tall; knobs carry one more square frame than numFrames says
                        int iw = (int)b->GetWidth(), ih = (int)b->GetHeight(), n = it.frames > 0 ? it.frames : 127, nimg = ih == (n + 1) * iw ? n + 1 : n;
                        double fh = (double)ih / nimg;
                        int fr = (int)lround(std::min(1.0f, std::max(0.0f, val(it.param))) * (nimg - 1));
                        draw(b, (int)(fr * fh), (int)fh);
                    }
                } else if (it.kind == pm::SkinItem::Btn) {
                    bool on = it.param >= 0 && opt(it.param, it.n > 1 ? it.n : 1) == (it.n > 1 ? it.id : 1);
                    Gdiplus::Bitmap* b = skin_image(v, sk->dir, on ? it.on : it.off);
                    if (b) draw(b, 0, (int)b->GetHeight());
                } else if (it.kind == pm::SkinItem::Label && k < texts.size()) {
                    std::string t = texts[k];
                    if (it.upper) for (auto& ch : t) ch = (char)toupper((unsigned char)ch);
                    int style = it.style.find("Bold") != std::string::npos ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular;
                    Gdiplus::Font font(L"Segoe UI", (Gdiplus::REAL)(it.size * s * 0.8), style, Gdiplus::UnitPixel);
                    Gdiplus::StringFormat sf; sf.SetLineAlignment(Gdiplus::StringAlignmentCenter); sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap); sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
                    sf.SetAlignment(it.just == 'c' ? Gdiplus::StringAlignmentCenter : it.just == 'r' ? Gdiplus::StringAlignmentFar : Gdiplus::StringAlignmentNear);
                    Gdiplus::SolidBrush br(Gdiplus::Color((it.colour >> 24) & 255, (it.colour >> 16) & 255, (it.colour >> 8) & 255, it.colour & 255));
                    std::wstring wt = W(t);
                    g.DrawString(wt.c_str(), (INT)wt.size(), &font, Gdiplus::RectF((Gdiplus::REAL)dst.X, (Gdiplus::REAL)dst.Y, (Gdiplus::REAL)dst.Width, (Gdiplus::REAL)dst.Height), &sf, &br);
                }
            }
            g.Flush();
            qstrip(ox, oy + (int)(sk->h * s) + 6, VW - 2 * ox);
        }
        if (!sk && S.ok) qstrip(16, 100, VW - 32);
    } else {
        if (lk.owns_lock()) lk.unlock();
        std::lock_guard<std::mutex> l(v->sm);
        button(RECT{16, 48, 116, 74}, "Refresh", H_REFRESH, 0);
        button(RECT{124, 48, 224, 74}, "Install", H_INSTALL, 0, v->msel >= 0);
        text(v->status + (v->busy ? "  ..." : ""), RECT{236, 48, VW - 16, 74}, dim);
        int rows = (VH - 90) / 24;
        for (int i = 0; i < rows; i++) {
            int idx = v->mscroll + i;
            if (idx >= (int)v->items.size()) break;
            RECT r{16, 84 + i * 24, VW - 16, 84 + i * 24 + 22};
            box(r, idx == v->msel ? RGB(80, 56, 30) : panel);
            const pm::Item& it = v->items[idx];
            text(it.name, RECT{24, r.top, 300, r.bottom}, fg);
            text(it.version, RECT{310, r.top, 380, r.bottom}, dim);
            text(it.kind, RECT{390, r.top, 480, r.bottom}, dim);
            text(it.author, RECT{490, r.top, VW - 24, r.bottom}, dim);
            v->hits.push_back({r, H_ITEM, idx});
        }
    }
    if (v->menu && v->tab != 1 && S.ok) {   // the plugin list: any click outside it closes it
        v->hits.push_back({RECT{0, 0, VW, VH}, H_MENUCLOSE, 0});
        const int rows = (int)S.names.size();
        box(RECT{198, 38, 522, 44 + rows * 28 + 4}, RGB(60, 65, 78));
        box(RECT{200, 40, 520, 42 + rows * 28 + 2}, RGB(36, 39, 46));
        for (int i = 0; i < rows; i++) button(RECT{204, 44 + i * 28, 516, 44 + i * 28 + 26}, S.names[i], H_PLUGIN, i, i == S.cur);
    }
    BitBlt(dc, 0, 0, VW, VH, m, 0, 0, SRCCOPY);
    DeleteObject(f); DeleteObject(fb); DeleteObject(bmp); DeleteDC(m);
    (void)cr;
}

static void skin_click(View* v, int x, int y) {   // a tap on the skin picture: the last visible hit under the pointer
    Host* h = v->h;
    std::shared_ptr<pm::Skin> sk; std::vector<float> pv;
    { std::unique_lock<std::mutex> l(h->mu, std::try_to_lock); if (!l.owns_lock()) return; sk = h->skin; pv = h->pv; }
    if (!sk || v->spage >= (int)sk->pages.size()) return;
    double px = (x - 8) / v->sscale, py = (y - 44) / v->sscale;
    auto val = [&](int p) { return p >= 0 && p < (int)pv.size() ? pv[p] : 0.0f; };
    auto opt = [&](int p, int n) { return n <= 1 ? (val(p) > 0.5f ? 1 : 0) : (int)lround(val(p) * (n - 1)); };
    const auto& items = sk->pages[v->spage].items;
    for (int i = (int)items.size() - 1; i >= 0; i--) {
        const pm::SkinItem& it = items[i];
        if (it.kind != pm::SkinItem::Hit || px < it.x || py < it.y || px >= it.x + it.w || py >= it.y + it.h) continue;
        bool shown = true;
        for (auto& c : it.cond) shown = shown && opt(c.param, c.count) == c.option;
        if (!shown || it.param < 0) continue;
        uint32_t id = kPluginBase + it.param;
        if (it.mode == pm::SkinItem::Radio) { ui_edit(v, id, it.n > 1 ? (double)it.id / (it.n - 1) : 0, 0); if (h->handler) h->handler->v->endEdit(h->handler, id); }
        else if (it.mode == pm::SkinItem::Toggle) { ui_edit(v, id, val(it.param) > 0.5f ? 0 : 1, 0); if (h->handler) h->handler->v->endEdit(h->handler, id); }
        else if (it.mode == pm::SkinItem::Drag) { v->sdrag = it.param; v->sdragy = y; v->sdragv = val(it.param); if (h->handler) h->handler->v->beginEdit(h->handler, id); }
        return;
    }
}
static double fader_val(View* v, int k, int x) {   // a horizontal fader: the pointer's place along its track
    const RECT& t = v->qtrack[k];
    double r = t.right > t.left ? (double)(x - t.left) / (t.right - t.left) : 0;
    return r < 0 ? 0 : r > 1 ? 1 : r;
}
static void click(View* v, int x, int y) {
    Host* h = v->h;
    for (int i = (int)v->hits.size() - 1; i >= 0; i--) {
        const Hit& ht = v->hits[i];
        if (x < ht.r.left || x >= ht.r.right || y < ht.r.top || y >= ht.r.bottom) continue;
        switch (ht.kind) {
            case H_TAB: v->tab = ht.arg; v->menu = false; break;
            case H_MENU: v->menu = !v->menu; break;
            case H_MENUCLOSE: v->menu = false; break;
            case H_BANK: ui_edit(v, kQBank, ht.arg / 15.0, 0); if (h->handler) h->handler->v->endEdit(h->handler, kQBank); break;
            case H_SKINPAGE: {   // the skin's tab also picks the matching Q-Link page, as on the device
                v->spage = ht.arg;
                int np = (int)h->pages.size();
                for (int j = 0; j < np; j++) if (h->pages[j].tab == ht.arg + 1 && h->pages[j].sub == 1) { ui_edit(v, kPage, np > 1 ? j / (double)(np - 1) : 0, 0); if (h->handler) h->handler->v->endEdit(h->handler, kPage); break; }
                break;
            }
            case H_PLUGIN: { v->menu = false; std::lock_guard<std::mutex> l(h->mu); request_switch(h, ht.arg); break; }
            case H_MODE: ui_edit(v, kQMode, ht.arg == 8 ? 1 : 0, 0); if (h->handler) h->handler->v->endEdit(h->handler, kQMode); break;
            case H_BANKPREV: case H_BANKNEXT: {
                int nb = nbanks(h), b = (h->qbank + (ht.kind == H_BANKNEXT ? 1 : nb - 1)) % nb;
                ui_edit(v, kQBank, b / 15.0, 0); if (h->handler) h->handler->v->endEdit(h->handler, kQBank);
                break;
            }
            case H_PAGEPREV: case H_PAGENEXT: {
                int np = (int)h->pages.size(), pg = (h->qpage + (ht.kind == H_PAGENEXT ? 1 : np - 1)) % (np ? np : 1);
                ui_edit(v, kPage, np > 1 ? pg / (double)(np - 1) : 0, 0); if (h->handler) h->handler->v->endEdit(h->handler, kPage);
                break;
            }
            case H_NEXT: ui_edit(v, kQNext, 1, 0); ui_edit(v, kQNext, 0, 2); break;
            case H_KNOB: { v->drag = ht.arg; if (h->handler) h->handler->v->beginEdit(h->handler, kQLink1 + ht.arg); ui_edit(v, kQLink1 + ht.arg, fader_val(v, ht.arg, x), 1); break; }
            case H_ITEM: { std::lock_guard<std::mutex> l(v->sm); v->msel = ht.arg; break; }
            case H_REFRESH: do_refresh(v); break;
            case H_INSTALL: do_install(v); break;
        }
        return;
    }
    if (v->tab == 2) skin_click(v, x, y);
}

static LRESULT wndproc2(HWND w, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {   // an exception must not cross user32
    try { return wndproc2(w, msg, wp, lp); } catch (const std::exception& e) { logf(std::string("editor: ") + e.what()); } catch (...) { logf("editor: exception"); }
    return DefWindowProcA(w, msg, wp, lp);
}
static LRESULT wndproc2(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    View* v = (View*)GetWindowLongPtrA(w, GWLP_USERDATA);
    if (!v) return DefWindowProcA(w, msg, wp, lp);
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps); paint(v, dc, ps.rcPaint); EndPaint(w, &ps); return 0; }
        case WM_PRINTCLIENT: paint(v, (HDC)wp, RECT{0, 0, VW, VH}); return 0;
        case WM_TIMER: {
            int ww, hh;
            { std::unique_lock<std::mutex> l(v->h->mu, std::try_to_lock); if (l.owns_lock()) { want_size(v->h, ww, hh); l.unlock(); resize_view(v, ww, hh); } }   // the plugin was switched: another skin, another size
            InvalidateRect(w, nullptr, FALSE); return 0;
        }
        case WM_LBUTTONDOWN: SetCapture(w); click(v, (short)LOWORD(lp), (short)HIWORD(lp)); InvalidateRect(w, nullptr, FALSE); return 0;
        case WM_MOUSEMOVE:
            if (v->sdrag >= 0) {
                double val = v->sdragv + (v->sdragy - (short)HIWORD(lp)) / 150.0;
                ui_edit(v, kPluginBase + v->sdrag, val < 0 ? 0 : val > 1 ? 1 : val, 1);
            }
            if (v->drag >= 0) {
                ui_edit(v, kQLink1 + v->drag, fader_val(v, v->drag, (short)LOWORD(lp)), 1);
            }
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            if (v->sdrag >= 0) { Host* h = v->h; if (h->handler) h->handler->v->endEdit(h->handler, kPluginBase + v->sdrag); v->sdrag = -1; }
            if (v->drag >= 0) { Host* h = v->h; if (h->handler) h->handler->v->endEdit(h->handler, kQLink1 + v->drag); v->drag = -1; }
            return 0;
        case WM_MOUSEWHEEL:
            if (v->tab == 1) { std::lock_guard<std::mutex> l(v->sm); v->mscroll += (short)HIWORD(wp) > 0 ? -2 : 2; if (v->mscroll < 0) v->mscroll = 0; if (v->mscroll > (int)v->items.size()) v->mscroll = (int)v->items.size(); }
            return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static tresult pv_query(void* s, const TUID iid, void** o) {
    View* v = (View*)((char*)s - offsetof(View, vt));
    if (iid_eq(iid, iid_FUnknown) || iid_eq(iid, iid_IPlugView)) { v->refs++; *o = &v->vt; return kResultOk; }
    *o = nullptr; return kNoInterface;
}
static uint32_t pv_ref(void* s) { return ++((View*)((char*)s - offsetof(View, vt)))->refs; }
static uint32_t pv_rel(void* s) { View* v = (View*)((char*)s - offsetof(View, vt)); uint32_t r = v->refs - 1; view_release(v); return r; }
static tresult pv_supported(void*, const char* t) { return !strcmp(t, "HWND") ? kResultOk : kResultFalse; }
static tresult pv_attached(void* s, void* parent, const char* type) {
    View* v = (View*)((char*)s - offsetof(View, vt));
    if (strcmp(type, "HWND")) return kResultFalse;
    static bool reg = false;
    static ULONG_PTR gdip = 0;
    if (!gdip) { Gdiplus::GdiplusStartupInput in; Gdiplus::GdiplusStartup(&gdip, &in, nullptr); }
    if (!reg) {
        WNDCLASSA wc{}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(nullptr); wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "OpenMPCHostView";
        RegisterClassA(&wc); reg = true;
    }
    { std::unique_lock<std::mutex> l(v->h->mu, std::try_to_lock); if (l.owns_lock()) want_size(v->h, v->vw, v->vh); }
    v->wnd = CreateWindowExA(0, "OpenMPCHostView", "", WS_CHILD | WS_VISIBLE, 0, 0, VW, VH, (HWND)parent, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!v->wnd) return kResultFalse;
    SetWindowLongPtrA(v->wnd, GWLP_USERDATA, (LONG_PTR)v);
    SetTimer(v->wnd, 1, 50, nullptr);
    return kResultOk;
}
static tresult pv_removed(void* s) {
    View* v = (View*)((char*)s - offsetof(View, vt));
    if (v->wnd) { KillTimer(v->wnd, 1); SetWindowLongPtrA(v->wnd, GWLP_USERDATA, 0); DestroyWindow(v->wnd); v->wnd = nullptr; }
    return kResultOk;
}
static tresult pv_nop(void*, float) { return kResultFalse; }
static tresult pv_key(void*, char16, int16_t, int16_t) { return kResultFalse; }
static tresult pv_size(void* s, ViewRect* r) { View* v = (View*)((char*)s - offsetof(View, vt)); r->left = r->top = 0; r->right = VW; r->bottom = VH; return kResultOk; }
static tresult pv_onsize(void* s, ViewRect* r) { View* v = (View*)((char*)s - offsetof(View, vt)); if (r && r->right - r->left > 0 && r->bottom - r->top > 0) { v->vw = r->right - r->left; v->vh = r->bottom - r->top; } if (v->wnd) SetWindowPos(v->wnd, nullptr, 0, 0, VW, VH, SWP_NOMOVE | SWP_NOZORDER); return kResultOk; }
static tresult pv_focus(void*, uint8_t) { return kResultOk; }
static tresult pv_frame(void* s, void* f) { View* v = (View*)((char*)s - offsetof(View, vt)); v->frame = f; return kResultOk; }
static tresult pv_can_resize(void*) { return kResultFalse; }
static tresult pv_check(void* s, ViewRect* r) { View* v = (View*)((char*)s - offsetof(View, vt)); r->right = r->left + VW; r->bottom = r->top + VH; return kResultOk; }
static const PlugViewVtbl pv_vtbl = {pv_query, pv_ref, pv_rel, pv_supported, pv_attached, pv_removed, pv_nop, pv_key, pv_key, pv_size, pv_onsize, pv_focus, pv_frame, pv_can_resize, pv_check};
static void* create_view(Host* h) { View* v = new View(); v->vt = &pv_vtbl; v->h = h; h->refs++;
    bool mgr = false;
    {
        std::lock_guard<std::mutex> l(h->mu);
        if (h->skin) v->tab = 2;   // a plugin with a skin opens on it
        want_size(h, v->vw, v->vh);
        mgr = h->cur >= 0 && h->entries[h->cur].name.find("Plugin Manager") != std::string::npos;
    }
    (void)mgr;
    return &v->vt; }
