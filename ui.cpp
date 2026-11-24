// Grabber
// © Benoit Saint-Moulin - 2026
// Version du fichier : 1.2.0
// Fonction du fichier : Implementation of Grabber's windows: main window, card editor, about box, zone picker and click circles.

#include "toolkit.h"
#include "ui.h"

#include <objidl.h>
#include <gdiplus.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <ctime>

using namespace Gdiplus;

static const wchar_t* RIPPLE_CLASS = L"GrabberRipple";
static const wchar_t* PICKER_CLASS = L"GrabberPicker";
static const UINT WM_APP_STARTED = WM_APP + 1, WM_APP_DONE = WM_APP + 2, WM_APP_ZONE = WM_APP + 3;
static const int KEY_VK[4] = {0, VK_F5, VK_F6, VK_F11};
static const wchar_t* KEY_NAME[4] = {L"", L"F5", L"F6", L"F11"};

// tests: GRABBER_TRACE=file logs what the window does
static void trace(const string& s) {
    wchar_t p[MAX_PATH];
    if (GetEnvironmentVariableW(L"GRABBER_TRACE", p, MAX_PATH) == 0) return;
    FILE* f = _wfopen(p, L"ab");
    if (!f) return;
    string line = F("%.2f ", now()) + s + "\n";
    fwrite(line.data(), 1, line.size(), f);
    fclose(f);
}

static wstring fmt_w(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return W(buf);
}

// ------------------------------------------------------------------------------------------------ where things are kept
fs::path app_dir() {
    wchar_t home[MAX_PATH];
    if (GetEnvironmentVariableW(L"GRABBER_HOME", home, MAX_PATH) > 0) return fs::path(home);          // tests and portable use
    PWSTR roaming = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) base = roaming;
    CoTaskMemFree(roaming);
    return base / L"BSM" / APP_NAME;
}

static fs::path presets_path() { return app_dir() / L"presets.json"; }

typedef vector<std::pair<string, Card>> Presets;

static Presets load_presets() {
    Presets out;
    string txt;
    JV j;
    if (!read_text_file(presets_path(), txt) || !json_parse(txt, j)) return out;
    const JV* p = j.get("presets");
    if (!p || p->t != JV::OBJ) return out;
    for (auto& kv : p->o)
        if (kv.second.t == JV::OBJ) out.emplace_back(kv.first, card_from_json(kv.second));
    return out;
}

static void save_presets(const Presets& ps) {
    JV root = JV::obj(), list = JV::obj();
    root.set("version", JV::num(1));
    for (auto& p : ps) list.set(p.first, card_to_json(p.second));
    root.set("presets", list);
    write_text_file(presets_path(), json_dump(root, 2));
}

// ------------------------------------------------------------------------------------------------ small dialogs
static int g_modal = 0;                                        // dialogs and the zone picker that are open: F5 and F6 do nothing then
wstring g_auto_shot;                                         // tests: the first dialog that opens is drawn into this PNG and closed

static void shot_window(HWND h, const wstring& path) {
    RECT c;
    GetWindowRect(h, &c);
    int w = c.right - c.left, hh = c.bottom - c.top;
    HDC dc = GetDC(nullptr), mem = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, w, hh);
    HGDIOBJ old = SelectObject(mem, bm);
    PrintWindow(h, mem, 2);                                    // PW_RENDERFULLCONTENT: the child controls too
    SelectObject(mem, old);
    Bitmap bmp(bm, nullptr);
    CLSID png{};
    UINT n = 0, sz = 0;
    GetImageEncodersSize(&n, &sz);
    vector<char> buf(sz);
    ImageCodecInfo* info = (ImageCodecInfo*)buf.data();
    GetImageEncoders(n, sz, info);
    for (UINT i = 0; i < n; ++i)
        if (wcscmp(info[i].MimeType, L"image/png") == 0) png = info[i].Clsid;
    bmp.Save(path.c_str(), &png, nullptr);
    DeleteObject(bm);
    DeleteDC(mem);
    ReleaseDC(nullptr, dc);
}

struct ModalGuard {
    ModalGuard() { ++g_modal; }
    ~ModalGuard() { --g_modal; }
};

static void run_modal(HWND owner, HWND dlg, const bool& done) {
    ModalGuard guard;
    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    MSG msg;
    if (!g_auto_shot.empty()) {
        for (int i = 0; i < 10; ++i) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            Sleep(40);
        }
        shot_window(dlg, g_auto_shot);
        g_auto_shot.clear();
        EnableWindow(owner, TRUE);
        DestroyWindow(dlg);
        return;
    }
    while (!done) {
        int r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) {                                          // the program is quitting: the loop of the main window must see it too
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (r < 0) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
    SetForegroundWindow(owner);
}

static HWND make_dialog(FormWindow* fw, HWND owner, const wchar_t* title, int cw, int ch) {
    int dpi = GetDpiForWindow(owner);
    fw->form.set_dpi(dpi);
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, ex = WS_EX_DLGMODALFRAME;
    RECT rc = {0, 0, fw->form.px(cw), fw->form.px(ch)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, ex, dpi);
    RECT orc;
    GetWindowRect(owner, &orc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = orc.left + ((orc.right - orc.left) - w) / 2, y = orc.top + ((orc.bottom - orc.top) - h) / 2;
    HWND dlg = CreateWindowExW(ex, FORM_CLASS, title, style, x, y, w, h, owner, nullptr, GetModuleHandleW(nullptr), fw);
    dark_title_bar(dlg);
    return dlg;
}

static HBRUSH field_brush() {
    static HBRUSH b = CreateSolidBrush(T.field);
    return b;
}

static HWND make_edit(HWND parent, bool multiline, const RECT& r, HFONT font, const wstring& text) {
    DWORD st = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOVSCROLL | (multiline ? (ES_MULTILINE | ES_WANTRETURN) : ES_AUTOHSCROLL);
    HWND e = CreateWindowExW(0, L"EDIT", L"", st, r.left, r.top, r.right - r.left, r.bottom - r.top, parent, (HMENU)1000, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(e, WM_SETFONT, (WPARAM)font, TRUE);
    wstring t = text;
    if (multiline) {                                           // an edit control wants CR LF
        wstring o;
        for (wchar_t c : t) {
            if (c == L'\n') o += L'\r';
            o += c;
        }
        t = o;
    }
    SetWindowTextW(e, t.c_str());
    return e;
}

static wstring edit_text(HWND e) {
    int n = GetWindowTextLengthW(e);
    wstring t(n + 1, L'\0');
    GetWindowTextW(e, &t[0], n + 1);
    t.resize(n);
    wstring o;
    for (wchar_t c : t)
        if (c != L'\r') o += c;
    return o;
}

// asks for a line of text; false when cancelled
struct InputDlg : FormWindow {
    HWND edit = nullptr;
    bool done = false, ok = false;
    wstring result;
    LRESULT on_message(UINT m, WPARAM wp, LPARAM lp, bool& handled) override {
        handled = true;
        switch (m) {
            case WM_CTLCOLOREDIT: {
                HDC dc = (HDC)wp;
                SetTextColor(dc, T.text);
                SetBkColor(dc, T.field);
                return (LRESULT)field_brush();
            }
            case WM_CLOSE:
                done = true;
                return 0;
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE) done = true;
                return 0;
            case WM_COMMAND:
                if (LOWORD(wp) == IDOK) {
                    ok = true;
                    result = trim(edit_text(edit));
                    done = true;
                }
                return 0;
            case WM_SETFOCUS:
                SetFocus(edit);
                return 0;
        }
        handled = false;
        return 0;
    }
};

static bool input_text(HWND owner, const wchar_t* title, const wchar_t* prompt, const wstring& init, wstring& out) {
    InputDlg d;
    HWND dlg = make_dialog(&d, owner, title, 340, 118);
    Form& f = d.form;
    Widget* l = f.add(K_LABEL, 1, prompt);
    l->r = {f.px(12), f.px(12), f.px(328), f.px(30)};
    Widget* ok = f.add(K_BUTTON, 2, L"OK");
    ok->r = {f.px(170), f.px(78), f.px(244), f.px(104)};
    Widget* cancel = f.add(K_BUTTON, 3, L"Cancel");
    cancel->r = {f.px(252), f.px(78), f.px(328), f.px(104)};
    RECT er = {f.px(12), f.px(36), f.px(328), f.px(62)};
    d.edit = make_edit(dlg, false, {er.left + 2, er.top + 4, er.right - 2, er.bottom - 3}, f.font(), init);
    SendMessageW(d.edit, EM_SETSEL, 0, -1);
    f.add(K_CUSTOM, 4)->r = er;
    f.on_custom = [](Widget&, HDC dc, const RECT& r) {
        HBRUSH b = CreateSolidBrush(T.field);
        HPEN p = CreatePen(PS_SOLID, 1, T.border);
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, 6, 6);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(b);
        DeleteObject(p);
    };
    f.on_action = [&](Widget& w) {
        if (w.id == 2) SendMessageW(dlg, WM_COMMAND, IDOK, 0);
        else if (w.id == 3) d.done = true;
    };
    SetFocus(d.edit);
    run_modal(owner, dlg, d.done);
    out = d.result;
    return d.ok;
}

