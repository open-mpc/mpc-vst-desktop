// The Windows editor (included by vst3_host.cpp after the Host type): plain GDI, 720x420, two tabs.
//   Plugin: the plugins on offer (click to switch), the Q-Link section (4 or 8 knobs, drag to turn; Mode, Bank < >, and the Next button that cycles the banks).
//   Manager: the OpenMPC catalog (Refresh), select a plugin and Install; it lands in <root>/sdcard/Synths and is switched to. (installer.h)
// Immediate mode: paint() records the clickable rectangles, the mouse handlers look them up. The skin view of the hosted plugin comes later (M4b).
#include "installer.h"

enum HitKind { H_TAB, H_PLUGIN, H_MODE, H_BANKPREV, H_BANKNEXT, H_NEXT, H_PAGEPREV, H_PAGENEXT, H_KNOB, H_ITEM, H_REFRESH, H_INSTALL };
struct Hit { RECT r; int kind, arg; };
struct View {
    const PlugViewVtbl* vt; uint32_t refs = 1;
    Host* h; HWND wnd = nullptr; void* frame = nullptr;
    int tab = 0, msel = -1, mscroll = 0, drag = -1; double dragy = 0, dragv = 0;
    std::vector<Hit> hits;
    std::mutex sm; std::string status = "Press Refresh to read the catalog.";
    std::vector<pm::Item> items; std::atomic<bool> busy{false};
};
static const int VW = 720, VH = 420;
static std::wstring W(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
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
    text("OpenMPC Host", RECT{16, 8, 300, 36}, fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE, fb);
    button(RECT{330, 8, 430, 36}, "Plugin", H_TAB, 0, v->tab == 0);
    button(RECT{436, 8, 536, 36}, "Manager", H_TAB, 1, v->tab == 1);

    Host* h = v->h;
    std::unique_lock<std::mutex> lk(h->mu, std::try_to_lock);
    if (v->tab == 0) {
        if (!lk.owns_lock()) text("Loading the plugin...", RECT{16, 60, VW, 90}, dim);
        else {
            text("PLUGINS", RECT{16, 48, 220, 68}, dim);
            int y = 70;
            for (int i = 0; i < (int)h->entries.size() && y < VH - 30; i++, y += 26)
                button(RECT{16, y, 230, y + 24}, h->entries[i].name, H_PLUGIN, i, i == h->cur);
            RECT qp{250, 48, VW - 16, VH - 16}; box(qp, panel);
            std::string pn = h->hp ? h->hp->name() : h->load_error;
            text(pn.empty() ? "(unnamed)" : pn, RECT{262, 54, VW - 30, 66}, fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, fb);
            if (!h->switch_error.empty()) text(h->switch_error, RECT{262, 64, VW - 30, 86}, RGB(255, 110, 100), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            text("Q-LINKS", RECT{262, 86, 400, 104}, dim);
            button(RECT{262, 108, 342, 134}, h->qmode == 4 ? "4 knobs" : "8 knobs", H_MODE, 0);
            button(RECT{352, 108, 382, 134}, "<", H_BANKPREV, 0);
            text("Bank " + std::to_string(h->qbank + 1) + " / " + std::to_string(nbanks(h)), RECT{386, 108, 466, 134}, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            button(RECT{470, 108, 500, 134}, ">", H_BANKNEXT, 0);
            button(RECT{512, 108, 612, 134}, "Next bank", H_NEXT, 0);
            if (!h->pages.empty()) {
                button(RECT{262, 140, 292, 166}, "<", H_PAGEPREV, 0);
                const pm::QPage& pg = h->pages[h->qpage % h->pages.size()];
                text((pg.name.empty() ? "Page " + std::to_string(h->qpage + 1) : pg.name) + "   (" + std::to_string(h->qpage + 1) + " / " + std::to_string(h->pages.size()) + ")", RECT{296, 140, 520, 166}, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                button(RECT{524, 140, 554, 166}, ">", H_PAGENEXT, 0);
            }
            int per = h->qmode == 8 ? 4 : 4, rows = h->qmode == 8 ? 2 : 1, kw = (VW - 16 - 250 - 24) / per;
            for (int k = 0; k < h->qmode; k++) {
                int cx = 262 + (k % per) * kw + kw / 2, cy = 222 + (k / per) * 108, r = 30;
                RECT kr{cx - kw / 2 + 4, cy - 44, cx + kw / 2 - 4, cy + 62};
                v->hits.push_back({kr, H_KNOB, k});
                HPEN pen = CreatePen(PS_SOLID, 3, RGB(70, 75, 88)); HGDIOBJ op = SelectObject(m, pen); HGDIOBJ ob = SelectObject(m, GetStockObject(NULL_BRUSH));
                Ellipse(m, cx - r, cy - r, cx + r, cy + r); SelectObject(m, op); DeleteObject(pen);
                int p = qparam(h, k);
                double val = h->qval[k];
                double ang = (-225.0 + 270.0 * val) * 3.14159265 / 180.0;
                HPEN ap = CreatePen(PS_SOLID, 4, p >= 0 ? acc : RGB(70, 75, 88)); op = SelectObject(m, ap);
                MoveToEx(m, cx, cy, nullptr); LineTo(m, cx + (int)(cos(ang) * (r - 4)), cy - (int)(sin(ang) * (r - 4)));
                SelectObject(m, op); DeleteObject(ap); SelectObject(m, ob);
                std::string nm = p >= 0 && h->hp ? h->hp->param_name(p) : "-";
                std::string ds = p >= 0 && h->hp ? h->hp->param_display(p) : "";
                text(std::to_string(k + 1) + "  " + nm, RECT{kr.left, cy + r + 4, kr.right, cy + r + 24}, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                text(ds, RECT{kr.left, cy + r + 24, kr.right, cy + r + 42}, dim, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            (void)rows;
        }
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
    BitBlt(dc, 0, 0, VW, VH, m, 0, 0, SRCCOPY);
    DeleteObject(f); DeleteObject(fb); DeleteObject(bmp); DeleteDC(m);
    (void)cr;
}

static void click(View* v, int x, int y) {
    Host* h = v->h;
    for (int i = (int)v->hits.size() - 1; i >= 0; i--) {
        const Hit& ht = v->hits[i];
        if (x < ht.r.left || x >= ht.r.right || y < ht.r.top || y >= ht.r.bottom) continue;
        switch (ht.kind) {
            case H_TAB: v->tab = ht.arg; break;
            case H_PLUGIN: { std::lock_guard<std::mutex> l(h->mu); request_switch(h, ht.arg); break; }
            case H_MODE: ui_edit(v, kQMode, h->qmode == 4 ? 1 : 0, 0); if (h->handler) h->handler->v->endEdit(h->handler, kQMode); break;
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
            case H_KNOB: v->drag = ht.arg; v->dragy = y; v->dragv = h->qval[ht.arg]; if (h->handler) h->handler->v->beginEdit(h->handler, kQLink1 + ht.arg); break;
            case H_ITEM: { std::lock_guard<std::mutex> l(v->sm); v->msel = ht.arg; break; }
            case H_REFRESH: do_refresh(v); break;
            case H_INSTALL: do_install(v); break;
        }
        return;
    }
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
        case WM_TIMER: InvalidateRect(w, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN: SetCapture(w); click(v, (short)LOWORD(lp), (short)HIWORD(lp)); InvalidateRect(w, nullptr, FALSE); return 0;
        case WM_MOUSEMOVE:
            if (v->drag >= 0) {
                double val = v->dragv + (v->dragy - (short)HIWORD(lp)) / 150.0;
                ui_edit(v, kQLink1 + v->drag, val < 0 ? 0 : val > 1 ? 1 : val, 1);
            }
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
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
    if (!reg) {
        WNDCLASSA wc{}; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(nullptr); wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "OpenMPCHostView";
        RegisterClassA(&wc); reg = true;
    }
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
static tresult pv_size(void*, ViewRect* r) { r->left = r->top = 0; r->right = VW; r->bottom = VH; return kResultOk; }
static tresult pv_onsize(void* s, ViewRect*) { View* v = (View*)((char*)s - offsetof(View, vt)); if (v->wnd) SetWindowPos(v->wnd, nullptr, 0, 0, VW, VH, SWP_NOMOVE | SWP_NOZORDER); return kResultOk; }
static tresult pv_focus(void*, uint8_t) { return kResultOk; }
static tresult pv_frame(void* s, void* f) { View* v = (View*)((char*)s - offsetof(View, vt)); v->frame = f; return kResultOk; }
static tresult pv_can_resize(void*) { return kResultFalse; }
static tresult pv_check(void*, ViewRect* r) { r->right = r->left + VW; r->bottom = r->top + VH; return kResultOk; }
static const PlugViewVtbl pv_vtbl = {pv_query, pv_ref, pv_rel, pv_supported, pv_attached, pv_removed, pv_nop, pv_key, pv_key, pv_size, pv_onsize, pv_focus, pv_frame, pv_can_resize, pv_check};
static void* create_view(Host* h) { View* v = new View(); v->vt = &pv_vtbl; v->h = h; h->refs++; return &v->vt; }
