// Grabber
// © Benoit Saint-Moulin - 2026
// Version du fichier : 1.2.0
// Fonction du fichier : Implementation of the hand-drawn widget set: buttons, check boxes, combo boxes, lists, spin boxes and sliders.

#include "toolkit.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>

// the dark palette
const Theme T = {RGB(0x32, 0x32, 0x32), RGB(0x26, 0x26, 0x26), RGB(0x1a, 0x1a, 0x1a), RGB(0xd6, 0xd6, 0xd6), RGB(0x9a, 0x9a, 0x9a), RGB(0x7a, 0x7a, 0x7a),
                 RGB(0x42, 0x42, 0x42), RGB(0x50, 0x50, 0x50), RGB(0x58, 0x58, 0x58), RGB(0x36, 0x36, 0x36), RGB(0xa8, 0xa8, 0xa8), RGB(0xc8, 0xc8, 0xc8),
                 RGB(0x1e, 0x1e, 0x1e), RGB(0x55, 0x55, 0x55), RGB(0x40, 0x40, 0x40), RGB(0x4a, 0x4a, 0x4a), RGB(0x2a, 0x2a, 0x2a), RGB(0xe5, 0x32, 0x2d)};

const wchar_t* FORM_CLASS = L"GrabberForm";
static const wchar_t* POPUP_CLASS = L"GrabberPopup";
static const int ROW_H = 22;                                   // logical height of a row of a list or a popup

void dark_title_bar(HWND h) {
    BOOL on = TRUE;
    DwmSetWindowAttribute(h, 20, &on, sizeof on);              // DWMWA_USE_IMMERSIVE_DARK_MODE
    DwmSetWindowAttribute(h, 19, &on, sizeof on);              // the same, before Windows 10 20H1
}

void draw_text(HDC dc, const wstring& s, RECT r, COLORREF c, UINT flags) {
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, flags | DT_NOPREFIX);
}

static void fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

// a rounded rectangle with a fill and a 1 pixel border
static void rounded(HDC dc, const RECT& r, int radius, COLORREF fillc, COLORREF border) {
    HBRUSH b = CreateSolidBrush(fillc);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF c, int w = 1) {
    HPEN p = CreatePen(PS_SOLID, w, c);
    HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, op);
    DeleteObject(p);
}

