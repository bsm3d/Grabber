// Grabber, the engine: what can be captured (a zone, a screen, a window, several applications), the video (ffmpeg, gdigrab), the sound of the PC
// (WASAPI loopback) and the final MP4 (H.264 + AAC).
//
// The video goes live to a temporary file, the sound is written to a WAV on the same clock; when the recording stops both are joined into one MP4,
// then encoded a last time with the chosen compression (with an intro and an outro if asked) and the temporary files are deleted.
// If the PC played nothing at all, the MP4 has no sound track.
#include <initguid.h>

#include "grabber.h"

#include <audioclient.h>
#include <dwmapi.h>
#include <propsys.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <ctime>

// ------------------------------------------------------------------------------------------------ compression
// the three ways to write the final MP4 (H.264 + AAC), all in Full HD (the size is the same, the compression changes)
const Profile PROFILES[3] = {
    {"streaming", L"Streaming  (Full HD, light file)", 1920, 1080, 26, "veryfast", 30, "96k", "4M", "8M"},
    {"web", L"Web  (Full HD, balanced)", 1920, 1080, 23, "fast", 30, "128k", "8M", "16M"},
    {"full", L"Full HD  (best quality)", 1920, 1080, 18, "medium", 30, "192k", nullptr, nullptr},
};

const Profile& profile(const string& key) {
    for (auto& p : PROFILES)
        if (key == p.key) return p;
    return PROFILES[1];
}

// ------------------------------------------------------------------------------------------------ the encoder
// the graphics-card encoders, best first. Each one is only offered when a real test encode works on this PC
struct HwDef {
    const char* key;
    const char* name;
    const wchar_t* label;
};
static const HwDef HW_ENCODERS[] = {{"nvenc", "h264_nvenc", L"NVIDIA NVENC"},
                                    {"qsv", "h264_qsv", L"Intel Quick Sync"},
                                    {"amf", "h264_amf", L"AMD AMF"},
                                    {"mf", "h264_mf", L"Windows hardware encoder"}};

static vector<wstring> args_of(std::initializer_list<string> l) {
    vector<wstring> v;
    for (auto& s : l) v.push_back(W(s));
    return v;
}

static void add(vector<wstring>& v, const vector<wstring>& more) { v.insert(v.end(), more.begin(), more.end()); }
static void add(vector<wstring>& v, std::initializer_list<string> l) { add(v, args_of(l)); }

// the ffmpeg arguments of one graphics-card encoder at about the quality of x264 at this crf
static vector<wstring> hw_args(const string& key, int crf, const char* maxrate, const char* buf) {
    string c = std::to_string(crf);
    vector<wstring> a;
    if (key == "nvenc") add(a, {"-c:v", "h264_nvenc", "-preset", "p5", "-rc", "vbr", "-cq", c, "-b:v", "0", "-profile:v", "high"});
    else if (key == "qsv") add(a, {"-c:v", "h264_qsv", "-preset", "faster", "-global_quality", c, "-profile:v", "high"});
    else if (key == "amf") add(a, {"-c:v", "h264_amf", "-quality", "quality", "-rc", "cqp", "-qp_i", c, "-qp_p", c, "-qp_b", c});
    else {                                                     // Media Foundation: the quality scale is 0 to 100
        int q = std::max(30, std::min(95, (int)std::lround(100 - (crf - 14) * 4)));
        add(a, {"-c:v", "h264_mf", "-hw_encoding", "1", "-rate_control", "quality", "-quality", std::to_string(q)});
    }
    if (maxrate && key != "mf") add(a, {"-maxrate", maxrate, "-bufsize", buf});
    return a;
}

static string pix_fmt(const string& key) { return key == "mf" ? "nv12" : "yuv420p"; }

static vector<HwEnc> hw_probe(const wstring& ffmpeg) {
    vector<HwEnc> found;
    string out, err;
    run_capture({ffmpeg, L"-hide_banner", L"-encoders"}, out, err, 15000);
    for (auto& d : HW_ENCODERS) {
        if (out.find(d.name) == string::npos) continue;
        vector<wstring> cmd = {ffmpeg};
        add(cmd, {"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc=size=1280x720:rate=30", "-frames:v", "15", "-pix_fmt", pix_fmt(d.key)});
        add(cmd, hw_args(d.key, 22, nullptr, nullptr));
        add(cmd, {"-f", "null", "-"});
        string o2, e2;                                         // not out: it holds the list of the encoders, still being read
        if (run_capture(cmd, o2, e2, 30000) == 0) found.push_back({d.key, d.label});
    }
    return found;
}

// [(key, label)] of the graphics-card encoders that really work here (a one second test encode each), best first. Takes a few seconds the first time
vector<HwEnc> hardware_encoders(const wstring& ffmpeg) {
    static std::mutex mu;
    static std::map<wstring, vector<HwEnc>> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(ffmpeg);
    if (it != cache.end()) return it->second;
    return cache[ffmpeg] = hw_probe(ffmpeg);
}

// the encoder key to use: "cpu" (x264), or the best graphics-card one that works here
static string pick_encoder(const wstring& ffmpeg) {
    auto k = hardware_encoders(ffmpeg);
    return k.empty() ? "cpu" : k[0].key;
}

static vector<wstring> video_args(const string& key, int crf, const char* preset, const char* maxrate, const char* buf, bool live) {
    if (key == "cpu") {
        vector<wstring> a;
        add(a, {"-c:v", "libx264", "-preset", preset, "-crf", std::to_string(crf)});
        if (!live) add(a, {"-profile:v", "high"});
        if (maxrate) add(a, {"-maxrate", maxrate, "-bufsize", buf});
        return a;
    }
    return hw_args(key, crf, maxrate, buf);
}

// the ffmpeg overlay position of the camera: 10 pixels from the two edges of its corner
static string corner_position(const string& corner) {
    string x = (corner == "bl" || corner == "tl") ? "10" : "main_w-overlay_w-10";
    string y = (corner == "tl" || corner == "tr") ? "10" : "main_h-overlay_h-10";
    return x + ":" + y;
}

// the size of a w x h picture shrunk to fit in mw x mh (never enlarged), both sides even
static void fit_size(int w, int h, int mw, int mh, int& ow, int& oh) {
    double k = std::min({1.0, mw / (double)w, mh / (double)h});
    ow = std::max(2, (int)(w * k) / 2 * 2);
    oh = std::max(2, (int)(h * k) / 2 * 2);
}

