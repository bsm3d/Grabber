// Grabber
// © Benoit Saint-Moulin - 2026
// Version du fichier : 1.2.0
// Fonction du fichier : Declares the hand-drawn widget set: the Theme colours, the Widget record and the Form that owns one window.

// in dark grey. One Form is the whole content of one window.
#pragma once
#include "grabber.h"

struct Theme {
    COLORREF bg, field, border, text, dim, disabled, btn, btn_hover, btn_border, btn_off, accent, accent_lit, on_accent, ind, sep, sel, menu, red;
};
extern const Theme T;

enum Kind { K_LABEL, K_HEADER, K_BUTTON, K_CHECK, K_RADIO, K_COMBO, K_LIST, K_SPIN, K_SLIDER, K_LINE, K_STATUS, K_CUSTOM };

struct Widget {
    int id = 0;
    Kind kind = K_LABEL;
    RECT r{};                                                  // pixels, in the client area
    wstring text;
    bool visible = true, enabled = true, checked = false, dim = false, wrap = false;
    vector<wstring> items;                                     // combo and list
    vector<char> checks;                                       // list: one check per item
    int sel = -1;                                              // combo: the chosen item; list: the row with the cursor
    int top = 0;                                               // list: first row shown
    int val = 0, minv = 0, maxv = 100;                         // spin, slider
    wstring suffix;                                            // spin
    int group = 0;                                             // radio
    COLORREF swatch = CLR_INVALID;                             // button: a colour shown on its left
    wstring red;                                               // status: a bold red word before the text
    bool focusable() const { return visible && enabled && kind != K_LABEL && kind != K_HEADER && kind != K_LINE && kind != K_STATUS && kind != K_CUSTOM; }
    wstring item_text() const { return sel >= 0 && sel < (int)items.size() ? items[sel] : L""; }
};

class Form {
public:
    Form() = default;
    ~Form();
    HWND hwnd = nullptr;
    int dpi = 96;
    vector<std::unique_ptr<Widget>> ws;
    std::function<void(Widget&)> on_action;                    // a button was pressed, a box toggled, a choice made, a value changed
    std::function<void(Widget&, HDC, const RECT&)> on_custom; // paints a K_CUSTOM widget

    int px(int v) const { return MulDiv(v, dpi, 96); }
    Widget* add(Kind k, int id, const wstring& text = L"");
    Widget* get(int id);
    void set_dpi(int d);
    void paint(HDC dc, const RECT& client);
    // the window procedure of the form's window calls this first; handled is set when the message was used
    LRESULT handle(HWND h, UINT m, WPARAM wp, LPARAM lp, bool& handled);
    void redraw(const Widget* w = nullptr);
    void close_popup();
    HFONT font() const { return f_; }
    HFONT font_bold() const { return fb_; }
    HFONT font_small() const { return fs_; }
    int text_h() const;
    void set_focus_to(Widget* w);

private:
    friend struct PopupData;
    void make_fonts();
    Widget* hit(int x, int y);
    void action(Widget& w);
    void open_combo(Widget& w);
    void step(Widget& w, int dir);
    void toggle(Widget& w);
    void tab(bool back);
    void slider_to(Widget& w, int x);
    HFONT f_ = nullptr, fb_ = nullptr, fs_ = nullptr;
    Widget *hover_ = nullptr, *down_ = nullptr, *focus_ = nullptr;
    HWND popup_ = nullptr;
    int down_part_ = 0;                                        // spin: 1 up, 2 down
};

// a window that holds a Form: the form sees the messages first, then on_message
struct FormWindow {
    Form form;
    virtual ~FormWindow() {}
    virtual LRESULT on_message(UINT, WPARAM, LPARAM, bool& handled) {
        handled = false;
        return 0;
    }
};

// the window class of every Grabber window that holds a Form: the FormWindow is the creation parameter
bool register_form_class(HINSTANCE inst);
extern const wchar_t* FORM_CLASS;
void dark_title_bar(HWND h);
// draws text in a rectangle with the font of the form; flags are DrawText flags
void draw_text(HDC dc, const wstring& s, RECT r, COLORREF c, UINT flags);
