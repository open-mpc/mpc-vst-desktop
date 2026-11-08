// A hosted plugin's skin (its TUI.json) flattened into drawable items, the same conversion tools/desktop_skin.py does (see its docstring), in memory.
// Parameters are the VST2 indices of the hosted plugin.
#pragma once
#include <string>
#include <vector>

namespace pm {

struct SkinItem {
    enum Kind { Img, Film, Btn, Label, Hit } kind = Img;
    int x = 0, y = 0, w = 0, h = 0;
    std::string src, on, off, text, style = "SemiBold";   // images are file names inside the skin folder
    int param = -1, frames = 127, id = 0, n = 0;           // Btn/Hit: option id of n (n = 1: a toggle)
    enum Mode { None, Radio, Toggle, Drag } mode = None;
    bool name = false, upper = false;                      // Label: the parameter's name (text) or its value
    char just = 'l';
    unsigned colour = 0xffffffff;                          // AARRGGBB
    double size = 20;
    struct Cond { int param, option, count; };             // shown while parameter `param` is at option `option` of `count`
    std::vector<Cond> cond;
};
struct SkinPage { std::string name; int fk = 0, fs = 0; std::vector<SkinItem> items; };   // fk, fs: the function key and the sub-page under it (the device has six keys; a key can hold several pages)
struct Skin { std::string dir; int w = 1280, h = 628; std::vector<SkinPage> pages; };

// Page indices by function key, in key order, each in sub-page order.
std::vector<std::vector<int>> skin_keys(const Skin& s);

bool load_skin(const std::string& skin_dir, Skin& out, std::string& err);   // <skin_dir>/TUI.json

}  // namespace pm