wstring find_ffmpeg() {
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    fs::path base = fs::path(buf).parent_path();
    for (fs::path c : {base / L"ffmpeg.exe", base / L"ffmpeg" / L"ffmpeg.exe"}) {
        std::error_code ec;
        if (fs::is_regular_file(c, ec)) return c.wstring();
    }
    wchar_t found[MAX_PATH * 2];
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, (DWORD)std::size(found), found, nullptr)) return found;
    return L"";
}

// the names of the cameras DirectShow knows
vector<wstring> webcams(const wstring& ffmpeg) {
    string out, err;
    run_capture({ffmpeg, L"-hide_banner", L"-list_devices", L"true", L"-f", L"dshow", L"-i", L"dummy"}, out, err, 15000);
    vector<wstring> names;
    string section;
    size_t i = 0;
    while (i < err.size()) {
        size_t j = err.find('\n', i);
        if (j == string::npos) j = err.size();
        string line = err.substr(i, j - i);
        i = j + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.find("Alternative name") != string::npos) continue;
        if (line.find("DirectShow video devices") != string::npos) section = "video";
        else if (line.find("DirectShow audio devices") != string::npos) section = "audio";
        size_t q1 = line.find('"');
        if (q1 == string::npos) continue;
        size_t q2 = line.find('"', q1 + 1);
        if (q2 == string::npos) continue;
        wstring name = W(line.substr(q1 + 1, q2 - q1 - 1));
        string kind;
        bool has_kind = false;
        size_t par = line.rfind('(');
        if (!line.empty() && line.back() == ')' && par != string::npos) {
            kind = line.substr(par + 1, line.size() - par - 2);
            has_kind = true;
        }
        if (((has_kind && kind == "video") || (!has_kind && section == "video")) && std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
    }
    return names;
}

// ------------------------------------------------------------------------------------------------ what to capture
// the monitors, in physical pixels (the program is DPI aware), the primary one first
vector<ScreenInfo> screens() {
    struct M {
        bool primary;
        RECT r;
    };
    vector<M> mons;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(hm, &mi);
            ((vector<M>*)lp)->push_back({(mi.dwFlags & MONITORINFOF_PRIMARY) != 0, mi.rcMonitor});
            return TRUE;
        }, (LPARAM)&mons);
    std::stable_sort(mons.begin(), mons.end(), [](const M& a, const M& b) { return a.primary && !b.primary; });
    vector<ScreenInfo> out;
    for (size_t i = 0; i < mons.size(); ++i) {
        int w = mons[i].r.right - mons[i].r.left, h = mons[i].r.bottom - mons[i].r.top;
        out.push_back({W(F("Screen %d%s  (%d x %d)", (int)i + 1, mons[i].primary ? ", main" : "", w, h)), mons[i].r.left, mons[i].r.top, w, h});
    }
    return out;
}

// the windows one can capture: visible, titled, not hidden by the system (a minimised window is listed: it is brought back when the recording starts)
vector<WinInfo> windows(HWND skip) {
    struct Ctx {
        HWND skip;
        vector<WinInfo> out;
    } ctx{skip, {}};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            Ctx* c = (Ctx*)lp;
            if (hwnd == c->skip || !IsWindowVisible(hwnd)) return TRUE;
            int n = GetWindowTextLengthW(hwnd);
            if (n == 0) return TRUE;
            DWORD cloaked = 0;
            DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked);            // windows of other virtual desktops, apps in the background
            if (cloaked) return TRUE;
            wstring t(n + 1, L'\0');
            int got = GetWindowTextW(hwnd, &t[0], n + 1);
            t.resize(got > 0 ? got : 0);
            if (t.empty()) return TRUE;
            RECT r;
            GetWindowRect(hwnd, &r);
            if ((!IsIconic(hwnd) && (r.right - r.left < 40 || r.bottom - r.top < 40)) || t == L"Program Manager" || t == L"Settings" ||
                t == L"Microsoft Text Input Application")
                return TRUE;
            c->out.push_back({hwnd, t});
            return TRUE;
        }, (LPARAM)&ctx);
    return ctx.out;
}

bool is_minimized(HWND h) { return IsIconic(h) != FALSE; }

// the visible rectangle of a window (without its invisible shadow border), in physical pixels
R4 window_rect(HWND h) {
    RECT r{};
    if (DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r) != S_OK && !GetWindowRect(h, &r)) return {0, 0, 0, 0};
    return {r.left, r.top, r.right - r.left, r.bottom - r.top};
}

DWORD process_of(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid;
}

// a window to the front: the system refuses it to a program that is not in front, a tap on Alt lifts the refusal
void bring_to_front(HWND h) {
    if (is_minimized(h)) ShowWindow(h, SW_RESTORE);
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(h);
}

POINT cursor_pos() {
    POINT p{};
    GetCursorPos(&p);
    return p;
}

// the tool's own window is not in the recording (Windows 10 2004 and later)
bool exclude_from_capture(HWND h, bool on) { return SetWindowDisplayAffinity(h, on ? 0x11 : 0) != FALSE; }

// ------------------------------------------------------------------------------------------------ the sound
namespace {
struct ComInit {
    bool ok;
    ComInit() : ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComInit() {
        if (ok) CoUninitialize();
    }
};

template <class T>
struct Rel {
    T* p = nullptr;
    ~Rel() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() { return p; }
    operator bool() const { return p != nullptr; }
};

wstring friendly_name(IMMDevice* d) {
    Rel<IPropertyStore> ps;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &ps))) return L"";
    PROPVARIANT v;
    PropVariantInit(&v);
    wstring r;
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) r = v.pwszVal;
    PropVariantClear(&v);
    return r;
}
}  // namespace

