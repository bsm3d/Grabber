// Grabber
// © Benoit Saint-Moulin - 2026
// Version du fichier : 1.2.0
// Fonction du fichier : Shared declarations of Grabber: the JSON value, INI settings, process runner, compression profiles and capture types.

#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using std::string;
using std::vector;
using std::wstring;

#define APP_NAME L"Grabber"
#define APP_VERSION L"1.2.0"
#define APP_SITE L"https://www.bsm3d.com"

// ------------------------------------------------------------------------------------------------ small helpers (util.cpp)
wstring W(const string& s);                                    // UTF-8 to UTF-16
string U(const wstring& s);                                    // UTF-16 to UTF-8
string F(const char* fmt, ...);                                // printf into a std::string (numbers and plain text)
double now();                                                  // a steady clock, in seconds
wstring quote_arg(const wstring& a);                           // one command-line argument, quoted for CreateProcess
wstring trim(const wstring& s);
vector<wstring> split_lines(const string& utf8);               // lines of a UTF-8 text, as UTF-16
void write_text_file(const fs::path& p, const string& utf8);
bool read_text_file(const fs::path& p, string& out);

// a small JSON value: enough for the settings, the cards and the presets
struct JV {
    enum T { NUL, BOOL, NUM, STR, OBJ } t = NUL;
    bool b = false;
    double n = 0;
    string s;
    vector<std::pair<string, JV>> o;
    const JV* get(const string& k) const;
    void set(const string& k, const JV& v);
    static JV str(const string& v) { JV j; j.t = STR; j.s = v; return j; }
    static JV num(double v) { JV j; j.t = NUM; j.n = v; return j; }
    static JV boolean(bool v) { JV j; j.t = BOOL; j.b = v; return j; }
    static JV obj() { JV j; j.t = OBJ; return j; }
};
bool json_parse(const string& text, JV& out);
string json_dump(const JV& v, int indent = 0);

// settings.ini: key=value lines under [General]
class Ini {
public:
    fs::path path;
    std::map<string, string> kv;
    void load();
    void save() const;
    string get(const string& k, const string& def = "") const;
    bool get_bool(const string& k, bool def) const;
    int get_int(const string& k, int def) const;
    void set(const string& k, const string& v) { kv[k] = v; }
    void set_bool(const string& k, bool v) { kv[k] = v ? "true" : "false"; }
    void set_int(const string& k, int v) { kv[k] = std::to_string(v); }
};

// a process with pipes
struct Proc {
    HANDLE process = nullptr, in = nullptr, out = nullptr, err = nullptr;
};
bool spawn(const vector<wstring>& args, Proc& p, bool pipe_in, bool pipe_out, bool pipe_err);
// runs a program to its end and returns its exit code (-1 on a timeout or when it cannot start); out and err are filled
int run_capture(const vector<wstring>& args, string& out, string& err, DWORD timeout_ms = 600000);

// ------------------------------------------------------------------------------------------------ the engine (core.cpp)
struct Profile {
    const char* key;
    const wchar_t* label;
    int box_w, box_h, crf;
    const char* preset;
    int fps;
    const char* abr;
    const char* maxrate;                                       // nullptr: none
    const char* buf;
};
extern const Profile PROFILES[3];
const Profile& profile(const string& key);

struct HwEnc {
    string key;
    wstring label;
};
vector<HwEnc> hardware_encoders(const wstring& ffmpeg);
wstring find_ffmpeg();
vector<wstring> webcams(const wstring& ffmpeg);
vector<wstring> microphones();
void purge_stale_temp();                                       // the temporary folders of recordings that ended with their program

struct ScreenInfo {
    wstring name;
    int x, y, w, h;
};
vector<ScreenInfo> screens();
struct WinInfo {
    HWND hwnd;
    wstring title;
};
vector<WinInfo> windows(HWND skip);
bool is_minimized(HWND h);
struct R4 {
    int x = 0, y = 0, w = 0, h = 0;
};
R4 window_rect(HWND h);
DWORD process_of(HWND h);
void bring_to_front(HWND h);
POINT cursor_pos();
bool exclude_from_capture(HWND h, bool on);