static void triangle_down(HDC dc, int cx, int cy, int s, COLORREF c) {
    POINT pt[3] = {{cx - s, cy - s / 2}, {cx + s, cy - s / 2}, {cx, cy + s / 2 + 1}};
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Polygon(dc, pt, 3);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void triangle_up(HDC dc, int cx, int cy, int s, COLORREF c) {
    POINT pt[3] = {{cx - s, cy + s / 2}, {cx + s, cy + s / 2}, {cx, cy - s / 2 - 1}};
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Polygon(dc, pt, 3);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

// ------------------------------------------------------------------------------------------------ the popup of a combo box
struct PopupData {
    Form* form = nullptr;
    Widget* w = nullptr;
    vector<wstring> items;
    int sel = -1, hover = -1, top = 0, visible = 0, row = 22;
    HFONT font = nullptr;
};

static void popup_close(HWND h, int pick) {
    PopupData* d = (PopupData*)GetWindowLongPtrW(h, GWLP_USERDATA);
    Form* f = d ? d->form : nullptr;
    Widget* w = d ? d->w : nullptr;
    if (GetCapture() == h) ReleaseCapture();
    if (f) f->close_popup();                                   // destroys the window
    if (f && w && pick >= 0) {
        w->sel = pick;
        f->redraw(w);
        if (f->on_action) f->on_action(*w);
    }
}

static LRESULT CALLBACK popup_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    PopupData* d = (PopupData*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
            return TRUE;
        case WM_NCDESTROY:
            delete d;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            return DefWindowProcW(h, m, wp, lp);
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT c;
            GetClientRect(h, &c);
            fill(dc, c, T.menu);
            SelectObject(dc, d->font);
            for (int i = 0; i < d->visible; ++i) {
                int idx = d->top + i;
                if (idx >= (int)d->items.size()) break;
                RECT r = {1, 1 + i * d->row, c.right - 1, 1 + (i + 1) * d->row};
                if (idx == d->hover) fill(dc, r, T.accent);
                else if (idx == d->sel) fill(dc, r, T.sel);
                RECT tr = r;
                tr.left += 8;
                tr.right -= 8;
                draw_text(dc, d->items[idx], tr, idx == d->hover ? T.on_accent : T.text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
            if ((int)d->items.size() > d->visible) {           // a thin scroll bar
                int total = (int)d->items.size(), th = std::max(12, (int)((c.bottom - 2) * d->visible / total));
                int ty = 1 + (c.bottom - 2 - th) * d->top / std::max(1, total - d->visible);
                RECT sb = {c.right - 6, ty, c.right - 2, ty + th};
                fill(dc, sb, T.btn_border);
            }
            HBRUSH nb = CreateSolidBrush(T.btn_border);
            FrameRect(dc, &c, nb);
            DeleteObject(nb);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int idx = d->top + (GET_Y_LPARAM(lp) - 1) / d->row;
            RECT c;
            GetClientRect(h, &c);
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!PtInRect(&c, p) || idx >= (int)d->items.size()) idx = -1;
            if (idx != d->hover) {
                d->hover = idx;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int dir = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -3 : 3;
            d->top = std::max(0, std::min((int)d->items.size() - d->visible, d->top + dir));
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            RECT c;
            GetClientRect(h, &c);
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!PtInRect(&c, p)) popup_close(h, -1);          // a click outside closes the list
            return 0;
        }
        case WM_LBUTTONUP: {
            RECT c;
            GetClientRect(h, &c);
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (PtInRect(&c, p)) {
                int idx = d->top + (p.y - 1) / d->row;
                if (idx >= 0 && idx < (int)d->items.size()) popup_close(h, idx);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if ((HWND)lp != h && d && d->form) popup_close(h, -1);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) popup_close(h, -1);
            else if (wp == VK_RETURN) popup_close(h, d->hover >= 0 ? d->hover : d->sel);
            else if (wp == VK_DOWN || wp == VK_UP) {
                int n = (int)d->items.size(), cur = d->hover >= 0 ? d->hover : d->sel;
                cur = std::max(0, std::min(n - 1, cur + (wp == VK_DOWN ? 1 : -1)));
                d->hover = cur;
                if (cur < d->top) d->top = cur;
                if (cur >= d->top + d->visible) d->top = cur - d->visible + 1;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ------------------------------------------------------------------------------------------------ the form
Form::~Form() {
    for (HFONT f : {f_, fb_, fs_})
        if (f) DeleteObject(f);
}

void Form::make_fonts() {
    for (HFONT f : {f_, fb_, fs_})
        if (f) DeleteObject(f);
    auto mk = [&](int px_h, int weight) {
        return CreateFontW(-px(px_h), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };
    f_ = mk(12, FW_NORMAL);
    fb_ = mk(12, FW_BOLD);
    fs_ = mk(11, FW_NORMAL);
}

int Form::text_h() const {
    HDC dc = GetDC(hwnd);
    HGDIOBJ o = SelectObject(dc, f_);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, o);
    ReleaseDC(hwnd, dc);
    return tm.tmHeight;
}

void Form::set_dpi(int d) {
    dpi = d;
    make_fonts();
}

Widget* Form::add(Kind k, int id, const wstring& text) {
    if (!f_) make_fonts();
    ws.emplace_back(new Widget());
    Widget* w = ws.back().get();
    w->kind = k;
    w->id = id;
    w->text = text;
    return w;
}

Widget* Form::get(int id) {
    for (auto& w : ws)
        if (w->id == id) return w.get();
    return nullptr;
}

void Form::redraw(const Widget*) {
    if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
}

void Form::set_focus_to(Widget* w) {
    focus_ = w;
    redraw();
}

Widget* Form::hit(int x, int y) {
    POINT p = {x, y};
    for (size_t i = ws.size(); i-- > 0;) {
        Widget* w = ws[i].get();
        if (w->visible && PtInRect(&w->r, p) && w->kind != K_LABEL && w->kind != K_HEADER && w->kind != K_LINE && w->kind != K_STATUS) return w;
    }
    return nullptr;
}

void Form::close_popup() {
    HWND p = popup_;
    popup_ = nullptr;
    if (p) DestroyWindow(p);
    redraw();
}

void Form::action(Widget& w) {
    if (on_action) on_action(w);
}

void Form::open_combo(Widget& w) {
    if (popup_ || w.items.empty()) return;
    PopupData* d = new PopupData();
    d->form = this;
    d->w = &w;
    d->items = w.items;
    d->sel = w.sel;
    d->hover = w.sel;
    d->row = px(ROW_H);
    d->visible = std::min((int)w.items.size(), 12);
    d->font = f_;
    int n = (int)w.items.size();
    d->top = std::max(0, std::min(w.sel - d->visible / 2, n - d->visible));
    POINT p = {w.r.left, w.r.bottom};
    ClientToScreen(hwnd, &p);
    int width = w.r.right - w.r.left, height = d->visible * d->row + 2;
    RECT wa;
    HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    wa = mi.rcWork;
    if (p.y + height > wa.bottom) {                            // not enough room below: above the box
        POINT t = {w.r.left, w.r.top};
        ClientToScreen(hwnd, &t);
        p.y = t.y - height;
    }
    popup_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, POPUP_CLASS, L"", WS_POPUP, p.x, p.y, width, height, hwnd, nullptr,
                             GetModuleHandleW(nullptr), d);
    ShowWindow(popup_, SW_SHOWNOACTIVATE);
    SetCapture(popup_);
}

void Form::step(Widget& w, int dir) {
    switch (w.kind) {
        case K_COMBO: {
            int n = (int)w.items.size();
            if (n == 0) return;
            int s = std::max(0, std::min(n - 1, w.sel + dir));
            if (s != w.sel) {
                w.sel = s;
                action(w);
            }
            break;
        }
        case K_SPIN:
        case K_SLIDER: {
            int v = std::max(w.minv, std::min(w.maxv, w.val + dir));
            if (v != w.val) {
                w.val = v;
                action(w);
            }
            break;
        }
        case K_LIST: {
            int n = (int)w.items.size();
            if (n == 0) return;
            w.sel = std::max(0, std::min(n - 1, (w.sel < 0 ? 0 : w.sel) + dir));
            int rows = std::max(1, (int)((w.r.bottom - w.r.top - 2) / px(ROW_H)));
            if (w.sel < w.top) w.top = w.sel;
            if (w.sel >= w.top + rows) w.top = w.sel - rows + 1;
            break;
        }
        default:
            break;
    }
    redraw();
}

void Form::toggle(Widget& w) {
    if (w.kind == K_CHECK) {
        w.checked = !w.checked;
        action(w);
    } else if (w.kind == K_RADIO) {
        if (!w.checked) {
            for (auto& o : ws)
                if (o->kind == K_RADIO && o->group == w.group) o->checked = false;
            w.checked = true;
            action(w);
        }
    } else if (w.kind == K_BUTTON) {
        action(w);
    } else if (w.kind == K_COMBO) {
        open_combo(w);
    } else if (w.kind == K_LIST && w.sel >= 0 && w.sel < (int)w.checks.size()) {
        w.checks[w.sel] = !w.checks[w.sel];
        action(w);
    }
    redraw();
}

void Form::tab(bool back) {
    vector<Widget*> order;
    for (auto& w : ws)
        if (w->focusable()) order.push_back(w.get());
    if (order.empty()) return;
    auto it = std::find(order.begin(), order.end(), focus_);
    size_t i = it == order.end() ? (back ? order.size() - 1 : 0) : (size_t)(it - order.begin());
    if (it != order.end()) i = back ? (i + order.size() - 1) % order.size() : (i + 1) % order.size();
    focus_ = order[i];
    redraw();
}

void Form::slider_to(Widget& w, int x) {
    int pad = px(8), span = std::max(1, (int)(w.r.right - w.r.left) - 2 * pad);
    int v = w.minv + (int)((double)(x - w.r.left - pad) / span * (w.maxv - w.minv) + 0.5);
    v = std::max(w.minv, std::min(w.maxv, v));
    if (v != w.val) {
        w.val = v;
        action(w);
        redraw();
    }
}

LRESULT Form::handle(HWND h, UINT m, WPARAM wp, LPARAM lp, bool& handled) {
    handled = true;
    switch (m) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT c;
            GetClientRect(h, &c);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bm = CreateCompatibleBitmap(dc, c.right, c.bottom);
            HGDIOBJ old = SelectObject(mem, bm);
            paint(mem, c);
            BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bm);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (down_ && down_->kind == K_SLIDER) {
                slider_to(*down_, x);
                return 0;
            }
            Widget* w = hit(x, y);
            if (w && !w->enabled) w = nullptr;
            if (w != hover_) {
                hover_ = w;
                redraw();
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = nullptr;
            redraw();
            return 0;
        case WM_LBUTTONDOWN: {
            if (popup_) {
                close_popup();
                return 0;
            }
            SetFocus(h);
            Widget* w = hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (!w || !w->enabled) return 0;
            if (w->focusable()) focus_ = w;
            down_ = w;
            down_part_ = 0;
            SetCapture(h);
            if (w->kind == K_SLIDER) slider_to(*w, GET_X_LPARAM(lp));
            if (w->kind == K_SPIN) {
                down_part_ = GET_Y_LPARAM(lp) < (w->r.top + w->r.bottom) / 2 ? 1 : 2;
                if (GET_X_LPARAM(lp) >= w->r.right - px(18)) step(*w, down_part_ == 1 ? 1 : -1);
            }
            redraw();
            return 0;
        }
        case WM_LBUTTONUP: {
            Widget* w = down_;                                 // read before ReleaseCapture: it sends WM_CAPTURECHANGED, which clears down_
            down_ = nullptr;
            if (GetCapture() == h) ReleaseCapture();
            redraw();
            if (!w) return 0;
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!PtInRect(&w->r, p)) return 0;
            if (w->kind == K_BUTTON || w->kind == K_CHECK || w->kind == K_RADIO || w->kind == K_COMBO) toggle(*w);
            else if (w->kind == K_LIST) {
                int row = (p.y - w->r.top - 1) / px(ROW_H) + w->top;
                if (row >= 0 && row < (int)w->items.size() && p.x < w->r.right - px(8)) {
                    w->sel = row;
                    if (row < (int)w->checks.size()) w->checks[row] = !w->checks[row];
                    action(*w);
                    redraw();
                }
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            return SendMessageW(h, WM_LBUTTONDOWN, wp, lp);
        case WM_MOUSEWHEEL: {
            if (popup_) {                                      // the wheel goes to the list that is open
                SendMessageW(popup_, WM_MOUSEWHEEL, wp, lp);
                return 0;
            }
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &p);
            Widget* w = hit(p.x, p.y);
            if (!w || !w->enabled) return 0;
            int dir = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;
            if (w->kind == K_LIST) {
                int rows = std::max(1, (int)((w->r.bottom - w->r.top - 2) / px(ROW_H)));
                w->top = std::max(0, std::min(std::max(0, (int)w->items.size() - rows), w->top + dir * 3));
                redraw();
            } else if (w->kind == K_SPIN || w->kind == K_SLIDER) {
                step(*w, -dir);
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if (popup_) {
                SendMessageW(popup_, WM_KEYDOWN, wp, lp);
                return 0;
            }
            bool shift = GetKeyState(VK_SHIFT) < 0;
            if (wp == VK_TAB) {
                tab(shift);
                return 0;
            }
            if (!focus_ || !focus_->enabled) {
                handled = false;
                return 0;
            }
            Widget& w = *focus_;
            if (wp == VK_SPACE || (wp == VK_RETURN && w.kind == K_BUTTON)) {
                toggle(w);
                return 0;
            }
            if (w.kind == K_COMBO && (wp == VK_F4 || (wp == VK_DOWN && GetKeyState(VK_MENU) < 0))) {
                open_combo(w);
                return 0;
            }
            int dir = (wp == VK_UP || wp == VK_LEFT) ? -1 : (wp == VK_DOWN || wp == VK_RIGHT) ? 1 : 0;
            if (dir) {
                if (w.kind == K_RADIO) {                       // arrows move inside a group of radio buttons
                    vector<Widget*> g;
                    for (auto& o : ws)
                        if (o->kind == K_RADIO && o->group == w.group && o->enabled && o->visible) g.push_back(o.get());
                    auto it = std::find(g.begin(), g.end(), &w);
                    if (it != g.end()) {
                        size_t i = ((it - g.begin()) + g.size() + dir) % g.size();
                        focus_ = g[i];
                        toggle(*g[i]);
                    }
                } else if (w.kind == K_SPIN || w.kind == K_SLIDER) {
                    step(w, (wp == VK_UP || wp == VK_RIGHT) ? 1 : -1);
                } else if (w.kind == K_COMBO || w.kind == K_LIST) {
                    step(w, dir);
                } else {
                    handled = false;
                }
                return 0;
            }
            handled = false;
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            redraw();
            handled = false;
            return 0;
        case WM_CAPTURECHANGED:
            if (down_ && (HWND)lp != h) {
                down_ = nullptr;
                redraw();
            }
            return 0;
        case WM_ENABLE:
        case WM_DESTROY:
            handled = false;
            return 0;
    }
    handled = false;
    return 0;
}

void Form::paint(HDC dc, const RECT& client) {
    fill(dc, client, T.bg);
    SetBkMode(dc, TRANSPARENT);
    bool has_focus = GetFocus() == hwnd;
    for (auto& up : ws) {
        Widget& w = *up;
        if (!w.visible) continue;
        RECT r = w.r;
        bool hov = hover_ == &w, dn = down_ == &w, foc = has_focus && focus_ == &w;
        COLORREF fg = w.enabled ? T.text : T.disabled;
        switch (w.kind) {
            case K_LABEL: {
                SelectObject(dc, f_);
                draw_text(dc, w.text, r, w.dim ? (w.enabled ? T.dim : T.disabled) : fg, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                break;
            }
            case K_HEADER: {
                SelectObject(dc, fs_);
                SetTextCharacterExtra(dc, px(2));
                wstring up_text = w.text;
                for (auto& ch : up_text) ch = (wchar_t)towupper(ch);
                draw_text(dc, up_text, r, T.dim, DT_SINGLELINE | DT_VCENTER);
                SetTextCharacterExtra(dc, 0);
                break;
            }
            case K_LINE:
                line(dc, r.left, (r.top + r.bottom) / 2, r.right, (r.top + r.bottom) / 2, T.sep);
                break;
            case K_STATUS: {
                SelectObject(dc, f_);
                RECT tr = r;
                if (!w.red.empty()) {
                    SelectObject(dc, fb_);
                    RECT m = {0, 0, 0, 0};
                    DrawTextW(dc, w.red.c_str(), (int)w.red.size(), &m, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                    draw_text(dc, w.red, tr, T.red, DT_SINGLELINE | DT_NOPREFIX);
                    SelectObject(dc, f_);
                    tr.left += m.right + px(8);
                }
                draw_text(dc, w.text, tr, T.text, DT_WORDBREAK | DT_NOPREFIX);
                break;
            }
            case K_BUTTON: {
                SelectObject(dc, f_);
                COLORREF face = !w.enabled ? T.btn_off : dn ? T.accent : hov ? T.btn_hover : T.btn;
                COLORREF edge = foc ? T.accent : (hov && w.enabled) ? T.btn_border : T.border;
                rounded(dc, r, px(3), face, edge);
                RECT tr = r;
                if (w.swatch != CLR_INVALID) {
                    RECT sw = {r.left + px(5), r.top + px(5), r.left + px(19), r.bottom - px(5)};
                    fill(dc, sw, w.swatch);
                    tr.left += px(20);
                }
                draw_text(dc, w.text, tr, !w.enabled ? T.disabled : dn ? T.on_accent : T.text, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
                break;
            }
            case K_CHECK:
            case K_RADIO: {
                SelectObject(dc, f_);
                int s = px(14), cy = (r.top + r.bottom) / 2;
                RECT b = {r.left, cy - s / 2, r.left + s, cy - s / 2 + s};
                COLORREF edge = foc ? T.accent : hov ? T.btn_border : T.ind;
                if (w.kind == K_CHECK) {
                    rounded(dc, b, px(2), w.enabled ? T.field : T.btn_off, edge);
                    if (w.checked) {
                        RECT in = {b.left + px(3), b.top + px(3), b.right - px(3), b.bottom - px(3)};
                        fill(dc, in, w.enabled ? T.accent : T.disabled);
                    }
                } else {
                    HBRUSH nb = CreateSolidBrush(w.enabled ? T.field : T.btn_off);
                    HPEN np = CreatePen(PS_SOLID, 1, edge);
                    HGDIOBJ ob = SelectObject(dc, nb), op = SelectObject(dc, np);
                    Ellipse(dc, b.left, b.top, b.right, b.bottom);
                    if (w.checked) {
                        HBRUSH ab = CreateSolidBrush(w.enabled ? T.accent : T.disabled);
                        HPEN ap = CreatePen(PS_SOLID, 1, w.enabled ? T.accent : T.disabled);
                        SelectObject(dc, ab);
                        SelectObject(dc, ap);
                        Ellipse(dc, b.left + px(4), b.top + px(4), b.right - px(4), b.bottom - px(4));
                        SelectObject(dc, nb);
                        SelectObject(dc, np);
                        DeleteObject(ab);
                        DeleteObject(ap);
                    }
                    SelectObject(dc, ob);
                    SelectObject(dc, op);
                    DeleteObject(nb);
                    DeleteObject(np);
                }
                RECT tr = {b.right + px(7), r.top, r.right, r.bottom};
                draw_text(dc, w.text, tr, fg, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                if (foc) {
                    RECT m = tr;
                    DrawTextW(dc, w.text.c_str(), (int)w.text.size(), &m, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                    RECT fr = {tr.left - px(3), cy - px(9), std::min(tr.right, tr.left + m.right + px(3)), cy + px(9)};
                    line(dc, fr.left, fr.bottom, fr.right, fr.bottom, T.accent);
                }
                break;
            }
            case K_COMBO:
            case K_SPIN: {
                SelectObject(dc, f_);
                COLORREF edge = foc ? T.accent : (hov && w.enabled) ? T.btn_border : T.border;
                rounded(dc, r, px(3), w.enabled ? T.field : T.btn_off, edge);
                int aw = w.kind == K_COMBO ? px(22) : px(18);
                RECT tr = {r.left + px(8), r.top, r.right - aw - px(4), r.bottom};
                wstring txt = w.kind == K_COMBO ? w.item_text() : std::to_wstring(w.val) + w.suffix;
                draw_text(dc, txt, tr, fg, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                RECT ar = {r.right - aw, r.top + 1, r.right - 1, r.bottom - 1};
                if (w.kind == K_COMBO) {
                    fill(dc, ar, w.enabled ? (hov ? T.accent : T.btn) : T.btn_off);
                    triangle_down(dc, (ar.left + ar.right) / 2, (ar.top + ar.bottom) / 2, px(4), w.enabled ? (hov ? T.on_accent : T.text) : T.disabled);
                } else {
                    fill(dc, ar, w.enabled ? T.btn : T.btn_off);
                    int mid = (ar.top + ar.bottom) / 2;
                    line(dc, ar.left, mid, ar.right, mid, T.border);
                    triangle_up(dc, (ar.left + ar.right) / 2, (ar.top + mid) / 2, px(3), w.enabled ? T.text : T.disabled);
                    triangle_down(dc, (ar.left + ar.right) / 2, (mid + ar.bottom) / 2, px(3), w.enabled ? T.text : T.disabled);
                }
                break;
            }
            case K_LIST: {
                SelectObject(dc, f_);
                rounded(dc, r, px(3), T.field, foc ? T.accent : T.border);
                int rh = px(ROW_H), rows = std::max(1, (int)((r.bottom - r.top - 2) / rh));
                IntersectClipRect(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1);
                for (int i = 0; i < rows; ++i) {
                    int idx = w.top + i;
                    if (idx >= (int)w.items.size()) break;
                    RECT rr = {r.left + 1, r.top + 1 + i * rh, r.right - 1, r.top + 1 + (i + 1) * rh};
                    if (idx == w.sel && foc) fill(dc, rr, T.sel);
                    int s = px(13), cy = (rr.top + rr.bottom) / 2;
                    RECT b = {rr.left + px(6), cy - s / 2, rr.left + px(6) + s, cy - s / 2 + s};
                    rounded(dc, b, px(2), T.field, T.ind);
                    if (idx < (int)w.checks.size() && w.checks[idx]) {
                        RECT in = {b.left + px(3), b.top + px(3), b.right - px(3), b.bottom - px(3)};
                        fill(dc, in, T.accent);
                    }
                    RECT tr = {b.right + px(7), rr.top, rr.right - px(8), rr.bottom};
                    draw_text(dc, w.items[idx], tr, w.enabled ? T.text : T.disabled, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                }
                if ((int)w.items.size() > rows) {
                    int total = (int)w.items.size(), th = std::max(px(12), (int)((r.bottom - r.top - 4) * rows / total));
                    int ty = r.top + 2 + ((r.bottom - r.top - 4 - th) * w.top) / std::max(1, total - rows);
                    RECT sb = {r.right - px(6), ty, r.right - px(2), ty + th};
                    fill(dc, sb, T.btn_border);
                }
                SelectClipRgn(dc, nullptr);
                break;
            }
            case K_SLIDER: {
                int pad = px(8), cy = (r.top + r.bottom) / 2, span = std::max(1, (int)(r.right - r.left) - 2 * pad);
                RECT tr = {r.left + pad, cy - px(2), r.right - pad, cy + px(2)};
                rounded(dc, tr, px(2), T.field, T.border);
                int kx = r.left + pad + (int)((double)(w.val - w.minv) / std::max(1, w.maxv - w.minv) * span);
                RECT done = {r.left + pad, cy - px(1), kx, cy + px(2) - 1};
                if (done.right > done.left) fill(dc, done, w.enabled ? T.accent : T.disabled);
                RECT k = {kx - px(6), cy - px(8), kx + px(6), cy + px(8)};
                rounded(dc, k, px(3), dn ? T.accent : hov ? T.btn_hover : T.btn, foc ? T.accent : T.btn_border);
                break;
            }
            case K_CUSTOM:
                if (on_custom) {
                    int saved = SaveDC(dc);
                    IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
                    on_custom(w, dc, r);
                    RestoreDC(dc, saved);
                }
                break;
        }
    }
}

// ------------------------------------------------------------------------------------------------ the window class
static LRESULT CALLBACK form_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) {
        FormWindow* fw = (FormWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)fw);
        if (fw) fw->form.hwnd = h;
        return DefWindowProcW(h, m, wp, lp);
    }
    FormWindow* fw = (FormWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (fw) {
        bool handled = false;
        LRESULT r = fw->form.handle(h, m, wp, lp, handled);
        if (handled) return r;
        r = fw->on_message(m, wp, lp, handled);
        if (handled) return r;
    }
    return DefWindowProcW(h, m, wp, lp);
}

bool register_form_class(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = form_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = FORM_CLASS;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    WNDCLASSEXW pc{sizeof pc};
    pc.style = CS_DROPSHADOW;
    pc.lpfnWndProc = popup_proc;
    pc.hInstance = inst;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pc.lpszClassName = POPUP_CLASS;
    return RegisterClassExW(&wc) && RegisterClassExW(&pc);
}