// the names of the microphones (WASAPI inputs), the default one first; empty when there is none
vector<wstring> microphones() {
    vector<wstring> names;
    std::thread([&] {
        ComInit com;
        Rel<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) return;
        wstring def;
        Rel<IMMDevice> dd;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &dd))) def = friendly_name(dd.p);
        Rel<IMMDeviceCollection> col;
        if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) return;
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            Rel<IMMDevice> d;
            if (FAILED(col->Item(i, &d))) continue;
            wstring nm = friendly_name(d.p);
            if (!nm.empty() && std::find(names.begin(), names.end(), nm) == names.end()) names.push_back(nm);
        }
        auto it = std::find(names.begin(), names.end(), def);
        if (it != names.end() && it != names.begin()) std::rotate(names.begin(), it, it + 1);
    }).join();
    return names;
}

AudioTrack::AudioTrack(const fs::path& p, bool mic, const wstring& device) : path(p), mic_(mic), device_(device) {}

AudioTrack::~AudioTrack() { stop(); }

bool AudioTrack::start() {
    th_ = std::thread([this] { run(); });
    for (int i = 0; i < 300 && state_ == 0; ++i) Sleep(10);          // wait for the stream to open (or to fail)
    ok = state_ == 1;
    if (!ok) {
        stop_ = true;
        if (th_.joinable()) th_.join();
    }
    return ok;
}

void AudioTrack::stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
}

static void put_le(FILE* f, unsigned v, int n) {
    for (int i = 0; i < n; ++i) fputc((v >> (8 * i)) & 0xFF, f);
}

void AudioTrack::run() {
    ComInit com;
    Rel<IMMDeviceEnumerator> en;
    Rel<IMMDevice> dev;
    Rel<IAudioClient> cl;
    Rel<IAudioCaptureClient> cap;
    WAVEFORMATEX* wf = nullptr;
    auto fail = [&](const wchar_t* m) {
        error = m;
        if (wf) CoTaskMemFree(wf);
        state_ = -1;
    };
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) return fail(L"no sound system");
    if (mic_) {
        if (!device_.empty()) {
            Rel<IMMDeviceCollection> col;
            UINT n = 0;
            if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) col->GetCount(&n);
            for (UINT i = 0; i < n && !dev; ++i) {
                Rel<IMMDevice> d;
                if (SUCCEEDED(col->Item(i, &d)) && friendly_name(d.p) == device_) {
                    dev.p = d.p;
                    d.p = nullptr;
                }
            }
        }
        if (!dev && FAILED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev))) return fail(L"no microphone");
    } else if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
        return fail(L"no speakers");
    }
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cl))) return fail(L"the sound device is not available");
    if (FAILED(cl->GetMixFormat(&wf))) return fail(L"no sound format");
    if (FAILED(cl->Initialize(AUDCLNT_SHAREMODE_SHARED, mic_ ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK, 5000000, 0, wf, nullptr))) return fail(L"the sound device refused");
    if (FAILED(cl->GetService(__uuidof(IAudioCaptureClient), (void**)&cap))) return fail(L"no capture service");
    int channels = wf->nChannels, rate = (int)wf->nSamplesPerSec, bits = wf->wBitsPerSample;
    bool is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) is_float = ((WAVEFORMATEXTENSIBLE*)wf)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
    int bytes = bits / 8, oc = std::min(channels, 2);
    CoTaskMemFree(wf);
    wf = nullptr;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return fail(L"cannot write the sound file");
    for (int i = 0; i < 44; ++i) fputc(0, f);                  // the header is written at the end, when the length is known
    if (FAILED(cl->Start())) {
        fclose(f);
        return fail(L"the sound device did not start");
    }
    t0 = now();
    state_ = 1;
    unsigned long long frames_out = 0;                         // frames written
    int extra = 0;
    vector<short> buf;
    auto sample = [&](const BYTE* p) -> float {
        if (is_float) return *(const float*)p;
        if (bytes == 2) return *(const short*)p / 32768.f;
        if (bytes == 3) return (((int)(p[0] << 8 | p[1] << 16 | p[2] << 24)) >> 8) / 8388608.f;
        if (bytes == 4) return *(const int*)p / 2147483648.f;
        if (bytes == 1) return ((int)p[0] - 128) / 128.f;
        return 0.f;
    };
    auto to16 = [](float v) -> short {
        v = std::max(-1.f, std::min(1.f, v));
        return (short)std::lround(v * 32767.f);
    };
    while (true) {
        bool stopping = stop_;
        bool got = false;
        UINT32 n = 0;
        while (SUCCEEDED(cap->GetNextPacketSize(&n)) && n > 0) {
            BYTE* data = nullptr;
            UINT32 fr = 0;
            DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &fr, &flags, nullptr, nullptr))) break;
            got = true;
            buf.assign((size_t)fr * oc, 0);
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                for (UINT32 i = 0; i < fr; ++i) {
                    const BYTE* fp = data + (size_t)i * channels * bytes;
                    float l = sample(fp), r = oc == 2 ? sample(fp + bytes) : l, norm = 1.f;
                    if (channels >= 4) {                       // front pair, then (5.1, 7.1) centre and low frequency, back pair, side pair
                        auto ch = [&](int k) { return k < channels ? 0.707f * sample(fp + k * bytes) : 0.f; };
                        if (channels == 4) {                   // quad: front pair, back pair
                            l += ch(2);
                            r += ch(3);
                            norm = 1.707f;
                        } else {
                            float c = ch(2);
                            l += c + ch(4) + ch(6);
                            r += c + ch(5) + ch(7);
                            norm = channels >= 8 ? 3.1f : 2.4f;
                        }
                    }
                    buf[(size_t)i * oc] = to16(l / norm);
                    if (oc == 2) buf[(size_t)i * oc + 1] = to16(r / norm);
                }
            }
            int pk = peak;
            for (short s : buf) pk = std::max(pk, (int)std::abs((int)s));
            peak = pk;
            fwrite(buf.data(), sizeof(short), buf.size(), f);
            frames_out += fr;
            cap->ReleaseBuffer(fr);
        }
        long long want = (long long)((now() - t0) * rate);
        if (want - (long long)frames_out > rate / 20) {        // more than 50 ms behind: nothing was playing, write silence
            vector<short> z((size_t)(want - frames_out) * oc, 0);
            fwrite(z.data(), sizeof(short), z.size(), f);
            frames_out = (unsigned long long)want;
        }
        if (stopping && (!got || ++extra >= 5)) break;          // what is still in the buffer is read; a source that keeps coming does not hold the stop
        Sleep(10);
    }
    cl->Stop();
    unsigned data_len = (unsigned)(frames_out * oc * 2);
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    put_le(f, 36 + data_len, 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put_le(f, 16, 4);
    put_le(f, 1, 2);
    put_le(f, oc, 2);
    put_le(f, rate, 4);
    put_le(f, rate * oc * 2, 4);
    put_le(f, oc * 2, 2);
    put_le(f, 16, 2);
    fwrite("data", 1, 4, f);
    put_le(f, data_len, 4);
    fclose(f);
}