// ------------------------------------------------------------------------------------------------ the About box
static void draw_robot(Graphics& g, REAL x0, REAL y0, REAL sx, REAL sy) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.TranslateTransform(x0, y0);
    g.ScaleTransform(sx, sy);
    Color dark(255, 0x2f, 0x31, 0x35), mid(255, 0x57, 0x5a, 0x60), light(255, 0xd8, 0xda, 0xdd), red(255, 0xe5, 0x32, 0x2d), grey(255, 0x8d, 0x90, 0x96);
    SolidBrush bdark(dark), bmid(mid), blight(light), bred(red), bgrey(grey), bshadow(Color(60, 0, 0, 0));
    auto rr = [&](Brush& b, REAL x, REAL y, REAL w, REAL h, REAL r) {
        GraphicsPath p;
        p.AddArc(x, y, 2 * r, 2 * r, 180, 90);
        p.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
        p.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
        p.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
        p.CloseFigure();
        g.FillPath(&b, &p);
    };
    auto el = [&](Brush& b, REAL cx, REAL cy, REAL rx, REAL ry) { g.FillEllipse(&b, cx - rx, cy - ry, 2 * rx, 2 * ry); };
    g.FillEllipse(&bshadow, 38.f, 136.f, 80.f, 8.f);
    rr(bmid, 58, 112, 10, 22, 3);
    rr(bmid, 88, 112, 10, 22, 3);
    rr(bdark, 52, 130, 22, 8, 4);
    rr(bdark, 82, 130, 22, 8, 4);
    rr(bmid, 48, 72, 60, 44, 9);
    rr(bdark, 60, 84, 36, 18, 4);
    el(bred, 70, 93, 3.5f, 3.5f);
    g.FillRectangle(&blight, 80.f, 90.f, 12.f, 2.5f);
    g.FillRectangle(&blight, 80.f, 95.f, 8.f, 2.5f);
    Pen ant(grey, 3);
    ant.SetStartCap(LineCapRound);
    ant.SetEndCap(LineCapRound);
    g.DrawLine(&ant, 78.f, 30.f, 78.f, 16.f);
    el(bred, 78, 13, 4.5f, 4.5f);
    rr(bmid, 46, 28, 64, 42, 11);
    rr(bdark, 53, 36, 50, 24, 8);
    el(blight, 68, 48, 5.5f, 5.5f);
    el(blight, 90, 48, 5.5f, 5.5f);
    el(bdark, 69.5f, 48.5f, 2.6f, 2.6f);
    el(bdark, 91.5f, 48.5f, 2.6f, 2.6f);
    Pen arm(mid, 8);
    arm.SetStartCap(LineCapRound);
    arm.SetEndCap(LineCapRound);
    g.DrawLine(&arm, 50.f, 82.f, 38.f, 104.f);
    g.DrawLine(&arm, 106.f, 82.f, 124.f, 66.f);
    rr(bgrey, 118, 52, 40, 28, 5);
    rr(bgrey, 126, 47, 14, 7, 2);
    el(bdark, 138, 66, 11, 11);
    el(blight, 138, 66, 9, 9);
    el(bdark, 138, 66, 7, 7);
    el(bred, 138, 66, 3.6f, 3.6f);
    el(blight, 135.5f, 63.5f, 1.4f, 1.4f);
    g.FillRectangle(&blight, 148.f, 56.f, 6.f, 3.5f);
    Pen corner(red, 3);
    corner.SetStartCap(LineCapSquare);
    corner.SetEndCap(LineCapSquare);
    const REAL cs[4][4] = {{168, 18, 1, 1}, {196, 18, -1, 1}, {168, 100, 1, -1}, {196, 100, -1, -1}};
    for (auto& c : cs) {
        g.DrawLine(&corner, c[0], c[1], c[0] + 10 * c[2], c[1]);
        g.DrawLine(&corner, c[0], c[1], c[0], c[1] + 10 * c[3]);
    }
    Pen spark(light, 2.5f);
    spark.SetStartCap(LineCapRound);
    spark.SetEndCap(LineCapRound);
    g.DrawLine(&spark, 150.f, 40.f, 155.f, 33.f);
    g.DrawLine(&spark, 160.f, 46.f, 167.f, 42.f);
    g.ResetTransform();
}

struct AboutDlg : FormWindow {
    bool done = false;
    LRESULT on_message(UINT m, WPARAM wp, LPARAM, bool& handled) override {
        handled = true;
        if (m == WM_CLOSE) done = true;
        else if (m == WM_KEYDOWN && (wp == VK_ESCAPE || wp == VK_RETURN)) done = true;
        else handled = false;
        return 0;
    }
};

static void about_box(HWND owner) {
    AboutDlg d;
    HWND dlg = make_dialog(&d, owner, L"About Grabber", 520, 226);
    Form& f = d.form;
    auto lab = [&](int id, const wstring& t, int y, bool dim = false) {
        Widget* w = f.add(K_LABEL, id, t);
        w->r = {f.px(228), f.px(y), f.px(508), f.px(y + 20)};
        w->dim = dim;
        return w;
    };
    lab(1, wstring(APP_NAME) + L" " + APP_VERSION, 18);
    lab(2, L"Screen video capture for Windows.", 44);
    lab(3, L"A free tool by BSM.", 64);
    lab(4, L"By Benoit (BSM) Saint-Moulin", 94);
    Widget* link = f.add(K_BUTTON, 5, L"www.bsm3d.com");
    link->r = {f.px(228), f.px(116), f.px(398), f.px(142)};
    lab(6, L"© 2026 Benoit Saint-Moulin", 150, true);
    lab(7, L"Uses ffmpeg. The licences are in the program folder.", 172, true);
    Widget* ok = f.add(K_BUTTON, 8, L"OK");
    ok->r = {f.px(428), f.px(192), f.px(508), f.px(218)};
    f.add(K_CUSTOM, 9)->r = {f.px(12), f.px(24), f.px(212), f.px(174)};
    f.on_custom = [&f](Widget&, HDC dc, const RECT& r) {
        Graphics g(dc);
        draw_robot(g, (REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left) / 200.f, (REAL)(r.bottom - r.top) / 150.f);
    };
    f.on_action = [&](Widget& w) {
        if (w.id == 5) ShellExecuteW(nullptr, L"open", APP_SITE, nullptr, nullptr, SW_SHOWNORMAL);
        else if (w.id == 8) d.done = true;
    };
    SetFocus(dlg);
    run_modal(owner, dlg, d.done);
}

// ------------------------------------------------------------------------------------------------ the card editor
struct CardDlg : FormWindow {
    Card cfg;
    int vw = 1920, vh = 1080, pw = 520, ph = 292;
    HWND edit = nullptr;
    bool done = false, ok = false;
    Presets presets;
    std::function<void(const Presets&)> on_presets;
    vector<wstring> fonts;
    Bitmap* preview = nullptr;
    bool loading = false;
    ~CardDlg() { delete preview; }

    void refresh_presets(const string& select = "") {
        Widget* c = form.get(10);
        c->items.clear();
        c->items.push_back(L"Presets...");
        c->sel = 0;
        vector<std::pair<string, Card>> sorted = presets;
        std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return _stricmp(a.first.c_str(), b.first.c_str()) < 0; });
        names.clear();
        names.push_back("");
        for (auto& p : sorted) {
            c->items.push_back(W(p.first));
            names.push_back(p.first);
            if (p.first == select) c->sel = (int)c->items.size() - 1;
        }
        form.get(12)->enabled = c->sel > 0;
        form.redraw();
    }
    vector<string> names;

    void controls_from_cfg() {
        loading = true;
        SetWindowTextW(edit, [&] {
            wstring o;
            for (wchar_t c : W(cfg.text)) {
                if (c == L'\n') o += L'\r';
                o += c;
            }
            return o;
        }().c_str());
        Widget* fc = form.get(20);
        wstring fam = W(cfg.font);
        int idx = -1;
        for (size_t i = 0; i < fc->items.size(); ++i)
            if (_wcsicmp(fc->items[i].c_str(), fam.c_str()) == 0) idx = (int)i;
        if (idx < 0) {
            fc->items.insert(fc->items.begin(), fam);
            idx = 0;
        }
        fc->sel = idx;
        form.get(21)->val = cfg.size;
        form.get(22)->checked = cfg.bold;
        form.get(23)->checked = cfg.italic;
        form.get(24)->checked = cfg.shadow;
        form.get(25)->sel = cfg.align == "left" ? 0 : cfg.align == "right" ? 2 : 1;
        form.get(33)->sel = cfg.fit == "fill" ? 1 : 0;
        form.get(36)->val = cfg.x;
        form.get(38)->val = cfg.y;
        loading = false;
        changed();
    }

    void changed() {
        if (loading) return;
        cfg.text = U(edit_text(edit));
        cfg.font = U(form.get(20)->item_text());
        cfg.size = form.get(21)->val;
        cfg.bold = form.get(22)->checked;
        cfg.italic = form.get(23)->checked;
        cfg.shadow = form.get(24)->checked;
        int a = form.get(25)->sel;
        cfg.align = a == 0 ? "left" : a == 2 ? "right" : "center";
        cfg.fit = form.get(33)->sel == 1 ? "fill" : "fit";
        cfg.x = form.get(36)->val;
        cfg.y = form.get(38)->val;
        wstring base = fs::path(W(cfg.img)).filename().wstring();
        Widget* bi = form.get(30);
        bi->text = cfg.img.empty() ? L"Image..." : base.size() < 20 ? base : base.substr(0, 17) + L"...";
        form.get(31)->visible = !cfg.img.empty();
        form.get(26)->swatch = hex_color(cfg.color);
        form.get(27)->swatch = hex_color(cfg.bg);
        delete preview;
        preview = (Bitmap*)render_card(pw, ph, cfg);
        form.redraw();
    }

    LRESULT on_message(UINT m, WPARAM wp, LPARAM lp, bool& handled) override {
        handled = true;
        switch (m) {
            case WM_CTLCOLOREDIT: {
                HDC dc = (HDC)wp;
                SetTextColor(dc, T.text);
                SetBkColor(dc, T.field);
                return (LRESULT)field_brush();
            }
            case WM_COMMAND:
                if (HIWORD(wp) == EN_CHANGE && (HWND)lp == edit) changed();
                return 0;
            case WM_CLOSE:
                done = true;
                return 0;
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE) done = true;
                return 0;
            case WM_SETFOCUS:
                return 0;
        }
        handled = false;
        return 0;
    }
};

