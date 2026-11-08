#include "skin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "installer.h"
#include "json_mini.h"

namespace pm {

static std::vector<int> rectof(const J& b) {
    std::vector<int> v;
    const char* p = b.str("bounds").c_str();
    std::string s = b.str("bounds");
    p = s.c_str();
    while (*p && v.size() < 4) { char* e; double d = strtod(p, &e); if (e == p) break; v.push_back((int)lround(d)); p = e; }
    v.resize(4, 0);
    return v;
}
static std::vector<SkinItem::Cond> handles(const J& c) {
    std::vector<SkinItem::Cond> out;
    const J* b = c.get("bounds");
    const J* a = b ? b->get("additionalInvalidatingHandles") : nullptr;
    if (!a || a->t != J::Arr) return out;
    for (const J& h : a->a) {
        int i, n, p;
        if (h.t == J::Str && sscanf(h.s.c_str(), "IndexedEnabling/%d/%d/Parameter %d", &i, &n, &p) == 3) out.push_back({p, i, n});
    }
    return out;
}
static std::map<std::string, int> remap(const J& c) {   // handle name -> parameter index
    std::map<std::string, int> out;
    const J* r = c.get("handle remapping");
    const J* m = r ? r->get("map") : nullptr;
    if (!m || m->t != J::Arr) return out;
    for (const J& e : m->a) {
        int p;
        if (sscanf(e.str("value").c_str(), "Parameter %d", &p) == 1) out[e.get("key") ? e.str("key") : "Data"] = p;
    }
    return out;
}
static const J* num(const J& o, const char* k) { const J* j = o.get(k); return j && j->t == J::Num ? j : nullptr; }

bool load_skin(const std::string& dir, Skin& sk, std::string& err) {
    std::vector<uint8_t> buf;
    if (!http_get(dir + "/TUI.json", buf, err)) return false;
    J root; P ps{(const char*)buf.data(), (const char*)buf.data() + buf.size()};
    if (!ps.val(root)) { err = "TUI.json is not valid"; return false; }
    const J* pd = root.get("pageData");
    const J* tabs = pd ? pd->get("tabs") : nullptr;
    const J* cd = pd ? pd->get("componentDefinitions") : nullptr;
    const J* ld = cd ? cd->get("localComponentDefinitions") : nullptr;
    if (!tabs || tabs->t != J::Arr) { err = "TUI.json has no tabs"; return false; }
    std::map<std::string, const J*> defs;
    if (ld && ld->t == J::Arr) for (const J& d : ld->a) if (const J* v = d.get("value")) defs[d.str("key")] = v;
    sk = Skin();
    sk.dir = dir;
    bool first = true;
    for (const J& tab : tabs->a) {
        const J* page = tab.get("componentDefinition");
        if (!page) { auto it = defs.find(tab.str("componentName")); if (it == defs.end()) continue; page = it->second; }
        if (first) {
            first = false;
            int a, b, w, h;
            if (sscanf(tab.str("initialSize").c_str(), "%d %d %d %d", &a, &b, &w, &h) == 4) { sk.w = w; sk.h = h; }
        }
        SkinPage out;
        out.name = tab.str("tabName");
        if (const J* k = num(tab, "fnKeyIndex")) out.fk = (int)k->n; else out.fk = (int)sk.pages.size();
        if (const J* k = num(tab, "fnKeySubIndex")) out.fs = (int)k->n;
        const J* comps = page->get("componentsData");
        if (comps && comps->t == J::Arr) for (const J& c : comps->a) {
            const J* cdat = c.get("componentData");
            const J* cb = c.get("bounds");
            if (!cdat || !cb) continue;
            auto r = rectof(*cb);
            auto cond = handles(c);
            const std::string type = cdat->str("type");
            const J* data = cdat->get("data");
            auto add = [&](SkinItem it) { it.cond = cond; out.items.push_back(std::move(it)); };
            if (type == "Image") {
                if (data && !data->str("image").empty()) { SkinItem it; it.kind = SkinItem::Img; it.x = r[0]; it.y = r[1]; it.w = r[2]; it.h = r[3]; it.src = data->str("image"); add(it); }
                continue;
            }
            auto di = defs.find(type);
            if (di == defs.end()) continue;
            const J* d = di->second;
            auto params = remap(c);
            int data_p = params.count("Data") ? params["Data"] : -1;
            bool has_group = false, has_knob = false, covered = false;
            int gid = 1, gn = 1, gp = -1, br[4] = {0, 0, 0, 0};
            std::vector<std::vector<int>> value_rects;
            const J* subs = d->get("componentsData");
            if (subs && subs->t == J::Arr) for (const J& s : subs->a) {
                const J* sd = s.get("componentData");
                const J* sb = s.get("bounds");
                if (!sd || !sb) continue;
                if (sb->str("whenVisible") == "WhenFocussed" || sd->str("type") == "Focus") continue;
                auto sr = rectof(*sb);
                const J* dd = sd->get("data");
                J empty;
                if (!dd) dd = &empty;
                std::string hn = dd->get("handleName") ? dd->str("handleName") : "Data";
                int p = params.count(hn) ? params[hn] : data_p;
                const std::string st = sd->str("type");
                SkinItem it; it.x = r[0] + sr[0]; it.y = r[1] + sr[1]; it.w = sr[2]; it.h = sr[3]; it.param = p;
                if (st == "Image" && !dd->str("image").empty()) { it.kind = SkinItem::Img; it.src = dd->str("image"); add(it); }
                else if (st == "Knob" && !dd->str("filmStrip").empty()) {
                    has_knob = true; it.kind = SkinItem::Film; it.src = dd->str("filmStrip");
                    const J* nf = num(*dd, "numFrames"); it.frames = nf && nf->n > 0 ? (int)nf->n : 127; add(it);
                } else if (st == "Button") {
                    it.kind = SkinItem::Btn; it.on = dd->str("onImage"); it.off = dd->str("offImage");
                    const J* bi = num(*dd, "buttonId"); const J* nb = num(*dd, "numButtonsInGroup");
                    it.id = bi ? (int)bi->n : 1; it.n = nb ? (int)nb->n : 1;
                    if (!has_group) { has_group = true; gid = it.id; gn = it.n; gp = p; br[0] = sr[0]; br[1] = sr[1]; br[2] = sr[2]; br[3] = sr[3]; }
                    add(it);
                } else if (st == "Label") {
                    const J* ts = dd->get("textStyle");
                    std::string just = ts ? ts->str("justification") : "";
                    it.kind = SkinItem::Label;
                    it.just = just.find("horizontallyCentred") != std::string::npos ? 'c' : just.find("right") != std::string::npos ? 'r' : 'l';
                    it.name = dd->str("type") == "Name";
                    if (!it.name) value_rects.push_back(sr);
                    else it.text = cdat->str("name");
                    it.upper = ts && ts->str("case") == "Upper Case";
                    std::string col = ts ? ts->str("colour") : "";
                    it.colour = col.empty() ? 0xffffffffu : (unsigned)strtoul(col.c_str(), nullptr, 16);
                    const J* f = ts ? ts->get("font") : nullptr;
                    const J* fh = f ? num(*f, "height") : nullptr;
                    it.size = fh ? fh->n : 20; it.style = f && !f->str("style").empty() ? f->str("style") : "SemiBold";
                    add(it);
                }
            }
            if (has_group) for (auto& vr : value_rects)
                if (vr[0] < br[0] + br[2] && br[0] < vr[0] + vr[2] && vr[1] < br[1] + br[3] && br[1] < vr[1] + vr[3]) covered = true;
            bool mouse_toggle = false, qlink = false;
            if (const J* acts = d->get("actions")) if (acts->t == J::Arr) for (const J& a : acts->a) {
                if (a.str("onAction") == "Mouse Down" && a.str("handler") == "Toggle Switch") mouse_toggle = true;
                if (a.str("onAction") == "Mouse Down" && a.str("handler") == "Q-Link") qlink = true;
            }
            const J* ign = d->get("ignoreMousePresses");
            bool tap = mouse_toggle || (has_group && !covered);
            if (ign && ign->n) tap = false;
            SkinItem h; h.kind = SkinItem::Hit; h.x = r[0]; h.y = r[1]; h.w = r[2]; h.h = r[3];
            if (tap && (data_p >= 0 || has_group)) {
                h.param = has_group ? gp : data_p; h.id = has_group ? gid : 1; h.n = has_group ? gn : 1; h.mode = h.n > 1 ? SkinItem::Radio : SkinItem::Toggle; add(h);
            } else if (data_p >= 0 && has_knob && qlink) { h.param = data_p; h.mode = SkinItem::Drag; add(h); }
        }
        sk.pages.push_back(std::move(out));
    }
    return !sk.pages.empty();
}

std::vector<std::vector<int>> skin_keys(const Skin& s) {
    std::vector<std::vector<int>> keys;
    std::vector<int> ids;
    for (auto& p : s.pages) if (std::find(ids.begin(), ids.end(), p.fk) == ids.end()) ids.push_back(p.fk);
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        std::vector<int> pg;
        for (int i = 0; i < (int)s.pages.size(); i++) if (s.pages[i].fk == id) pg.push_back(i);
        std::stable_sort(pg.begin(), pg.end(), [&](int a, int b) { return s.pages[a].fs < s.pages[b].fs; });
        keys.push_back(pg);
    }
    return keys;
}

}  // namespace pm