// ------------------------------------------------------------------------------------------------ the recording
static fs::path make_temp_dir() {
    wchar_t t[MAX_PATH];
    GetTempPathW(MAX_PATH, t);
    for (int i = 0; i < 100; ++i) {
        fs::path p = fs::path(t) / (L"grabber" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64() % 100000000 + i));
        if (CreateDirectoryW(p.c_str(), nullptr)) return p;
    }
    return fs::path();                                         // no temporary folder: the recording cannot start, and nothing is ever deleted
}

Recording::Recording(const wstring& ffmpeg, const Target& tg, const fs::path& out_dir, bool sound, bool cursor, const string& quality,
                     const wstring& webcam, const string& corner, bool mic_on, const wstring& mic_device)
    : target(tg), ffmpeg_(ffmpeg), webcam_(webcam), out_dir_(out_dir), quality_(quality), corner_(corner), cursor_(cursor) {
    quality_ = profile(quality).key;
    if (corner_ != "bl" && corner_ != "br" && corner_ != "tl" && corner_ != "tr") corner_ = "bl";
    dir = make_temp_dir();
    video_ = dir / L"video_%03d.mkv";
    if (sound) audio.reset(new AudioTrack(dir / L"sound.wav", false, L""));
    if (mic_on) mic.reset(new AudioTrack(dir / L"mic.wav", true, mic_device));
}

Recording::~Recording() {
    stop_follow_ = true;
    if (follow_th_.joinable()) follow_th_.join();
}

wstring Recording::get_error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return error_;
}

void Recording::set_error(const wstring& e) {
    std::lock_guard<std::mutex> lk(mu_);
    error_ = e;
}

// the pieces of the video written so far, in order
vector<fs::path> Recording::video_parts() const {
    vector<fs::path> parts;
    std::error_code ec;
    if (dir.empty()) return parts;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        wstring n = e.path().filename().wstring();
        if (n.rfind(L"video_", 0) == 0 && e.path().extension() == L".mkv") parts.push_back(e.path());
    }
    std::sort(parts.begin(), parts.end());
    return parts;
}

unsigned long long Recording::disk_bytes() const {
    unsigned long long total = 0;
    std::error_code ec;
    if (dir.empty()) return 0;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code e2;
        if (e.is_regular_file(e2)) total += (unsigned long long)e.file_size(e2);
    }
    return total;
}

bool Recording::alive() const { return proc_.process && WaitForSingleObject(proc_.process, 0) == WAIT_TIMEOUT; }

// the length of a media file, in seconds (0 when it cannot be read): ffmpeg prints it
static double media_duration(const wstring& ffmpeg, const fs::path& file) {
    string so, se;
    run_capture({ffmpeg, L"-hide_banner", L"-i", file.wstring()}, so, se, 30000);
    size_t i = se.find("Duration: ");
    int h = 0, m = 0;
    double s = 0;
    if (i == string::npos || sscanf(se.c_str() + i + 10, "%d:%d:%lf", &h, &m, &s) != 3) return 0;
    return h * 3600.0 + m * 60.0 + s;
}

double Recording::elapsed() const {
    double t = t_video_;
    return t < 0 ? 0 : now() - t;
}

// the ffmpeg arguments of the picture to capture; w and h are set
bool Recording::input_args(vector<wstring>& a) {
    add(a, {"-f", "gdigrab", "-framerate", std::to_string(fps), "-draw_mouse", cursor_ ? "1" : "0"});
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (target.kind == Target::WINDOW) {
        // gdigrab's own "title=" mode shows nothing for windows drawn by the graphics card (browsers, most modern apps): the window is
        // brought to the front and its rectangle on the screen is recorded instead (it stays where it was when the recording began)
        HWND hw = target.hwnd;
        if (!IsWindow(hw)) {
            set_error(L"the window is closed");
            return false;
        }
        bool was_min = is_minimized(hw);
        if (was_min) ShowWindow(hw, SW_RESTORE);
        SetForegroundWindow(hw);
        Sleep(was_min ? 1000 : 400);
        R4 r = window_rect(hw);
        int x2 = std::min(r.x + r.w, vx + vw), y2 = std::min(r.y + r.h, vy + vh);
        int x = std::max(r.x, vx), y = std::max(r.y, vy);
        w = (x2 - x) / 2 * 2;
        h = (y2 - y) / 2 * 2;
        if (w < 16 || h < 16) {
            set_error(L"the window is outside the screen");
            return false;
        }
        add(a, {"-offset_x", std::to_string(x), "-offset_y", std::to_string(y), "-video_size", F("%dx%d", w, h), "-i", "desktop"});
        return true;
    }
    if (target.kind == Target::APPS) {
        vector<HWND> hw;
        for (auto& wi : target.apps)
            if (IsWindow(wi.hwnd)) hw.push_back(wi.hwnd);
        if (hw.empty()) {
            set_error(L"the applications are closed");
            return false;
        }
        bool woke = false;
        for (HWND h2 : hw)
            if (is_minimized(h2)) {
                ShowWindow(h2, SW_RESTORE);
                woke = true;
            }
        bring_to_front(hw[0]);
        Sleep(woke ? 1000 : 400);
        vector<R4> rects;
        for (HWND h2 : hw) rects.push_back(window_rect(h2));
        vector<ScreenInfo> mons;
        for (auto& m : screens())
            for (auto& r : rects)
                if (m.x < r.x + r.w && r.x < m.x + m.w && m.y < r.y + r.h && r.y < m.y + m.h) {
                    mons.push_back(m);
                    break;
                }
        if (mons.empty()) mons.push_back({L"", vx, vy, vw, vh});
        int x1 = INT_MAX, y1 = INT_MAX, x2 = INT_MIN, y2 = INT_MIN;
        for (auto& m : mons) {
            x1 = std::min(x1, m.x);
            y1 = std::min(y1, m.y);
            x2 = std::max(x2, m.x + m.w);
            y2 = std::max(y2, m.y + m.h);
        }
        w = (x2 - x1) / 2 * 2;
        h = (y2 - y1) / 2 * 2;
        area_ = {x1, y1, w, h};
        apps_ = hw;
        for (HWND h2 : hw) pids_.insert(process_of(h2));
        R4 big = rects[0];
        for (auto& r : rects)
            if (r.w * r.h > big.w * big.h) big = r;
        win_size = {0, 0, std::max(2, std::min(big.w, w)), std::max(2, std::min(big.h, h))};
        add(a, {"-offset_x", std::to_string(x1), "-offset_y", std::to_string(y1), "-video_size", F("%dx%d", w, h), "-i", "desktop"});
        return true;
    }
    int rx2 = std::min(target.r.x + target.r.w, vx + vw), ry2 = std::min(target.r.y + target.r.h, vy + vh);        // what is on the screen
    int x = std::max(target.r.x, vx), y = std::max(target.r.y, vy);
    w = (rx2 - x) / 2 * 2;
    h = (ry2 - y) / 2 * 2;
    if (w < 16 || h < 16) {
        set_error(L"the zone is outside the screen, or too small");
        return false;
    }
    add(a, {"-offset_x", std::to_string(x), "-offset_y", std::to_string(y), "-video_size", F("%dx%d", w, h), "-i", "desktop"});
    return true;
}

