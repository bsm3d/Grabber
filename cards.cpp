// Grabber
// © Benoit Saint-Moulin - 2026
// Version du fichier : 1.2.0
// Fonction du fichier : Draws the title cards of the intro and outro of a recording with GDI+, including its startup and shutdown.

#include "grabber.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>

using namespace Gdiplus;

static ULONG_PTR g_token = 0;

bool gdip_start() {
    GdiplusStartupInput in;
    return GdiplusStartup(&g_token, &in, nullptr) == Ok;
}

void gdip_stop() {
    if (g_token) GdiplusShutdown(g_token);
    g_token = 0;
}

COLORREF hex_color(const string& s) {
    unsigned v = 0;
    if (s.size() == 7 && s[0] == '#') v = (unsigned)strtoul(s.c_str() + 1, nullptr, 16);
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

string color_hex(COLORREF c) { return F("#%02x%02x%02x", GetRValue(c), GetGValue(c), GetBValue(c)); }

static string jstr(const JV& j, const char* k, const string& def) {
    const JV* v = j.get(k);
    return v && v->t == JV::STR ? v->s : def;
}
static int jint(const JV& j, const char* k, int def) {
    const JV* v = j.get(k);
    return v && v->t == JV::NUM ? (int)v->n : def;
}
static bool jbool(const JV& j, const char* k, bool def) {
    const JV* v = j.get(k);
    if (!v) return def;
    if (v->t == JV::BOOL) return v->b;
    if (v->t == JV::NUM) return v->n != 0;
    return def;
}

Card card_from_json(const JV& j) {
    Card c;
    c.text = jstr(j, "text", c.text);
    c.font = jstr(j, "font", c.font);
    c.size = std::max(2, std::min(40, jint(j, "size", c.size)));
    c.bold = jbool(j, "bold", c.bold);
    c.italic = jbool(j, "italic", c.italic);
    c.shadow = jbool(j, "shadow", c.shadow);
    c.color = jstr(j, "color", c.color);
    c.bg = jstr(j, "bg", c.bg);
    c.img = jstr(j, "img", c.img);
    c.fit = jstr(j, "fit", c.fit) == "fill" ? "fill" : "fit";
    c.x = std::max(0, std::min(100, jint(j, "x", c.x)));
    c.y = std::max(0, std::min(100, jint(j, "y", c.y)));
    c.align = jstr(j, "align", c.align);
    if (c.align != "left" && c.align != "right") c.align = "center";
    return c;
}

JV card_to_json(const Card& c) {
    JV j = JV::obj();
    j.set("text", JV::str(c.text));
    j.set("font", JV::str(c.font));
    j.set("size", JV::num(c.size));
    j.set("bold", JV::boolean(c.bold));
    j.set("italic", JV::boolean(c.italic));
    j.set("shadow", JV::boolean(c.shadow));
    j.set("color", JV::str(c.color));
    j.set("bg", JV::str(c.bg));
    j.set("img", JV::str(c.img));
    j.set("fit", JV::str(c.fit));
    j.set("x", JV::num(c.x));
    j.set("y", JV::num(c.y));
    j.set("align", JV::str(c.align));
    return j;
}

wstring card_summary(const Card& c) {
    wstring t = W(c.text), words, cur;
    for (wchar_t ch : t) {
        if (iswspace(ch)) {
            if (!cur.empty()) words += (words.empty() ? L"" : L" ") + cur;
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) words += (words.empty() ? L"" : L" ") + cur;
    if (!words.empty()) return words.size() <= 26 ? words : words.substr(0, 24) + L"...";
    if (!c.img.empty()) return fs::path(W(c.img)).filename().wstring();
    return L"(empty)";
}

static Color gc(COLORREF c, int a = 255) { return Color((BYTE)a, GetRValue(c), GetGValue(c), GetBValue(c)); }

void* load_gdip_image(const wstring& path) {
    Image* im = Image::FromFile(path.c_str());
    if (!im || im->GetLastStatus() != Ok) {
        delete im;
        return nullptr;
    }
    return im;
}

void free_gdip(void* p) { delete (Image*)p; }

void* render_card(int w, int h, const Card& c) {
    Bitmap* bmp = new Bitmap(w, h, PixelFormat32bppRGB);
    Graphics g(bmp);
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.Clear(gc(hex_color(c.bg)));
    std::error_code ec;
    if (!c.img.empty() && fs::is_regular_file(W(c.img), ec)) {
        static std::mutex img_mu;                              // the picture is decoded once, not at each key typed in the editor
        static wstring img_path;
        static Image* img_keep = nullptr;
        std::lock_guard<std::mutex> lk(img_mu);
        if (!img_keep || img_path != W(c.img)) {
            free_gdip(img_keep);
            img_keep = (Image*)load_gdip_image(W(c.img));
            img_path = W(c.img);
        }
        Image* pic = img_keep;
        if (pic) {
            double iw = pic->GetWidth(), ih = pic->GetHeight();
            if (iw > 0 && ih > 0) {
                double k = c.fit == "fill" ? std::max(w / iw, h / ih) : std::min(w / iw, h / ih);
                double sw = std::round(iw * k), sh = std::round(ih * k);
                g.DrawImage(pic, RectF((REAL)((w - sw) / 2), (REAL)((h - sh) / 2), (REAL)sw, (REAL)sh));
            }
        }
    }
    wstring text = W(c.text);
    text.erase(std::remove(text.begin(), text.end(), L'\r'), text.end());
    while (!text.empty() && text.front() == L'\n') text.erase(text.begin());
    while (!text.empty() && text.back() == L'\n') text.pop_back();
    if (!trim(text).empty()) {
        int bw = (int)(w * 0.84);
        int size = std::max(8, (int)(h * c.size / 100.0));
        std::unique_ptr<FontFamily> fam_own(new FontFamily(W(c.font).c_str()));
        if (!fam_own->IsAvailable()) {
            fam_own.reset(new FontFamily(L"Segoe UI"));
            if (!fam_own->IsAvailable()) fam_own.reset(FontFamily::GenericSansSerif()->Clone());
        }
        FontFamily& fam = *fam_own;
        int style = (c.bold ? FontStyleBold : 0) | (c.italic ? FontStyleItalic : 0);
        StringFormat sf(StringFormat::GenericTypographic());
        sf.SetFormatFlags(sf.GetFormatFlags() & ~StringFormatFlagsNoWrap);
        sf.SetAlignment(c.align == "left" ? StringAlignmentNear : c.align == "right" ? StringAlignmentFar : StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        RectF bounds;
        while (true) {                                         // smaller until the block fits in the picture
            Font font(&fam, (REAL)size, style, UnitPixel);
            g.MeasureString(text.c_str(), -1, &font, RectF(0, 0, (REAL)bw, (REAL)h), &sf, &bounds);
            if ((bounds.Height <= h * 0.92 && bounds.Width <= bw) || size <= 8) break;
            size -= 2;
        }
        Font font(&fam, (REAL)size, style, UnitPixel);
        double bh = std::ceil(bounds.Height);
        double box_w = std::min((double)bw, (double)std::ceil(bounds.Width) + std::max(4, size / 4));   // the box hugs the text, so the place moves the text itself to the very edges
        double cx = w * c.x / 100.0, cy = h * c.y / 100.0;
        double left = std::min(std::max(cx - box_w / 2.0, w * 0.02), w * 0.98 - box_w);
        double top = std::min(std::max(cy - bh / 2.0, h * 0.02), h * 0.98 - bh);
        RectF box((REAL)left, (REAL)top, (REAL)box_w, (REAL)bh);
        if (c.shadow) {
            double off = std::max(1, size / 16);
            SolidBrush sh(Color(200, 0, 0, 0));
            g.DrawString(text.c_str(), -1, &font, RectF(box.X + (REAL)off, box.Y + (REAL)off, box.Width, box.Height), &sf, &sh);
        }
        SolidBrush br(gc(hex_color(c.color)));
        g.DrawString(text.c_str(), -1, &font, box, &sf, &br);
    }
    return bmp;
}

static bool encoder_clsid(const WCHAR* mime, CLSID* out) {
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    if (size == 0) return false;
    vector<char> buf(size);
    ImageCodecInfo* info = (ImageCodecInfo*)buf.data();
    GetImageEncoders(n, size, info);
    for (UINT i = 0; i < n; ++i)
        if (wcscmp(info[i].MimeType, mime) == 0) {
            *out = info[i].Clsid;
            return true;
        }
    return false;
}

bool save_card(const fs::path& path, int w, int h, const Card& c) {
    Bitmap* bmp = (Bitmap*)render_card(w, h, c);
    CLSID png;
    bool ok = encoder_clsid(L"image/png", &png) && bmp->Save(path.c_str(), &png, nullptr) == Ok;
    delete bmp;
    return ok;
}

vector<wstring> font_families() {
    InstalledFontCollection col;
    int n = col.GetFamilyCount(), got = 0;
    vector<FontFamily> fams(n > 0 ? n : 1);
    col.GetFamilies(n, fams.data(), &got);
    vector<wstring> out;
    for (int i = 0; i < got; ++i) {
        WCHAR name[LF_FACESIZE];
        fams[i].GetFamilyName(name);
        out.push_back(name);
    }
    std::sort(out.begin(), out.end(), [](const wstring& a, const wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