static bool card_dialog(HWND owner, const wchar_t* title, Card& card, int vsw, int vsh, Presets& presets, const std::function<void(const Presets&)>& on_presets) {
    CardDlg d;
    d.cfg = card;
    double k = std::min(1.0, 1080.0 / vsh);
    d.vw = std::max(2, (int)(vsw * k));
    d.vh = std::max(2, (int)(vsh * k));
    d.pw = 520;
    d.ph = std::max(60, std::min(360, (int)(d.pw * d.vh / (double)d.vw)));
    d.pw = (int)(d.ph * d.vw / (double)d.vh);
    d.presets = presets;
    d.on_presets = on_presets;
    d.fonts = font_families();
    int cw = 404 + d.pw + 12, ch = std::max(398, 12 + d.ph + 6 + 22 + 56);
    HWND dlg = make_dialog(&d, owner, title, cw, ch);
    Form& f = d.form;
    auto R = [&](int x, int y, int w, int h) { return RECT{f.px(x), f.px(y), f.px(x + w), f.px(y + h)}; };
    auto combo = [&](int id, RECT r) {
        Widget* w = f.add(K_COMBO, id);
        w->r = r;
        return w;
    };
    auto button = [&](int id, const wchar_t* t, RECT r) {
        Widget* w = f.add(K_BUTTON, id, t);
        w->r = r;
        return w;
    };
    auto check = [&](int id, const wchar_t* t, RECT r) {
        Widget* w = f.add(K_CHECK, id, t);
        w->r = r;
        return w;
    };
    int y = 12;
    combo(10, R(12, y, 196, 26));
    button(11, L"Save as...", R(216, y, 86, 26));
    button(12, L"Delete", R(310, y, 82, 26));
    y += 34;
    RECT er = R(12, y, 380, 76);
    f.add(K_CUSTOM, 13)->r = er;
    d.edit = make_edit(dlg, true, {er.left + 3, er.top + 4, er.right - 3, er.bottom - 3}, f.font(), W(d.cfg.text));
    y += 84;
    Widget* fc = combo(20, R(12, y, 296, 26));
    fc->items = d.fonts;
    Widget* sz = f.add(K_SPIN, 21);
    sz->r = R(316, y, 76, 26);
    sz->minv = 2;
    sz->maxv = 40;
    sz->suffix = L" %";
    y += 34;
    check(22, L"Bold", R(12, y + 2, 58, 22));
    check(23, L"Italic", R(74, y + 2, 62, 22));
    check(24, L"Shadow", R(140, y + 2, 76, 22));
    Widget* al = combo(25, R(224, y, 168, 26));
    al->items = {L"Left", L"Centre", L"Right"};
    y += 34;
    button(26, L"Text colour", R(12, y, 186, 26));
    button(27, L"Background", R(206, y, 186, 26));
    y += 34;
    button(30, L"Image...", R(12, y, 200, 26));
    button(31, L"x", R(220, y, 26, 26));
    Widget* fit = combo(33, R(254, y, 138, 26));
    fit->items = {L"Image: whole", L"Image: fills the picture"};
    y += 34;
    f.add(K_LABEL, 34, L"Place, left to right")->r = R(12, y, 380, 18);
    y += 18;
    Widget* sx = f.add(K_SLIDER, 36);
    sx->r = R(12, y, 380, 24);
    y += 28;
    f.add(K_LABEL, 37, L"Place, top to bottom")->r = R(12, y, 380, 18);
    y += 18;
    Widget* sy = f.add(K_SLIDER, 38);
    sy->r = R(12, y, 380, 24);
    f.add(K_CUSTOM, 40)->r = R(404, 12, d.pw, d.ph);
    f.add(K_LABEL, 41, fmt_w("The picture as it will be in the video (%d x %d).", d.vw, d.vh))->r = R(404, 12 + d.ph + 6, d.pw, 20);
    f.get(41)->dim = true;
    button(50, L"OK", R(404 + d.pw - 170, ch - 38, 80, 26));
    button(51, L"Cancel", R(404 + d.pw - 82, ch - 38, 80, 26));
    f.on_custom = [&d, &f](Widget& w, HDC dc, const RECT& r) {
        if (w.id == 13) {
            HBRUSH b = CreateSolidBrush(T.field);
            HPEN p = CreatePen(PS_SOLID, 1, T.border);
            HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
            RoundRect(dc, r.left, r.top, r.right, r.bottom, 6, 6);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(b);
            DeleteObject(p);
        } else if (w.id == 40 && d.preview) {
            Graphics g(dc);
            g.DrawImage(d.preview, (INT)r.left, (INT)r.top, (INT)(r.right - r.left), (INT)(r.bottom - r.top));
            HBRUSH nb = CreateSolidBrush(T.btn_border);
            FrameRect(dc, &r, nb);
            DeleteObject(nb);
        }
        (void)f;
    };
    f.on_action = [&](Widget& w) {
        switch (w.id) {
            case 10: {                                         // a preset was chosen
                int i = w.sel;
                f.get(12)->enabled = i > 0;
                if (i > 0 && i < (int)d.names.size())
                    for (auto& p : d.presets)
                        if (p.first == d.names[i]) {
                            d.cfg = p.second;
                            d.controls_from_cfg();
                        }
                break;
            }
            case 11: {
                wstring name;
                int cur = f.get(10)->sel;
                if (!input_text(dlg, L"Preset", L"Name of the preset:", cur > 0 ? W(d.names[cur]) : L"", name) || name.empty()) break;
                string n = U(name);
                auto it = std::find_if(d.presets.begin(), d.presets.end(), [&](auto& p) { return p.first == n; });
                if (it != d.presets.end()) {
                    if (MessageBoxW(dlg, (L"A preset named \"" + name + L"\" exists. Replace it?").c_str(), L"Preset", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
                    it->second = d.cfg;
                } else {
                    d.presets.emplace_back(n, d.cfg);
                }
                if (d.on_presets) d.on_presets(d.presets);
                d.refresh_presets(n);
                break;
            }
            case 12: {
                int cur = f.get(10)->sel;
                if (cur <= 0 || cur >= (int)d.names.size()) break;
                string n = d.names[cur];
                if (MessageBoxW(dlg, (L"Delete the preset \"" + W(n) + L"\"?").c_str(), L"Preset", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
                d.presets.erase(std::remove_if(d.presets.begin(), d.presets.end(), [&](auto& p) { return p.first == n; }), d.presets.end());
                if (d.on_presets) d.on_presets(d.presets);
                d.refresh_presets();
                break;
            }
            case 26:
            case 27: {
                static COLORREF custom[16];
                CHOOSECOLORW cc{sizeof cc};
                cc.hwndOwner = dlg;
                cc.rgbResult = hex_color(w.id == 26 ? d.cfg.color : d.cfg.bg);
                cc.lpCustColors = custom;
                cc.Flags = CC_RGBINIT | CC_FULLOPEN;
                if (ChooseColorW(&cc)) {
                    (w.id == 26 ? d.cfg.color : d.cfg.bg) = color_hex(cc.rgbResult);
                    f.get(26)->swatch = hex_color(d.cfg.color);
                    f.get(27)->swatch = hex_color(d.cfg.bg);
                    d.changed();
                }
                break;
            }
            case 30: {
                wchar_t file[MAX_PATH * 2] = L"";
                OPENFILENAMEW ofn{sizeof ofn};
                ofn.hwndOwner = dlg;
                ofn.lpstrFilter = L"Images (*.png *.jpg *.jpeg *.bmp *.webp)\0*.png;*.jpg;*.jpeg;*.bmp;*.webp\0All files\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = (DWORD)std::size(file);
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                if (GetOpenFileNameW(&ofn)) {
                    d.cfg.img = U(file);
                    d.changed();
                }
                break;
            }
            case 31:
                d.cfg.img.clear();
                d.changed();
                break;
            case 50:
                d.ok = d.done = true;
                break;
            case 51:
                d.done = true;
                break;
            default:
                d.changed();
        }
    };
    d.refresh_presets();
    d.controls_from_cfg();
    SetFocus(dlg);
    run_modal(owner, dlg, d.done);
    if (d.ok) card = d.cfg;
    presets = d.presets;
    return d.ok;
}

// ------------------------------------------------------------------------------------------------ the click circles
struct Ripple {
    HWND hwnd = nullptr;
    double t0 = 0;
    COLORREF col = 0;
};
static const int RIPPLE_SIZE = 110;
static const double RIPPLE_LIFE = 0.5;

static void ripple_draw(HWND h, Ripple* r) {
    double u = (now() - r->t0) / RIPPLE_LIFE;
    if (u >= 1) {
        KillTimer(h, 1);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
        delete r;                                              // the window is gone: nothing uses the data any more
        return;
    }
    HDC scr = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(scr);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), RIPPLE_SIZE, -RIPPLE_SIZE, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bm);
    memset(bits, 0, RIPPLE_SIZE * RIPPLE_SIZE * 4);
    {
        Bitmap bmp(RIPPLE_SIZE, RIPPLE_SIZE, RIPPLE_SIZE * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        double ease = 1 - (1 - u) * (1 - u), rad = 14 + 36 * ease;
        int a = (int)(255 * (1 - u));
        SolidBrush fillb(Color((BYTE)(a * 0.25), GetRValue(r->col), GetGValue(r->col), GetBValue(r->col)));
        Pen ring(Color((BYTE)a, GetRValue(r->col), GetGValue(r->col), GetBValue(r->col)), 5.f);
        REAL c = RIPPLE_SIZE / 2.f;
        g.FillEllipse(&fillb, c - (REAL)rad, c - (REAL)rad, (REAL)(2 * rad), (REAL)(2 * rad));
        g.DrawEllipse(&ring, c - (REAL)rad, c - (REAL)rad, (REAL)(2 * rad), (REAL)(2 * rad));
    }
    POINT src = {0, 0};
    SIZE sz = {RIPPLE_SIZE, RIPPLE_SIZE};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(h, scr, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(bm);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
}

static LRESULT CALLBACK ripple_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Ripple* r = (Ripple*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            return TRUE;
        case WM_TIMER:
            if (r) ripple_draw(h, r);
            return 0;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCDESTROY:
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void spawn_ripple(int x, int y, bool right) {
    Ripple* r = new Ripple();
    r->t0 = now();
    r->col = right ? RGB(90, 200, 255) : RGB(255, 205, 40);
    r->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, RIPPLE_CLASS, L"", WS_POPUP,
                              x - RIPPLE_SIZE / 2, y - RIPPLE_SIZE / 2, RIPPLE_SIZE, RIPPLE_SIZE, nullptr, nullptr, GetModuleHandleW(nullptr), r);
    if (!r->hwnd) {
        delete r;
        return;
    }
    ripple_draw(r->hwnd, r);
    ShowWindow(r->hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(r->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(r->hwnd, 1, 10, nullptr);
}

// ------------------------------------------------------------------------------------------------ the zone picker
struct Picker {
    HWND hwnd = nullptr, notify = nullptr;
    int ox = 0, oy = 0, w = 0, h = 0;
    bool dragging = false, answered = false;
    POINT a{}, b{};                                            // physical pixels of the screen
};

static void picker_draw(Picker* p) {
    HDC scr = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(scr);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), p->w, -p->h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bm);
    {
        Bitmap bmp(p->w, p->h, p->w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.SetCompositingMode(CompositingModeSourceCopy);
        g.Clear(Color(110, 0, 0, 0));
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        FontFamily fam(L"Segoe UI");
        if (p->dragging) {
            int x = std::min(p->a.x, p->b.x) - p->ox, y = std::min(p->a.y, p->b.y) - p->oy;
            int rw = abs(p->b.x - p->a.x), rh = abs(p->b.y - p->a.y);
            SolidBrush clear(Color(1, 0, 0, 0));                // nearly invisible, but not transparent to the mouse
            g.FillRectangle(&clear, x, y, rw, rh);
            g.SetCompositingMode(CompositingModeSourceOver);
            Pen red(Color(255, 0xe5, 0x32, 0x2d), 2.f);
            g.DrawRectangle(&red, x, y, rw, rh);
            Font font(&fam, 13.f, FontStyleBold, UnitPixel);
            SolidBrush white(Color(255, 255, 255, 255));
            wstring t = fmt_w("%d x %d", rw, rh);
            g.DrawString(t.c_str(), -1, &font, PointF((REAL)x + 6, (REAL)std::max(0, y - 22)), &white);
        } else {
            g.SetCompositingMode(CompositingModeSourceOver);
            Font font(&fam, 20.f, FontStyleRegular, UnitPixel);
            SolidBrush white(Color(255, 255, 255, 255));
            StringFormat sf;
            sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            wstring t = L"Drag to choose the zone to record.   Esc to cancel.";
            g.DrawString(t.c_str(), -1, &font, RectF(0, 0, (REAL)p->w, (REAL)p->h), &sf, &white);
        }
    }
    POINT src = {0, 0}, dst = {p->ox, p->oy};
    SIZE sz = {p->w, p->h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(p->hwnd, scr, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(bm);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
}

static LRESULT CALLBACK picker_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Picker* p = (Picker*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            return TRUE;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        case WM_LBUTTONDOWN:
            p->dragging = true;
            p->a = p->b = cursor_pos();
            SetCapture(h);
            picker_draw(p);
            return 0;
        case WM_MOUSEMOVE:
            if (p->dragging) {
                p->b = cursor_pos();
                picker_draw(p);
            }
            return 0;
        case WM_LBUTTONUP:
            if (p->dragging) {
                p->b = cursor_pos();
                ReleaseCapture();
                int x = std::min(p->a.x, p->b.x), y = std::min(p->a.y, p->b.y), w = abs(p->b.x - p->a.x), hh = abs(p->b.y - p->a.y);
                HWND notify = p->notify;
                p->answered = true;
                DestroyWindow(h);
                if (w >= 16 && hh >= 16) {
                    int* v = new int[4]{x, y, w, hh};
                    PostMessageW(notify, WM_APP_ZONE, 1, (LPARAM)v);
                } else {
                    PostMessageW(notify, WM_APP_ZONE, 0, 0);
                }
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(h);                 // the answer "nothing" is given when the window is gone
            return 0;
        case WM_RBUTTONDOWN:
            DestroyWindow(h);
            return 0;
        case WM_NCDESTROY:
            if (p) {
                --g_modal;
                if (!p->answered) PostMessageW(p->notify, WM_APP_ZONE, 0, 0);           // closed in any other way: the main window must still come back
            }
            delete p;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void open_picker(HWND notify) {
    ++g_modal;
    Picker* p = new Picker();
    p->notify = notify;
    p->ox = GetSystemMetrics(SM_XVIRTUALSCREEN);
    p->oy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    p->w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    p->h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    p->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, PICKER_CLASS, L"", WS_POPUP, p->ox, p->oy, p->w, p->h, nullptr, nullptr,
                              GetModuleHandleW(nullptr), p);
    picker_draw(p);
    ShowWindow(p->hwnd, SW_SHOW);
    SetForegroundWindow(p->hwnd);
    SetFocus(p->hwnd);
}

// ------------------------------------------------------------------------------------------------ the main window
enum {
    ID_R_ZONE = 1, ID_R_SCREEN, ID_R_WINDOW, ID_R_APPS, ID_ZONE_BTN, ID_ZONE_LBL, ID_SCREEN, ID_WINDOW, ID_REFRESH, ID_APPS, ID_QUALITY, ID_SOUND,
    ID_CURSOR, ID_CLICKS, ID_MIC_K, ID_MIC, ID_CAM_K, ID_CORNER, ID_CAM, ID_H_WHAT, ID_H_OPT, ID_H_CARDS, ID_INTRO_K, ID_INTRO_EDIT, ID_INTRO_LBL,
    ID_INTRO_SECS, ID_OUTRO_K, ID_OUTRO_EDIT, ID_OUTRO_LBL, ID_OUTRO_SECS, ID_DIR_LBL, ID_DIR_BTN, ID_LINE, ID_REC, ID_STOP, ID_HIDE, ID_STATUS, ID_PLAY,
    ID_FOLDER, ID_ABOUT, ID_KEYS
};

struct MainWindow : FormWindow {
    Ini ini;
    wstring ffmpeg;
    std::unique_ptr<Recording> rec, pending;
    std::atomic<bool> busy{false};
    bool quit_after = false, have_zone = false, apps_loaded = false, shown_once = false, starting = false, stop_pending = false;
    wstring note;                                              // added to the status after saving (ffmpeg stopped by itself...)
    R4 zone;
    wstring last, out_dir, done_path, done_err;
    double done_secs = 0;
    vector<WinInfo> wins;
    Card cards[2];
    Presets presets;
    bool click_watch = false, prev_down[2] = {false, false};
    bool testing = false;

    Form& F() { return form; }
    Widget* w(int id) { return form.get(id); }
    string mode() {
        if (w(ID_R_SCREEN)->checked) return "screen";
        if (w(ID_R_WINDOW)->checked) return "window";
        if (w(ID_R_APPS)->checked) return "apps";
        return "zone";
    }

    // ---- building
    void build();
    void layout();
    void update();
    void refresh_windows();
    Target target();
    void mode_changed();
    void set_status(const wstring& text, const wstring& red = L"");
    void start_recording();
    void started(bool ok);
    void tick();
    void stop_recording();
    void saved();
    void toggle_hidden();
    void exclude();
    void hotkey(int id);
    void register_keys(bool on);
    void edit_card(int i);
    void load_settings();
    void save_settings();
    void video_size(int& vw, int& vh);
    bool on_close();
    void action(Widget& wd);
    LRESULT on_message(UINT m, WPARAM wp, LPARAM lp, bool& handled) override;
};

void MainWindow::set_status(const wstring& text, const wstring& red) {
    Widget* s = w(ID_STATUS);
    bool grow = s->text != text;
    s->text = text;
    s->red = red;
    if (grow) layout();
    else form.redraw();
}

void MainWindow::build() {
    Form& f = form;
    f.on_action = [this](Widget& wd) { action(wd); };
    auto radio = [&](int id, const wchar_t* t) {
        Widget* x = f.add(K_RADIO, id, t);
        x->group = 1;
        return x;
    };
    f.add(K_HEADER, ID_H_WHAT, L"What to record");
    radio(ID_R_ZONE, L"Zone");
    radio(ID_R_SCREEN, L"Screen");
    radio(ID_R_WINDOW, L"Window");
    radio(ID_R_APPS, L"Applications");
    f.add(K_BUTTON, ID_ZONE_BTN, L"Choose the zone...");
    f.add(K_LABEL, ID_ZONE_LBL, L"no zone yet");
    Widget* sc = f.add(K_COMBO, ID_SCREEN);
    for (auto& s : screens()) sc->items.push_back(s.name);
    sc->sel = sc->items.empty() ? -1 : 0;
    f.add(K_COMBO, ID_WINDOW);
    f.add(K_LIST, ID_APPS);
    f.add(K_BUTTON, ID_REFRESH, L"Refresh");
    f.add(K_HEADER, ID_H_OPT, L"Options");
    Widget* q = f.add(K_COMBO, ID_QUALITY);
    for (auto& p : PROFILES) q->items.push_back(p.label);
    f.add(K_CHECK, ID_SOUND, L"Sound of the PC");
    f.add(K_CHECK, ID_CURSOR, L"Mouse cursor");
    f.add(K_CHECK, ID_CLICKS, L"Show the clicks (circle)");
    f.add(K_CHECK, ID_MIC_K, L"Microphone");
    Widget* mic = f.add(K_COMBO, ID_MIC);
    for (auto& n : microphones()) mic->items.push_back(n);
    if (mic->items.empty()) {
        mic->items.push_back(L"no microphone found");
        mic->enabled = false;
        w(ID_MIC_K)->enabled = false;
    }
    mic->sel = 0;
    f.add(K_CHECK, ID_CAM_K, L"Webcam");
    Widget* co = f.add(K_COMBO, ID_CORNER);
    co->items = {L"bottom left", L"bottom right", L"top left", L"top right"};
    Widget* cam = f.add(K_COMBO, ID_CAM);
    if (!ffmpeg.empty())
        for (auto& n : webcams(ffmpeg)) cam->items.push_back(n);
    if (cam->items.empty()) {
        cam->items.push_back(L"no camera found");
        cam->enabled = false;
        w(ID_CAM_K)->enabled = false;
    }
    cam->sel = 0;
    f.add(K_HEADER, ID_H_CARDS, L"Intro and outro (before and after the recording)");
    const int ks[2] = {ID_INTRO_K, ID_OUTRO_K};
    for (int i = 0; i < 2; ++i) {
        f.add(K_CHECK, ks[i], i == 0 ? L"Intro" : L"Outro");
        f.add(K_BUTTON, ks[i] + 1, L"Edit the card...");
        f.add(K_LABEL, ks[i] + 2)->dim = true;
        Widget* sp = f.add(K_SPIN, ks[i] + 3);
        sp->minv = 1;
        sp->maxv = 15;
        sp->val = 3;
        sp->suffix = L" s";
    }
    f.add(K_LABEL, ID_DIR_LBL);
    f.add(K_BUTTON, ID_DIR_BTN, L"Folder...");
    f.add(K_LINE, ID_LINE);
    f.add(K_BUTTON, ID_REC, L"Record   F5");
    f.add(K_BUTTON, ID_STOP, L"Stop and save   F6");
    f.add(K_BUTTON, ID_HIDE, L"Hide   F11");
    f.add(K_STATUS, ID_STATUS);
    f.add(K_BUTTON, ID_PLAY, L"Play");
    f.add(K_BUTTON, ID_FOLDER, L"Show in folder");
    f.add(K_BUTTON, ID_ABOUT, L"About");
    f.add(K_CHECK, ID_KEYS, L"F5, F6 and F11 work from any program");
}

void MainWindow::layout() {
    Form& f = form;
    const int M = 12, IW = 496;
    string md = mode();
    auto place = [&](int id, int x, int y, int ww, int hh, bool vis = true) {
        Widget* x_ = w(id);
        x_->r = {f.px(x), f.px(y), f.px(x + ww), f.px(y + hh)};
        x_->visible = vis;
    };
    int y = M;
    place(ID_H_WHAT, M, y, IW, 16);
    y += 22;
    const int rids[4] = {ID_R_ZONE, ID_R_SCREEN, ID_R_WINDOW, ID_R_APPS};
    for (int i = 0; i < 4; ++i) place(rids[i], M + i * (IW / 4), y, IW / 4 - 4, 22);
    y += 30;
    bool zone = md == "zone", screen = md == "screen", window = md == "window", apps = md == "apps";
    place(ID_ZONE_BTN, M, y, 190, 28, zone);
    place(ID_ZONE_LBL, M + 200, y, IW - 200, 28, zone);
    place(ID_SCREEN, M, y, IW, 26, screen);
    place(ID_WINDOW, M, y, IW - 90, 26, window);
    place(ID_REFRESH, M + IW - 82, y, 82, 26, window || apps);
    if (apps) {
        place(ID_APPS, M, y + 32, IW, 120, true);
        y += 32 + 120;
    } else {
        place(ID_APPS, M, y, IW, 120, false);
        y += zone ? 28 : 26;
    }
    y += 14;
    place(ID_H_OPT, M, y, IW, 16);
    y += 22;
    place(ID_QUALITY, M, y, IW, 26);
    y += 34;
    place(ID_SOUND, M, y, 140, 22);
    place(ID_CURSOR, M + 150, y, 130, 22);
    place(ID_CLICKS, M + 290, y, 206, 22);
    y += 30;
    place(ID_MIC_K, M, y, 110, 22);
    place(ID_MIC, M + 120, y, IW - 120, 26);
    y += 34;
    place(ID_CAM_K, M, y, 80, 22);
    place(ID_CORNER, M + 90, y, 120, 26);
    place(ID_CAM, M + 218, y, IW - 218, 26);
    y += 38;
    place(ID_H_CARDS, M, y, IW, 16);
    y += 22;
    const int ks[2] = {ID_INTRO_K, ID_OUTRO_K};
    for (int i = 0; i < 2; ++i) {
        place(ks[i], M, y, 66, 22);
        place(ks[i] + 1, M + 74, y - 2, 130, 26);
        place(ks[i] + 2, M + 214, y - 2, IW - 214 - 76, 26);
        place(ks[i] + 3, M + IW - 70, y - 2, 70, 26);
        y += 32;
    }
    place(ID_DIR_LBL, M, y, IW - 100, 26);
    place(ID_DIR_BTN, M + IW - 90, y, 90, 26);
    y += 36;
    place(ID_LINE, M, y, IW, 8);
    y += 14;
    int bw = (IW - 16) / 3;
    place(ID_REC, M, y, bw, 32);
    place(ID_STOP, M + bw + 8, y, bw, 32);
    place(ID_HIDE, M + 2 * (bw + 8), y, bw, 32);
    y += 42;
    // the status: as high as its text needs (two lines at least)
    {
        Widget* s = w(ID_STATUS);
        HDC dc = GetDC(form.hwnd);
        HGDIOBJ old = SelectObject(dc, form.font());
        RECT m = {0, 0, f.px(IW) - (s->red.empty() ? 0 : f.px(48)), 0};
        wstring t = s->text.empty() ? L" " : s->text;
        DrawTextW(dc, t.c_str(), (int)t.size(), &m, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(form.hwnd, dc);
        int hh = std::max(36, (int)(m.bottom * 96 / f.dpi) + 4);
        place(ID_STATUS, M, y, IW, hh);
        y += hh + 6;
    }
    bool has_last = !last.empty();
    place(ID_PLAY, M, y, 70, 26, has_last);
    place(ID_FOLDER, M + 78, y, 120, 26, has_last);
    place(ID_ABOUT, M + IW - 80, y, 80, 26);
    y += 34;
    place(ID_KEYS, M, y, IW, 22);
    y += 22 + M;
    // the window takes the height of its content
    RECT rc = {0, 0, f.px(M * 2 + IW), f.px(y)};
    DWORD style = (DWORD)GetWindowLongPtrW(form.hwnd, GWL_STYLE), ex = (DWORD)GetWindowLongPtrW(form.hwnd, GWL_EXSTYLE);
    AdjustWindowRectExForDpi(&rc, style, FALSE, ex, f.dpi);
    RECT cur;
    GetWindowRect(form.hwnd, &cur);
    if (cur.bottom - cur.top != rc.bottom - rc.top || cur.right - cur.left != rc.right - rc.left)
        SetWindowPos(form.hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    form.redraw();
}

void MainWindow::video_size(int& vw, int& vh) {
    Target t = target();
    vw = 1920;
    vh = 1080;
    if (t.kind == Target::NONE) return;
    if (t.kind == Target::WINDOW) {
        R4 r = window_rect(t.hwnd);
        vw = std::max(2, r.w);
        vh = std::max(2, r.h);
    } else if (t.kind == Target::APPS) {
        R4 big;
        for (auto& a : t.apps) {
            R4 r = window_rect(a.hwnd);
            if (r.w * r.h > big.w * big.h) big = r;
        }
        vw = std::max(2, big.w);
        vh = std::max(2, big.h);
    } else {
        vw = std::max(2, t.r.w);
        vh = std::max(2, t.r.h);
    }
}

void MainWindow::refresh_windows() {
    Widget* win = w(ID_WINDOW);
    Widget* lst = w(ID_APPS);
    std::set<wstring> ticked;
    if (!lst->items.empty() || apps_loaded) {
        for (size_t i = 0; i < wins.size() && i < lst->checks.size(); ++i)
            if (lst->checks[i]) ticked.insert(wins[i].title);
    } else {
        for (auto& l : split_lines(ini.get("apps"))) ticked.insert(l);
    }
    apps_loaded = true;
    wstring cur_win = win->sel >= 0 && win->sel < (int)wins.size() ? wins[win->sel].title : L"";
    wins = windows(form.hwnd);
    win->items.clear();
    lst->items.clear();
    lst->checks.clear();
    win->sel = wins.empty() ? -1 : 0;
    for (size_t i = 0; i < wins.size(); ++i) {
        wstring label = wins[i].title.size() < 60 ? wins[i].title : wins[i].title.substr(0, 57) + L"...";
        if (is_minimized(wins[i].hwnd)) label += L"   (minimized)";
        win->items.push_back(label);
        lst->items.push_back(label);
        lst->checks.push_back(ticked.count(wins[i].title) ? 1 : 0);
        if (!cur_win.empty() && wins[i].title == cur_win) win->sel = (int)i;
    }
    lst->top = 0;
    lst->sel = -1;
    update();
}

Target MainWindow::target() {
    Target t;
    string md = mode();
    if (md == "zone") {
        if (have_zone) {
            t.kind = Target::REGION;
            t.r = zone;
        }
    } else if (md == "screen") {
        auto sc = screens();
        int i = w(ID_SCREEN)->sel;
        if (i >= 0 && i < (int)sc.size()) {
            t.kind = Target::SCREEN;
            t.r = {sc[i].x, sc[i].y, sc[i].w, sc[i].h};
        }
    } else if (md == "apps") {
        Widget* l = w(ID_APPS);
        for (size_t i = 0; i < wins.size() && i < l->checks.size(); ++i)
            if (l->checks[i]) t.apps.push_back(wins[i]);
        if (!t.apps.empty()) t.kind = Target::APPS;
    } else {
        int i = w(ID_WINDOW)->sel;
        if (i >= 0 && i < (int)wins.size()) {
            t.kind = Target::WINDOW;
            t.hwnd = wins[i].hwnd;
            t.title = wins[i].title;
        }
    }
    return t;
}

void MainWindow::mode_changed() {
    layout();
    update();
}

void MainWindow::update() {
    bool recording = rec != nullptr, b = busy;
    w(ID_ZONE_LBL)->text = have_zone ? fmt_w("%d x %d  at %d, %d", zone.w, zone.h, zone.x, zone.y) : L"no zone yet";
    w(ID_REC)->enabled = !ffmpeg.empty() && !recording && !b && target().kind != Target::NONE;
    w(ID_STOP)->enabled = recording && !b;
    bool free = !recording && !b;
    for (int id : {ID_R_ZONE, ID_R_SCREEN, ID_R_WINDOW, ID_R_APPS, ID_ZONE_BTN, ID_SCREEN, ID_WINDOW, ID_REFRESH, ID_APPS, ID_SOUND, ID_CURSOR, ID_CLICKS,
                   ID_QUALITY, ID_INTRO_K, ID_INTRO_EDIT, ID_INTRO_SECS, ID_OUTRO_K, ID_OUTRO_EDIT, ID_OUTRO_SECS, ID_DIR_BTN})
        w(id)->enabled = free;
    bool has_mic = w(ID_MIC)->items.empty() || w(ID_MIC)->item_text() != L"no microphone found";
    w(ID_MIC_K)->enabled = free && has_mic;
    w(ID_MIC)->enabled = free && has_mic;
    bool has_cam = w(ID_CAM)->items.empty() || w(ID_CAM)->item_text() != L"no camera found";
    w(ID_CAM_K)->enabled = free && has_cam;
    w(ID_CAM)->enabled = free && has_cam;
    w(ID_CORNER)->enabled = free;
    w(ID_ABOUT)->enabled = free;                               // a box open while the video is saved would hold the closing of the program
    w(ID_PLAY)->visible = !last.empty();
    w(ID_FOLDER)->visible = !last.empty();
    form.redraw();
}

void MainWindow::exclude() { exclude_from_capture(form.hwnd, true); }

void MainWindow::toggle_hidden() {
    if (!w(ID_KEYS)->checked) {                                // the keys work only in the window: hiding it would lose it, it is minimised
        ShowWindow(form.hwnd, IsIconic(form.hwnd) ? SW_RESTORE : SW_MINIMIZE);
        return;
    }
    if (IsWindowVisible(form.hwnd)) {
        ShowWindow(form.hwnd, SW_HIDE);
    } else {
        ShowWindow(form.hwnd, SW_SHOW);
        SetForegroundWindow(form.hwnd);
        exclude();
    }
}

void MainWindow::register_keys(bool on) {
    wstring failed;
    for (int i = 1; i <= 3; ++i) {
        UnregisterHotKey(form.hwnd, i);
        if (on && !RegisterHotKey(form.hwnd, i, MOD_NOREPEAT, KEY_VK[i])) failed += (failed.empty() ? L"" : L", ") + wstring(KEY_NAME[i]);
    }
    if (!failed.empty()) set_status(L"Another program already uses " + failed + L": use the buttons for it.");
}

void MainWindow::hotkey(int id) {
    trace(::F("hotkey %d rec=%d busy=%d enabled=%d", id, rec != nullptr, (int)busy, (int)w(ID_REC)->enabled));
    if (g_modal > 0) return;                                   // a dialog or the zone picker is open
    if (id == 1) start_recording();
    else if (id == 2) stop_recording();
    else if (id == 3) toggle_hidden();
}

void MainWindow::edit_card(int i) {
    int vw, vh;
    video_size(vw, vh);
    Card c = cards[i];
    if (card_dialog(form.hwnd, i == 0 ? L"Intro of the video" : L"Outro of the video", c, vw, vh, presets, [this](const Presets& p) {
            presets = p;
            save_presets(p);
        })) {
        cards[i] = c;
        int base = i == 0 ? ID_INTRO_K : ID_OUTRO_K;
        w(base + 2)->text = card_summary(c);
        w(base)->checked = card_summary(c) != L"(empty)";
        form.redraw();
    }
}

void MainWindow::start_recording() {
    if (rec || busy || !w(ID_REC)->enabled) return;
    Target t = target();
    busy = true;
    starting = true;
    stop_pending = false;
    last.clear();
    set_status(L"Starting...");
    update();
    Widget* cam = w(ID_CAM);
    wstring webcam = (w(ID_CAM_K)->checked && w(ID_CAM_K)->enabled) ? cam->item_text() : L"";
    static const char* corners[4] = {"bl", "br", "tl", "tr"};
    string corner = corners[std::max(0, std::min(3, w(ID_CORNER)->sel))];
    bool mic = w(ID_MIC_K)->checked && w(ID_MIC_K)->enabled;
    pending.reset(new Recording(ffmpeg, t, out_dir, w(ID_SOUND)->checked, w(ID_CURSOR)->checked, PROFILES[std::max(0, w(ID_QUALITY)->sel)].key, webcam, corner,
                                mic, w(ID_MIC)->enabled ? w(ID_MIC)->item_text() : L""));
    HWND hw = form.hwnd;
    Recording* r = pending.get();
    std::thread([r, hw] {
        bool ok = false;
        try {
            ok = r->start();
        } catch (...) {
            r->set_error(L"unexpected error");
            r->abort();
        }
        PostMessageW(hw, WM_APP_STARTED, ok, 0);
    }).detach();
}

void MainWindow::started(bool ok) {
    trace(::F("started %d error %s", (int)ok, pending ? U(pending->get_error()).c_str() : ""));
    busy = false;
    starting = false;
    if (!ok) {
        stop_pending = false;
        wstring e = pending ? pending->get_error() : L"";
        set_status(L"The recording did not start: " + (e.empty() ? L"unknown error" : e));
        pending.reset();
        update();
        return;
    }
    rec = std::move(pending);
    if (w(ID_CLICKS)->checked) {
        click_watch = true;
        prev_down[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;           // a button already down is not a click
        prev_down[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        SetTimer(form.hwnd, 2, 10, nullptr);
    }
    SetTimer(form.hwnd, 1, 200, nullptr);
    tick();
    update();
    if (stop_pending) {                                        // F6 was pressed while it started
        stop_pending = false;
        stop_recording();
    }
}

// 1:23 or 1:02:03
static wstring clock_text(int s) {
    return s >= 3600 ? fmt_w("%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60) : fmt_w("%02d:%02d", s / 60, s % 60);
}

static wstring size_text(unsigned long long b) {
    if (b < (1ull << 20)) return fmt_w("%.0f KB", b / 1024.0);
    return b >= (1ull << 30) ? fmt_w("%.2f GB", b / 1073741824.0) : fmt_w("%.1f MB", b / 1048576.0);
}

void MainWindow::tick() {
    if (!rec) return;
    if (!rec->alive()) {                                       // ffmpeg stopped by itself (the graphics driver, an antivirus): what exists is saved
        wstring e = rec->get_error();
        note = L"  ffmpeg stopped during the recording" + (e.empty() ? wstring() : L" (" + e + L")") + L": what was recorded before is saved.";
        stop_recording();
        return;
    }
    int s = (int)rec->elapsed();
    wstring snd;
    if (rec->audio) snd = rec->audio->ok ? L", with sound" : L", no sound card";
    if (rec->mic) snd += rec->mic->ok ? L", with the microphone" : L", microphone not available";
    wstring enc = rec->live_encoder == "cpu" ? L"" : L", graphics card";
    wstring sz = size_text(rec->disk_bytes());
    set_status(L"  " + clock_text(s) + L"   " + sz + snd + enc, L"REC");
    SetWindowTextW(form.hwnd, (L"REC " + clock_text(s) + L" - " + sz + L" - " + APP_NAME).c_str());
}

void MainWindow::stop_recording() {
    if (busy && starting) stop_pending = true;
    if (!rec || busy) return;
    std::unique_ptr<Recording> r = std::move(rec);
    KillTimer(form.hwnd, 1);
    KillTimer(form.hwnd, 2);
    click_watch = false;
    // the intro and the outro: the card drawn at the size of the video
    for (int i = 0; i < 2; ++i) {
        int base = i == 0 ? ID_INTRO_K : ID_OUTRO_K;
        if (!w(base)->checked || card_summary(cards[i]) == L"(empty)") continue;
        fs::path p = r->dir / (i == 0 ? L"intro.png" : L"outro.png");
        if (save_card(p, r->out_w, r->out_h, cards[i])) (i == 0 ? r->intro : r->outro) = std::make_pair(p.wstring(), (double)w(base + 3)->val);
    }
    busy = true;
    set_status(L"Saving...");
    SetWindowTextW(form.hwnd, APP_NAME);
    update();
    HWND hw = form.hwnd;
    Recording* raw = r.release();
    std::thread([this, raw, hw] {
        wstring path, err;
        try {
            path = raw->stop_and_save();
            err = raw->get_error();
        } catch (...) {
            err = L"unexpected error while saving";
        }
        done_secs = raw->length();
        delete raw;
        done_path = path;
        done_err = err;
        PostMessageW(hw, WM_APP_DONE, 0, 0);
    }).detach();
}

void MainWindow::saved() {
    trace("saved " + U(done_path) + " err " + U(done_err));
    busy = false;
    if (!done_path.empty()) {
        last = done_path;
        std::error_code ec;
        set_status(L"Saved: " + fs::path(last).filename().wstring() + L"  (" + clock_text((int)(done_secs + 0.5)) + L", " +
                   size_text((unsigned long long)fs::file_size(last, ec)) + L")" + note);
    } else {
        set_status(L"Not saved: " + (done_err.empty() ? L"unknown error" : done_err) + note);
    }
    note.clear();
    update();
    layout();
    if (quit_after) {
        quit_after = false;
        PostMessageW(form.hwnd, WM_CLOSE, 0, 0);
        return;
    }
    if (!IsWindowVisible(form.hwnd)) {
        ShowWindow(form.hwnd, SW_SHOW);
        exclude();
    }
}

void MainWindow::action(Widget& wd) {
    switch (wd.id) {
        case ID_R_ZONE:
        case ID_R_SCREEN:
        case ID_R_WINDOW:
        case ID_R_APPS:
            mode_changed();
            break;
        case ID_ZONE_BTN:
            ++g_modal;
            ShowWindow(form.hwnd, SW_HIDE);
            SetTimer(form.hwnd, 3, 200, nullptr);               // the window is gone before the picker opens
            break;
        case ID_REFRESH:
            refresh_windows();
            break;
        case ID_APPS:
        case ID_WINDOW:
        case ID_SCREEN:
            update();
            break;
        case ID_INTRO_EDIT:
            edit_card(0);
            break;
        case ID_OUTRO_EDIT:
            edit_card(1);
            break;
        case ID_DIR_BTN: {
            IFileOpenDialog* dlg = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) {
                DWORD o = 0;
                dlg->GetOptions(&o);
                dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                dlg->SetTitle(L"Folder of the recordings");
                IShellItem* init = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(out_dir.c_str(), nullptr, IID_PPV_ARGS(&init)))) {
                    dlg->SetFolder(init);
                    init->Release();
                }
                if (SUCCEEDED(dlg->Show(form.hwnd))) {
                    IShellItem* it = nullptr;
                    if (SUCCEEDED(dlg->GetResult(&it))) {
                        PWSTR p = nullptr;
                        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                            out_dir = p;
                            w(ID_DIR_LBL)->text = out_dir;
                            CoTaskMemFree(p);
                        }
                        it->Release();
                    }
                }
                dlg->Release();
            }
            form.redraw();
            break;
        }
        case ID_REC:
            start_recording();
            break;
        case ID_STOP:
            stop_recording();
            break;
        case ID_HIDE:
            toggle_hidden();
            break;
        case ID_PLAY:
            if (!last.empty()) ShellExecuteW(nullptr, L"open", last.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case ID_FOLDER:
            if (!last.empty() && [&] { std::error_code e; return fs::is_regular_file(last, e); }()) ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + last + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            break;
        case ID_ABOUT:
            about_box(form.hwnd);
            break;
        case ID_KEYS:
            ini.set_bool("global_keys", wd.checked);
            register_keys(wd.checked);
            break;
        default:
            break;
    }
}

void MainWindow::load_settings() {
    ini.path = app_dir() / L"settings.ini";
    ini.load();
    string md = ini.get("mode", "zone");
    w(md == "screen" ? ID_R_SCREEN : md == "window" ? ID_R_WINDOW : md == "apps" ? ID_R_APPS : ID_R_ZONE)->checked = true;
    string q = ini.get("quality", "web");
    for (size_t i = 0; i < 3; ++i)
        if (q == PROFILES[i].key) w(ID_QUALITY)->sel = (int)i;
    if (w(ID_QUALITY)->sel < 0) w(ID_QUALITY)->sel = 1;
    w(ID_SOUND)->checked = ini.get_bool("sound", true);
    w(ID_CURSOR)->checked = ini.get_bool("cursor", true);
    w(ID_CLICKS)->checked = ini.get_bool("clicks", false);
    wstring lastmic = W(ini.get("mic_device"));
    Widget* mic = w(ID_MIC);
    for (size_t i = 0; i < mic->items.size(); ++i)
        if (mic->items[i] == lastmic) mic->sel = (int)i;
    w(ID_MIC_K)->checked = w(ID_MIC_K)->enabled && ini.get_bool("mic", false);
    string corner = ini.get("corner", "bl");
    w(ID_CORNER)->sel = corner == "br" ? 1 : corner == "tl" ? 2 : corner == "tr" ? 3 : 0;
    wstring lastcam = W(ini.get("cam"));
    Widget* cam = w(ID_CAM);
    for (size_t i = 0; i < cam->items.size(); ++i)
        if (cam->items[i] == lastcam) cam->sel = (int)i;
    w(ID_CAM_K)->checked = w(ID_CAM_K)->enabled && ini.get_bool("webcam", false);
    presets = load_presets();
    const char* names[2] = {"intro", "outro"};
    for (int i = 0; i < 2; ++i) {
        int base = i == 0 ? ID_INTRO_K : ID_OUTRO_K;
        JV j;
        if (json_parse(ini.get(string(names[i]) + "_card", "{}"), j)) cards[i] = card_from_json(j);
        w(base)->checked = ini.get_bool(string(names[i]) + "_on", false);
        w(base + 3)->val = std::max(1, std::min(15, ini.get_int(string(names[i]) + "_secs", 3)));
        w(base + 2)->text = card_summary(cards[i]);
    }
    PWSTR vid = nullptr;
    fs::path def;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &vid))) def = fs::path(vid) / L"Grabber";
    CoTaskMemFree(vid);
    out_dir = W(ini.get("dir", U(def.wstring())));
    w(ID_DIR_LBL)->text = out_dir;
    w(ID_KEYS)->checked = ini.get_bool("global_keys", true);
}

void MainWindow::save_settings() {
    ini.set("mode", mode());
    string apps;
    Widget* l = w(ID_APPS);
    for (size_t i = 0; i < wins.size() && i < l->checks.size(); ++i)
        if (l->checks[i]) apps += (apps.empty() ? "" : "\n") + U(wins[i].title);
    ini.set("apps", apps);
    ini.set_bool("sound", w(ID_SOUND)->checked);
    ini.set_bool("cursor", w(ID_CURSOR)->checked);
    ini.set_bool("clicks", w(ID_CLICKS)->checked);
    ini.set("quality", PROFILES[std::max(0, w(ID_QUALITY)->sel)].key);
    ini.set_bool("webcam", w(ID_CAM_K)->checked);
    static const char* corners[4] = {"bl", "br", "tl", "tr"};
    ini.set("corner", corners[std::max(0, std::min(3, w(ID_CORNER)->sel))]);
    ini.set_bool("mic", w(ID_MIC_K)->checked);
    if (w(ID_MIC)->enabled) ini.set("mic_device", U(w(ID_MIC)->item_text()));
    if (w(ID_CAM)->enabled) ini.set("cam", U(w(ID_CAM)->item_text()));
    const char* names[2] = {"intro", "outro"};
    for (int i = 0; i < 2; ++i) {
        int base = i == 0 ? ID_INTRO_K : ID_OUTRO_K;
        ini.set_bool(string(names[i]) + "_on", w(base)->checked);
        ini.set(string(names[i]) + "_card", json_dump(card_to_json(cards[i])));
        ini.set_int(string(names[i]) + "_secs", w(base + 3)->val);
    }
    ini.set("dir", U(out_dir));
    ini.set_bool("global_keys", w(ID_KEYS)->checked);
    ini.save();
}

bool MainWindow::on_close() {
    if (busy) {
        MessageBoxW(form.hwnd, L"The video is starting or being saved: wait a moment before quitting.", APP_NAME, MB_OK | MB_ICONINFORMATION);
        return false;
    }
    if (rec) {
        if (MessageBoxW(form.hwnd, L"A recording is running. Stop it and save it before quitting?", APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) {
            quit_after = true;
            stop_recording();                                  // the window closes by itself when the video is saved
        }
        return false;
    }
    save_settings();
    register_keys(false);
    return true;
}

LRESULT MainWindow::on_message(UINT m, WPARAM wp, LPARAM lp, bool& handled) {
    handled = true;
    switch (m) {
        case WM_HOTKEY:
            hotkey((int)wp);
            return 0;
        case WM_TIMER:
            if (wp == 1) {
                tick();
            } else if (wp == 2 && click_watch) {
                for (int i = 0; i < 2; ++i) {
                    bool down = (GetAsyncKeyState(i == 0 ? VK_LBUTTON : VK_RBUTTON) & 0x8000) != 0;
                    if (down && !prev_down[i]) {
                        POINT p = cursor_pos();
                        spawn_ripple(p.x, p.y, i == 1);
                    }
                    prev_down[i] = down;
                }
            } else if (wp == 3) {
                KillTimer(form.hwnd, 3);
                --g_modal;
                open_picker(form.hwnd);
            }
            return 0;
        case WM_APP_STARTED:
            started(wp != 0);
            return 0;
        case WM_APP_DONE:
            saved();
            return 0;
        case WM_APP_ZONE:
            if (wp) {
                int* v = (int*)lp;
                zone = {v[0], v[1], v[2], v[3]};
                have_zone = true;
                delete[] v;
            }
            ShowWindow(form.hwnd, SW_SHOW);
            exclude();
            SetForegroundWindow(form.hwnd);
            update();
            return 0;
        case WM_SHOWWINDOW:
            if (wp) PostMessageW(form.hwnd, WM_APP + 9, 0, 0);
            handled = false;
            return 0;
        case WM_APP + 9:
            exclude();
            return 0;
        case WM_KEYDOWN:                                       // when the keys are not global, they still work in the window
            if (!w(ID_KEYS)->checked) {
                int id = wp == VK_F5 ? 1 : wp == VK_F6 ? 2 : wp == VK_F11 ? 3 : 0;
                if (id) {
                    hotkey(id);
                    return 0;
                }
            }
            handled = false;
            return 0;
        case WM_CLOSE:
            if (on_close()) DestroyWindow(form.hwnd);
            return 0;
        case WM_DPICHANGED: {
            form.set_dpi(HIWORD(wp));
            RECT* r = (RECT*)lp;
            SetWindowPos(form.hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    handled = false;
    return 0;
}

// ------------------------------------------------------------------------------------------------ entry points
static bool register_windows(HINSTANCE inst) {
    if (!register_form_class(inst)) return false;
    WNDCLASSEXW rc{sizeof rc};
    rc.lpfnWndProc = ripple_proc;
    rc.hInstance = inst;
    rc.lpszClassName = RIPPLE_CLASS;
    WNDCLASSEXW pc{sizeof pc};
    pc.lpfnWndProc = picker_proc;
    pc.hInstance = inst;
    pc.lpszClassName = PICKER_CLASS;
    return RegisterClassExW(&rc) && RegisterClassExW(&pc);
}

// tests: opens the zone picker, drags a rectangle with the real mouse (500,400 to 800,650) and writes what it answered
int picker_test(const wstring& out_file) {
    if (!register_windows(GetModuleHandleW(nullptr))) return 1;
    HWND host = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    open_picker(host);
    auto pump = [&](int ms) {
        double end = now() + ms / 1000.0;
        while (now() < end) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
            Sleep(5);
        }
    };
    pump(400);
    SetCursorPos(500, 400);
    pump(100);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    pump(150);
    SetCursorPos(650, 500);
    pump(100);
    SetCursorPos(800, 650);
    pump(150);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    string res = "no answer";
    double end = now() + 3;
    while (now() < end) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_APP_ZONE) {
                if (m.wParam) {
                    int* v = (int*)m.lParam;
                    res = F("zone %d %d %d %d", v[0], v[1], v[2], v[3]);
                    delete[] v;
                } else {
                    res = "cancelled";
                }
                end = 0;
            } else {
                DispatchMessageW(&m);
            }
        }
        Sleep(5);
    }
    write_text_file(out_file, res + "\n");
    return 0;
}
int run_app(HINSTANCE inst, int show, const wstring& shot_path) {
    if (!register_windows(inst)) return 1;
    if (shot_path.empty()) {                                   // one Grabber at a time (per data folder): the second one brings the first to the front
        wstring dirn = app_dir().wstring();
        for (auto& ch : dirn)
            if (ch == L'\\' || ch == L'/') ch = L'_';
        wstring name = L"Local\\BSM.Grabber." + dirn;
        CreateMutexW(nullptr, FALSE, name.c_str());
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            struct Find {
                HWND found = nullptr;
            } f;
            EnumWindows(
                [](HWND h, LPARAM lp) -> BOOL {
                    wchar_t cls[64], title[128];
                    GetClassNameW(h, cls, 64);
                    GetWindowTextW(h, title, 128);
                    if (wcscmp(cls, FORM_CLASS) == 0 && (wcscmp(title, APP_NAME) == 0 || wcsncmp(title, L"REC ", 4) == 0)) ((Find*)lp)->found = h;
                    return TRUE;
                }, (LPARAM)&f);
            if (f.found) {
                ShowWindow(f.found, IsIconic(f.found) ? SW_RESTORE : SW_SHOW);
                SetForegroundWindow(f.found);
            }
            return 0;
        }
        std::thread(purge_stale_temp).detach();
    }
    MainWindow* mw = new MainWindow();
    mw->ffmpeg = find_ffmpeg();
    if (!mw->ffmpeg.empty()) std::thread([f = mw->ffmpeg] { hardware_encoders(f); }).detach();           // the graphics-card test is done now, not at the first F5
    int dpi = GetDpiForSystem();
    mw->form.set_dpi(dpi);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = {0, 0, mw->form.px(520), mw->form.px(600)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
    HWND hw = CreateWindowExW(0, FORM_CLASS, APP_NAME, style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, mw);
    if (!hw) return 1;
    dark_title_bar(hw);
    mw->form.set_dpi(GetDpiForWindow(hw));
    mw->build();
    mw->load_settings();
    {
        wchar_t tz[128];
        int zx, zy, zw, zh;
        if (GetEnvironmentVariableW(L"GRABBER_TEST_ZONE", tz, 128) > 0 && swscanf(tz, L"%d,%d,%d,%d", &zx, &zy, &zw, &zh) == 4) {        // tests: a zone chosen in advance
            mw->zone = {zx, zy, zw, zh};
            mw->have_zone = true;
        }
    }
    mw->refresh_windows();
    mw->layout();
    mw->update();
    if (mw->ffmpeg.empty()) mw->set_status(L"ffmpeg.exe was not found: the recording is not possible.");
    mw->register_keys(mw->w(ID_KEYS)->checked);
    ShowWindow(hw, show);
    UpdateWindow(hw);
    if (shot_path.rfind(L"click:", 0) == 0) {                  // tests: real mouse messages on the main window, then what became of the boxes
        auto click = [&](int id) {
            Widget* x = mw->w(id);
            LPARAM pos = MAKELPARAM((x->r.left + x->r.right) / 2, (x->r.top + x->r.bottom) / 2);
            SendMessageW(hw, WM_LBUTTONDOWN, MK_LBUTTON, pos);
            SendMessageW(hw, WM_LBUTTONUP, 0, pos);
        };
        click(ID_R_SCREEN);
        click(ID_CLICKS);
        click(ID_CLICKS);
        click(ID_CURSOR);
        bool ok = mw->w(ID_R_SCREEN)->checked && !mw->w(ID_R_ZONE)->checked && mw->mode() == "screen" && !mw->w(ID_CLICKS)->checked &&
                  !mw->w(ID_CURSOR)->checked;
        write_text_file(shot_path.substr(6), ok ? "ok\n" : "fail\n");
        mw->register_keys(false);
        DestroyWindow(hw);
        return ok ? 0 : 1;
    }
    if (shot_path.rfind(L"about:", 0) == 0 || shot_path.rfind(L"card:", 0) == 0) {        // a picture of a dialog (used to look at the result)
        bool about = shot_path[0] == L'a';
        g_auto_shot = shot_path.substr(about ? 6 : 5);
        if (about) {
            about_box(hw);
        } else {
            Card c;
            c.text = "Title of the video\nsecond line";
            card_dialog(hw, L"Intro of the video", c, 1920, 1080, mw->presets, nullptr);
        }
        mw->register_keys(false);
        DestroyWindow(hw);
        return 0;
    }
    if (!shot_path.empty()) {                                  // a picture of the window, then quit (used to look at the result)
        for (int i = 0; i < 5; ++i) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            Sleep(30);
        }
        RECT c;
        GetClientRect(hw, &c);
        HDC dc = GetDC(hw), mem = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, c.right, c.bottom);
        HGDIOBJ old = SelectObject(mem, bm);
        mw->form.paint(mem, c);
        SelectObject(mem, old);
        Bitmap bmp(bm, nullptr);
        CLSID png;
        UINT n = 0, sz = 0;
        GetImageEncodersSize(&n, &sz);
        vector<char> buf(sz);
        ImageCodecInfo* info = (ImageCodecInfo*)buf.data();
        GetImageEncoders(n, sz, info);
        for (UINT i = 0; i < n; ++i)
            if (wcscmp(info[i].MimeType, L"image/png") == 0) png = info[i].Clsid;
        bmp.Save(shot_path.c_str(), &png, nullptr);
        DeleteObject(bm);
        DeleteDC(mem);
        ReleaseDC(hw, dc);
        mw->register_keys(false);
        DestroyWindow(hw);
        return 0;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