vector<wstring> Recording::cam_input() const {
    if (webcam_.rfind(L"test:", 0) == 0) {                     // a test picture instead of a camera (the tests)
        return args_of({"-re", "-f", "lavfi", "-i", "testsrc=size=640x480:rate=30"});
    }
    vector<wstring> v = args_of({"-use_wallclock_as_timestamps", "1", "-thread_queue_size", "512", "-rtbufsize", "64M", "-f", "dshow", "-i"});
    v.push_back(L"video=" + webcam_);
    return v;
}

void Recording::watch(HANDLE h) {
    string pending;
    char buf[4096];
    DWORD n;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) {
        pending.append(buf, n);
        size_t i;
        while ((i = pending.find('\n')) != string::npos) {
            string line = pending.substr(0, i);
            pending.erase(0, i + 1);
            if (t_video_ < 0 && line.rfind("frame=", 0) == 0) {
                int fr = atoi(line.c_str() + 6);
                if (fr > 0) t_video_ = now();                  // "frame=0" comes before any picture, and when ffmpeg fails to start
            }
        }
    }
}

void Recording::errors(HANDLE h) {
    string all;
    char buf[4096];
    DWORD n;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) all.append(buf, n);
    auto lines = split_lines(all);
    for (size_t i = lines.size(); i-- > 0;)
        if (!trim(lines[i]).empty()) {
            set_error(trim(lines[i]));
            return;
        }
}

static void close_proc(Proc& p, std::thread& a, std::thread& b, bool kill) {
    if (p.process && kill && WaitForSingleObject(p.process, 0) == WAIT_TIMEOUT) {
        TerminateProcess(p.process, 1);
        WaitForSingleObject(p.process, 5000);
    }
    if (p.in) CloseHandle(p.in);
    if (a.joinable()) a.join();
    if (b.joinable()) b.join();
    for (HANDLE h : {p.out, p.err, p.process})
        if (h) CloseHandle(h);
    p = Proc();
}

bool Recording::start() {
    vector<wstring> inp;
    if (dir.empty()) {
        set_error(L"no temporary folder");
        return false;
    }
    {
        std::error_code ec;
        fs::create_directories(out_dir_, ec);
        fs::path probe = out_dir_ / L".grabber_write_test";
        FILE* pf = _wfopen(probe.c_str(), L"wb");
        if (!pf) {
            set_error(L"the folder of the videos cannot be written: " + out_dir_.wstring());
            abort();
            return false;
        }
        fclose(pf);
        fs::remove(probe, ec);
    }
    if (!input_args(inp)) {
        abort();
        return false;
    }
    const Profile& P = profile(quality_);
    R4 src = target.kind == Target::APPS ? win_size : R4{0, 0, w, h};
    fit_size(src.w ? src.w : w, src.h ? src.h : h, P.box_w, P.box_h, out_w, out_h);
    string crop = "crop=trunc(iw/2)*2:trunc(ih/2)*2,format=yuv420p";
    vector<wstring> base = {ffmpeg_};
    add(base, {"-hide_banner", "-loglevel", "error", "-y", "-use_wallclock_as_timestamps", "1"});
    add(base, inp);
    if (has_cam() && target.kind == Target::APPS) {            // the camera is kept apart: it is put on the cut-out picture when saving
        add(base, cam_input());
        add(base, {"-filter_complex", "[0:v]" + crop + "[bg];[1:v]format=yuv420p[cam]", "-map", "[bg]", "-map", "[cam]"});
    } else if (has_cam()) {                                    // the camera: small, in its corner, 10 pixels from the edges of the picture
        int cw = std::max(2, std::min(480, std::min(std::max(160, w / 5), w / 2))) / 2 * 2;
        add(base, cam_input());
        add(base, {"-filter_complex", F("[0:v]%s[bg];[1:v]scale=%d:-2,format=yuv420p[cam];[bg][cam]overlay=%s[v]", crop.c_str(), cw, corner_position(corner_).c_str()),
                   "-map", "[v]"});
    } else {
        add(base, {"-vf", crop});
    }
    t_start_ = now();
    if (audio) audio->start();                                 // started first: its start is trimmed to the picture's when joining
    if (mic) mic->start();
    string first = pick_encoder(ffmpeg_);
    vector<string> keys = {first};
    if (first != "cpu") keys.push_back("cpu");                 // a graphics-card encoder that fails to start: the processor takes over
    for (auto& key : keys) {
        set_error(L"");
        t_video_ = -1;
        live_encoder = key;
        vector<wstring> cmd = base;
        add(cmd, video_args(key, 20, "veryfast", nullptr, nullptr, true));
        add(cmd, {"-pix_fmt", pix_fmt(key), "-progress", "pipe:1", "-stats_period", "0.05", "-nostats"});
        // pieces of ten minutes in the temporary folder: a long recording is never one huge file, and they are put together when saving
        wchar_t sg[16];                                        // tests: GRABBER_SEGMENT seconds
        string seg_time = GetEnvironmentVariableW(L"GRABBER_SEGMENT", sg, 16) > 0 ? U(sg) : "120";
        add(cmd, {"-flush_packets", "1", "-f", "segment", "-segment_time", seg_time, "-segment_format", "matroska", "-reset_timestamps", "1"});
        cmd.push_back(video_.wstring());
        if (!spawn(cmd, proc_, true, true, true)) {
            set_error(L"ffmpeg could not start");
            continue;
        }
        watch_th_ = std::thread([this, h = proc_.out] { watch(h); });
        err_th_ = std::thread([this, h = proc_.err] { errors(h); });
        double end = now() + 20;                               // wait for the first picture (or the failure): a slow machine, two big screens
        while (t_video_ < 0 && WaitForSingleObject(proc_.process, 0) == WAIT_TIMEOUT && now() < end) Sleep(20);
        if (t_video_ >= 0) {
            if (target.kind == Target::APPS) follow_th_ = std::thread([this] { follow(); });
            return true;
        }
        close_proc(proc_, watch_th_, err_th_, true);
    }
    if (get_error().empty()) set_error(L"ffmpeg wrote no picture");
    abort();
    return false;
}