struct Target {
    enum Kind { NONE, REGION, SCREEN, WINDOW, APPS } kind = NONE;
    R4 r;                                                      // region / screen
    HWND hwnd = nullptr;                                       // window
    wstring title;
    vector<WinInfo> apps;                                      // apps
};

// one sound written to a WAV on a steady clock (the sound the PC plays, or a microphone)
class AudioTrack {
public:
    AudioTrack(const fs::path& path, bool mic, const wstring& device);
    ~AudioTrack();
    bool start();
    void stop();
    bool ok = false;
    wstring error;
    std::atomic<int> peak{0};
    double t0 = -1;
    fs::path path;
private:
    void run();
    bool mic_;
    wstring device_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<int> state_{0};                                // 0 starting, 1 running, -1 failed
};

struct Seg {
    double t;
    R4 r;
    bool black;
};
struct LogEntry {
    double t;
    bool has;
    R4 r;
};
struct Card;

class Recording {
public:
    Recording(const wstring& ffmpeg, const Target& target, const fs::path& out_dir, bool sound, bool cursor, const string& quality,
              const wstring& webcam, const string& corner, bool mic, const wstring& mic_device);
    ~Recording();
    bool start();
    wstring stop_and_save();                                   // the path of the MP4, empty (with error) when it failed
    void abort();
    double elapsed() const;
    double length() const { return length_; }
    bool alive() const;                                        // ffmpeg is still running
    wstring kept() const { return kept_; }                     // where the recording was kept when it could not be saved                  // seconds recorded, known once stopped
    unsigned long long disk_bytes() const;                     // what the recording takes in the temporary folder so far
    wstring get_error() const;
    void set_error(const wstring& e);

    int fps = 30;
    int w = 0, h = 0, out_w = 0, out_h = 0;
    string live_encoder = "cpu";
    string final_encoder = "cpu";
    fs::path dir;
    std::unique_ptr<AudioTrack> audio, mic;
    std::pair<wstring, double> intro, outro;                   // path of a picture, seconds (empty path: none)
    Target target;
    R4 win_size;
    bool has_cam() const { return !webcam_.empty(); }

private:
    bool input_args(vector<wstring>& a);
    vector<wstring> cam_input() const;
    void watch(HANDLE h);
    void errors(HANDLE h);
    bool front_rect(R4& out) const;
    void follow();
    vector<Seg> segments() const;
    string app_parts(int fps, const vector<Seg>& segs) const;
    vector<wstring> final_cmd(const fs::path& main, const fs::path& out, bool has_audio, const string& enc, const vector<Seg>& segs) const;

    wstring ffmpeg_, webcam_;
    fs::path out_dir_, video_;                                 // video_: the pattern of the pieces, video_000.mkv, video_001.mkv...
    vector<fs::path> video_parts() const;
    double length_ = 0;
    double t_cap0_ = -1;                                       // when the first picture was captured, found when stopping (see stop_and_save)
    wstring kept_;
    string quality_, corner_;
    bool cursor_;
    Proc proc_;
    R4 area_;
    vector<HWND> apps_;
    std::set<DWORD> pids_;
    mutable std::mutex mu_;
    vector<LogEntry> log_;
    std::atomic<bool> stop_follow_{false};
    std::thread follow_th_, watch_th_, err_th_;
    std::atomic<double> t_video_{-1};
    double t_start_ = 0;
    wstring error_;
};

// ------------------------------------------------------------------------------------------------ the title cards (cards.cpp)
struct Card {
    string text, font = "Segoe UI";
    int size = 11;
    bool bold = true, italic = false, shadow = true;
    string color = "#ffffff", bg = "#000000", img, fit = "fit";
    int x = 50, y = 50;
    string align = "center";
};
Card card_from_json(const JV& j);
JV card_to_json(const Card& c);
wstring card_summary(const Card& c);
COLORREF hex_color(const string& s);
string color_hex(COLORREF c);
bool gdip_start();
void gdip_stop();
// the card as a bitmap of w x h (Gdiplus::Bitmap*, owned by the caller)
void* render_card(int w, int h, const Card& c);
bool save_card(const fs::path& path, int w, int h, const Card& c);
void* load_gdip_image(const wstring& path);                    // Gdiplus::Image*
void free_gdip(void* p);
vector<wstring> font_families();