// apps: the rectangle (in the picture) of the application in front, false when another program is in front
bool Recording::front_rect(R4& out) const {
    HWND fg = GetForegroundWindow();
    if (!fg || !pids_.count(process_of(fg)) || is_minimized(fg)) return false;
    R4 r = window_rect(fg);
    if (r.w <= 0 || r.h <= 0) return false;
    if (std::find(apps_.begin(), apps_.end(), fg) == apps_.end()) {      // a dialog, a menu of the application: shown with its main window
        for (HWND hw : apps_) {
            if (process_of(hw) == process_of(fg) && !is_minimized(hw) && IsWindow(hw)) {
                R4 m = window_rect(hw);
                if (m.w <= 0 || m.h <= 0) continue;
                int x2 = std::max(r.x + r.w, m.x + m.w), y2 = std::max(r.y + r.h, m.y + m.h);
                r.x = std::min(r.x, m.x);
                r.y = std::min(r.y, m.y);
                r.w = x2 - r.x;
                r.h = y2 - r.y;
                break;
            }
        }
    }
    int x1 = std::max(r.x, area_.x) - area_.x, y1 = std::max(r.y, area_.y) - area_.y;
    int x2 = std::min(r.x + r.w, area_.x + area_.w) - area_.x, y2 = std::min(r.y + r.h, area_.y + area_.h) - area_.y;
    if (x2 - x1 < 16 || y2 - y1 < 16) return false;
    out = {x1, y1, x2 - x1, y2 - y1};
    return true;
}

void Recording::follow() {
    int last = -1;                                             // -1 nothing logged yet, 0 another program in front, 1 a rectangle
    R4 last_r;
    while (!stop_follow_) {
        R4 cur;
        bool has = front_rect(cur);
        bool same = (last >= 0) && ((!has && last == 0) ||
                                    (has && last == 1 && abs(cur.x - last_r.x) < 4 && abs(cur.y - last_r.y) < 4 && abs(cur.w - last_r.w) < 4 &&
                                     abs(cur.h - last_r.h) < 4));
        if (!same) {
            std::lock_guard<std::mutex> lk(mu_);
            log_.push_back({now(), has, cur});
            last = has ? 1 : 0;
            last_r = cur;
        }
        for (int i = 0; i < 10 && !stop_follow_; ++i) Sleep(10);
    }
}

// apps: [(start, rectangle, black)] from the log of the window in front, in seconds of the video; empty when there is nothing to cut
vector<Seg> Recording::segments() const {
    double tv = t_cap0_ >= 0 ? t_cap0_ : (double)t_video_;
    if (tv < 0) return {};
    vector<LogEntry> lg;
    {
        std::lock_guard<std::mutex> lk(mu_);
        lg = log_;
    }
    if (lg.empty()) return {{0.0, {0, 0, win_size.w, win_size.h}, false}};
    struct Pt {
        double t;
        bool has;
        R4 r;
    };
    auto same = [](const Pt& a, const Pt& b) {
        return a.has == b.has && (!a.has || (a.r.x == b.r.x && a.r.y == b.r.y && a.r.w == b.r.w && a.r.h == b.r.h));
    };
    vector<Pt> pts;
    for (auto& e : lg) pts.push_back({std::max(0.0, e.t - tv), e.has, e.r});
    pts[0].t = 0.0;
    double gap = 0.15;
    vector<Pt> out;
    while (true) {
        out.clear();
        for (auto& p : pts) {
            if (!out.empty() && p.t - out.back().t < gap) {
                out.back().has = p.has;
                out.back().r = p.r;
            } else if (out.empty() || !same(out.back(), p)) {
                out.push_back(p);
            }
        }
        if (out.size() <= 150) break;
        gap *= 2;
        pts = out;
    }
    vector<Seg> segs;
    bool have_prev = false;
    R4 prev;
    for (auto& p : out) {
        if (!p.has) {
            R4 r = have_prev ? prev : R4{0, 0, win_size.w, win_size.h};
            segs.push_back({p.t, r, true});
        } else {
            prev = p.r;
            have_prev = true;
            segs.push_back({p.t, p.r, false});
        }
    }
    return segs;
}

// apps: the filters that cut the recorded screen to the application in front ([0:v] to [vm])
string Recording::app_parts(int fps_, const vector<Seg>& segs) const {
    int ow = out_w, oh = out_h;
    size_t n = segs.size();
    vector<string> parts;
    if (n > 1) {
        string s = F("[0:v:0]split=%d", (int)n);
        for (size_t i = 0; i < n; ++i) s += F("[s%d]", (int)i);
        parts.push_back(s);
    } else {
        parts.push_back("[0:v:0]null[s0]");
    }
    for (size_t i = 0; i < n; ++i) {
        double a = segs[i].t;
        R4 r = segs[i].r;
        int cw = std::max(2, r.w / 2 * 2), ch = std::max(2, r.h / 2 * 2);
        string cut = F("trim=start=%.3f", a);
        if (i + 1 < n) cut += F(":end=%.3f", segs[i + 1].t);
        cut += ",setpts=PTS-STARTPTS,";
        string hold = segs[i].black ? ",drawbox=x=0:y=0:w=iw:h=ih:color=black:t=fill" : "";        // another program in front: black
        parts.push_back(F("[s%d]%scrop=%d:%d:%d:%d,scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos,pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black,"
                          "setsar=1,fps=%d,format=yuv420p%s[g%d]", (int)i, cut.c_str(), cw, ch, r.x, r.y, ow, oh, ow, oh, fps_, hold.c_str(), (int)i));
    }
    string cat;
    for (size_t i = 0; i < n; ++i) cat += F("[g%d]", (int)i);
    if (has_cam()) {
        int cw = std::max(2, std::min(480, std::min(std::max(160, ow / 5), ow / 2))) / 2 * 2;
        parts.push_back(F("%sconcat=n=%d:v=1:a=0[vm0]", cat.c_str(), (int)n));
        parts.push_back(F("[0:v:1]scale=%d:-2,fps=%d,format=yuv420p[cam]", cw, fps_));
        parts.push_back(F("[vm0][cam]overlay=%s:eof_action=repeat,format=yuv420p[vm]", corner_position(corner_).c_str()));
    } else {
        parts.push_back(F("%sconcat=n=%d:v=1:a=0[vm]", cat.c_str(), (int)n));
    }
    string all;
    for (size_t i = 0; i < parts.size(); ++i) all += (i ? ";" : "") + parts[i];
    return all;
}

void Recording::abort() {
    stop_follow_ = true;
    close_proc(proc_, watch_th_, err_th_, true);
    if (follow_th_.joinable()) follow_th_.join();
    if (audio) audio->stop();
    if (mic) mic->stop();
    std::error_code ec;
    if (!dir.empty()) fs::remove_all(dir, ec);
}

// ends the recording and writes the MP4 in the folder: returns its path (empty, with the error, when it failed)
wstring Recording::stop_and_save() {
    stop_follow_ = true;
    double t_stop = now();
    if (t_video_ >= 0) length_ = t_stop - t_video_;
    if (proc_.in) {
        DWORD n;
        WriteFile(proc_.in, "q", 1, &n, nullptr);
    }
    if (proc_.process && WaitForSingleObject(proc_.process, 20000) != WAIT_OBJECT_0) TerminateProcess(proc_.process, 1);
    close_proc(proc_, watch_th_, err_th_, true);
    if (follow_th_.joinable()) follow_th_.join();
    if (audio) audio->stop();
    if (mic) mic->stop();
    std::error_code ec;
    vector<fs::path> parts = video_parts();
    while (!parts.empty() && (!fs::is_regular_file(parts.back(), ec) || fs::file_size(parts.back(), ec) == 0)) parts.pop_back();      // a piece cut short by the stop
    if (parts.empty()) {
        if (get_error().empty()) set_error(L"nothing was recorded");
        fs::remove_all(dir, ec);
        return L"";
    }
    // The first picture was captured earlier than ffmpeg says (its progress comes with the delay of the encoder: up to two seconds). The video
    // ends when the recording was stopped, so it began its own length before: that is the clock of the windows log and of the sound.
    double vlen = 0;
    for (auto& p : parts) vlen += media_duration(ffmpeg_, p);
    if (vlen > 0) {
        t_cap0_ = t_stop - vlen;
        length_ = vlen;
    } else if (t_video_ >= 0) {
        t_cap0_ = t_video_;
    }
    vector<Seg> segs;
    if (target.kind == Target::APPS) segs = segments();
    fs::create_directories(out_dir_, ec);
    time_t tt = time(nullptr);
    tm lt;
    localtime_s(&lt, &tt);
    char stamp[64];
    strftime(stamp, sizeof stamp, "Capture-%Y%m%d-%H%M%S.mp4", &lt);
    fs::path out = out_dir_ / W(stamp);
    for (int k = 2; fs::exists(out, ec) && k < 1000; ++k) out = out_dir_ / (fs::path(W(stamp)).stem().wstring() + L"-" + std::to_wstring(k) + L".mp4");
    fs::path main = dir / L"main.mp4";
    // the sounds that were heard (the PC, the microphone): joined, mixed when there are two
    vector<AudioTrack*> tracks;
    for (AudioTrack* a : {audio.get(), mic.get()})
        if (a && a->t0 >= 0 && a->peak > 0 && fs::is_regular_file(a->path, ec)) tracks.push_back(a);
    bool has_audio = !tracks.empty();
    vector<wstring> cmd = {ffmpeg_};
    add(cmd, {"-hide_banner", "-loglevel", "error", "-y"});
    if (parts.size() == 1) {
        add(cmd, {"-i"});
        cmd.push_back(parts[0].wstring());
    } else {                                                   // several pieces: put end to end
        string list;
        for (auto& p : parts) list += "file '" + U(p.filename().wstring()) + "'\n";                 // relative to the list, which is in the same folder
        fs::path lp = dir / L"parts.txt";
        write_text_file(lp, list);
        add(cmd, {"-f", "concat", "-safe", "0", "-i"});
        cmd.push_back(lp.wstring());
    }
    for (AudioTrack* a : tracks) {
        double lead = (t_cap0_ >= 0 ? t_cap0_ : (double)t_video_) - a->t0;      // > 0: the sound began before the first picture: skip that start
        if (lead > 0) add(cmd, {"-ss", F("%.3f", lead)});
        else add(cmd, {"-itsoffset", F("%.3f", -lead)});
        cmd.push_back(L"-i");
        cmd.push_back(a->path.wstring());
    }
    if (tracks.size() == 1) {
        add(cmd, {"-map", "0:v", "-map", "1:a", "-c:v", "copy", "-c:a", "aac", "-b:a", "192k", "-shortest"});
    } else if (tracks.size() == 2) {
        add(cmd, {"-filter_complex",
                  "[1:a]aresample=48000,aformat=channel_layouts=stereo[s];[2:a]aresample=48000,aformat=channel_layouts=stereo[m];"
                  "[s][m]amix=inputs=2:duration=longest:normalize=0,alimiter=limit=0.95[a]",
                  "-map", "0:v", "-map", "[a]", "-c:v", "copy", "-c:a", "aac", "-b:a", "192k", "-shortest"});
    } else {
        add(cmd, {"-map", "0:v", "-c:v", "copy"});
    }
    cmd.push_back(main.wstring());
    string so, se;
    int rc = run_capture(cmd, so, se, INFINITE);
    if (rc == 0 && fs::is_regular_file(main, ec)) {
        string first = pick_encoder(ffmpeg_);
        vector<string> keys = {first};
        if (first != "cpu") keys.push_back("cpu");             // the graphics card fails here: the processor does it
        for (auto& key : keys) {
            final_encoder = key;
            rc = run_capture(final_cmd(main, out, has_audio, key, segs), so, se, INFINITE);
            if (rc == 0 && fs::is_regular_file(out, ec)) break;
            fs::remove(out, ec);
        }
    }
    bool failed = rc != 0 || !fs::is_regular_file(out, ec);
    if (failed && fs::is_regular_file(main, ec)) {             // the recording is kept, not thrown away with the temporary files
        fs::path keep = dir.parent_path() / (L"grabber_kept_" + W(stamp).substr(8, 15));
        std::error_code e2;
        fs::rename(dir, keep, e2);
        kept_ = e2 ? dir.wstring() : keep.wstring();
    } else {
        fs::remove_all(dir, ec);
    }
    if (failed) {
        auto lines = split_lines(se);
        wstring last = L"ffmpeg failed";
        for (size_t i = lines.size(); i-- > 0;)
            if (!trim(lines[i]).empty()) {
                last = trim(lines[i]);
                break;
            }
        set_error(kept_.empty() ? last : last + L"  (the recording is kept in " + kept_ + L")");
        return L"";
    }
    return out.wstring();
}

// the last encode: size and quality of the chosen compression, the intro and the outro put around the recording
vector<wstring> Recording::final_cmd(const fs::path& main, const fs::path& out, bool has_audio, const string& enc, const vector<Seg>& segs) const {
    const Profile& P = profile(quality_);
    int f = std::min(fps, P.fps);
    int ow = out_w, oh = out_h;
    string vmain = F("fps=%d,scale=%d:%d:flags=lanczos,setsar=1,format=yuv420p", f, ow, oh);
    vector<wstring> cmd = {ffmpeg_};
    add(cmd, {"-hide_banner", "-loglevel", "error", "-y", "-i"});
    cmd.push_back(main.wstring());
    std::error_code ec;
    bool intro_on = !intro.first.empty() && fs::is_regular_file(intro.first, ec) && intro.second > 0;
    bool outro_on = !outro.first.empty() && fs::is_regular_file(outro.first, ec) && outro.second > 0;
    if (!intro_on && !outro_on && segs.empty()) {
        add(cmd, {"-vf", vmain, "-map", "0:v"});
        if (has_audio) add(cmd, {"-map", "0:a"});
    } else {
        vector<string> parts, sg;
        int k = 1;
        auto card = [&](const std::pair<wstring, double>& c, const char* tag) {
            add(cmd, {"-loop", "1", "-framerate", std::to_string(f), "-t", F("%.2f", c.second), "-i"});
            cmd.push_back(c.first);
            parts.push_back(F("[%d:v]fps=%d,scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos,pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black,"
                              "setsar=1,format=yuv420p,setpts=PTS-STARTPTS[v%s]", k, f, ow, oh, ow, oh, tag));
            string seg = F("[v%s]", tag);
            if (has_audio) {
                parts.push_back(F("anullsrc=r=48000:cl=stereo,atrim=duration=%.2f,asetpts=PTS-STARTPTS[a%s]", c.second, tag));
                seg += F("[a%s]", tag);
            }
            sg.push_back(seg);
            ++k;
        };
        if (intro_on) card(intro, "i");
        if (!segs.empty()) parts.push_back(app_parts(f, segs));
        else parts.push_back("[0:v]" + vmain + ",setpts=PTS-STARTPTS[vm]");
        sg.push_back("[vm]");
        if (has_audio) {
            parts.push_back("[0:a]aresample=48000,aformat=channel_layouts=stereo,asetpts=PTS-STARTPTS[am]");
            sg.back() += "[am]";
        }
        if (outro_on) card(outro, "o");
        string cat;
        for (auto& s : sg) cat += s;
        parts.push_back(F("%sconcat=n=%d:v=1:a=%d[v]%s", cat.c_str(), (int)sg.size(), has_audio ? 1 : 0, has_audio ? "[a]" : ""));
        string all;
        for (size_t i = 0; i < parts.size(); ++i) all += (i ? ";" : "") + parts[i];
        if (all.size() < 20000) {                              // a command line may not be longer than about 32000 characters
            add(cmd, {"-filter_complex", all, "-map", "[v]"});
        } else {                                               // a long graph (many changes of window) goes through a file (ffmpeg 7 and later)
            fs::path script = dir / (L"graph_" + W(enc) + L".txt");
            write_text_file(script, all);
            add(cmd, {"-/filter_complex"});
            cmd.push_back(script.wstring());
            add(cmd, {"-map", "[v]"});
        }
        if (has_audio) add(cmd, {"-map", "[a]"});
    }
    add(cmd, video_args(enc, P.crf, P.preset, P.maxrate, P.buf, false));
    add(cmd, {"-pix_fmt", pix_fmt(enc)});
    if (has_audio) add(cmd, {"-c:a", "aac", "-b:a", P.abr});
    add(cmd, {"-movflags", "+faststart"});
    cmd.push_back(out.wstring());
    return cmd;
}

// the temporary folders of the recordings whose program no longer runs (a crash, a power cut); the ones that keep a recording are named grabber_kept_*
void purge_stale_temp() {
    wchar_t t[MAX_PATH];
    GetTempPathW(MAX_PATH, t);
    std::error_code ec;
    for (auto& e : fs::directory_iterator(t, ec)) {
        wstring n = e.path().filename().wstring();
        if (n.rfind(L"grabber", 0) != 0 || n.size() < 9 || !iswdigit(n[7]) || !e.is_directory(ec)) continue;
        DWORD pid = (DWORD)_wtoi(n.c_str() + 7);
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        bool running = h && WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
        if (h) CloseHandle(h);
        if (!running) fs::remove_all(e.path(), ec);
    }
}
