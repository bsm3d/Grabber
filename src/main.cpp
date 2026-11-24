// Grabber
// © Benoit Saint-Moulin - 2026
// Version du fichier : 1.2.0
// Fonction du fichier : Entry point of Grabber: handles the command line test modes, otherwise starts the main window.

// Command line, for tests (otherwise the program is a window):
// Grabber.exe --shot file.png                 draws the window into a PNG and quits
#include "grabber.h"
#include "ui.h"

#include <objbase.h>
#include <mmsystem.h>
#include <shellapi.h>

static void progress(const fs::path& dir, const string& s) {
    FILE* f = _wfopen((dir / L"progress.txt").c_str(), L"ab");
    if (!f) return;
    string l = F("%.2f ", now()) + s + "\n";
    fwrite(l.data(), 1, l.size(), f);
    fclose(f);
}

static int rec_test(const vector<wstring>& a) {
    // a: outdir seconds x y w h [sound] [cam]
    fs::path out = a[0];
    std::error_code ec;
    fs::create_directories(out, ec);
    string log;
    wstring ff = find_ffmpeg();
    log += "ffmpeg: " + U(ff) + "\n";
    { string o, e; int rc = run_capture({ff, L"-hide_banner", L"-encoders"}, o, e, 15000); log += F("encoders rc %d out %d err %d has_mf %d\n", rc, (int)o.size(), (int)e.size(), (int)(o.find("h264_mf") != string::npos)); }
    { string o, e; int rc = run_capture({ff, L"-hide_banner", L"-loglevel", L"error", L"-f", L"lavfi", L"-i", L"testsrc=size=1280x720:rate=30", L"-frames:v", L"15", L"-pix_fmt", L"nv12", L"-c:v", L"h264_mf", L"-hw_encoding", L"1", L"-rate_control", L"quality", L"-quality", L"68", L"-f", L"null", L"-"}, o, e, 30000); log += F("mf rc %d err [%s]\n", rc, e.c_str()); }
    double t_hw = now();
    auto hw = hardware_encoders(ff);
    log += F("hw probe %.2f s, %d found\n", now() - t_hw, (int)hw.size());
    for (auto& h : hw) log += "hardware: " + h.key + "\n";
    Target t;
    t.kind = Target::REGION;
    t.r = {_wtoi(a[2].c_str()), _wtoi(a[3].c_str()), _wtoi(a[4].c_str()), _wtoi(a[5].c_str())};
    bool sound = false, use_mic = false;
    wstring cam;
    for (size_t i = 6; i < a.size(); ++i) {
        if (a[i] == L"sound") sound = true;
        else if (a[i] == L"mic") use_mic = true;
        else if (a[i].rfind(L"cam:", 0) == 0) cam = a[i].substr(4);
    }
    Recording rec(ff, t, out, sound, true, "web", cam, "br", use_mic, L"");
    progress(out, "starting");
    bool ok = rec.start();
    progress(out, "started");
    log += F("start: %d  size %dx%d  out %dx%d  live encoder %s\n", ok, rec.w, rec.h, rec.out_w, rec.out_h, rec.live_encoder.c_str());
    if (!ok) {
        log += "error: " + U(rec.get_error()) + "\n";
    } else {
        Sleep((DWORD)(_wtof(a[1].c_str()) * 1000));
        progress(out, "stopping");
        wstring path = rec.stop_and_save();
        progress(out, "stopped");
        log += "saved: " + U(path) + "\nerror: " + U(rec.get_error()) + "\nfinal encoder " + rec.final_encoder + "\n";
        if (rec.audio) log += F("audio ok %d peak %d err %s\n", rec.audio->ok, rec.audio->peak.load(), U(rec.audio->error).c_str());
        if (rec.mic) log += F("mic ok %d err %s\n", rec.mic->ok, U(rec.mic->error).c_str());
    }
    write_text_file(out / L"result.txt", log);
    return ok ? 0 : 2;
}

// --apps-test outdir titleA titleB titleC : A and B are recorded, C is not; the front window goes A, C, B, A (2 s, 2 s, 2 s, 1 s), with the camera and an intro
static int apps_test(const vector<wstring>& a) {
    fs::path out = a[0];
    std::error_code ec;
    fs::create_directories(out, ec);
    string log;
    wstring ff = find_ffmpeg();
    HWND ha = nullptr, hb = nullptr, hc = nullptr;
    for (auto& w : windows(nullptr)) {
        if (w.title == a[1]) ha = w.hwnd;
        if (w.title == a[2]) hb = w.hwnd;
        if (w.title == a[3]) hc = w.hwnd;
    }
    if (!ha || !hb || !hc) {
        write_text_file(out / L"result.txt", "windows not found\n");
        return 3;
    }
    Target t;
    t.kind = Target::APPS;
    t.apps = {{ha, a[1]}, {hb, a[2]}};
    Recording rec(ff, t, out, false, false, "web", L"test:1", "tr", false, L"");
    bool ok = rec.start();
    log += F("start: %d  area %dx%d  out %dx%d\n", ok, rec.w, rec.h, rec.out_w, rec.out_h);
    if (ok) {
        Card c;
        c.text = "INTRO";
        fs::path ip = rec.dir / L"intro.png";
        save_card(ip, rec.out_w, rec.out_h, c);
        rec.intro = std::make_pair(ip.wstring(), 2.0);
        struct Step { HWND h; int ms; } steps[4] = {{ha, 2000}, {hc, 2000}, {hb, 2000}, {ha, 1000}};
        for (auto& s : steps) {
            bring_to_front(s.h);
            Sleep(s.ms);
        }
        wstring path = rec.stop_and_save();
        log += "saved: " + U(path) + "\nerror: " + U(rec.get_error()) + "\n";
    } else {
        log += "error: " + U(rec.get_error()) + "\n";
    }
    write_text_file(out / L"result.txt", log);
    return ok ? 0 : 2;
}

// --sync-test outdir tone.wav : a window of ours turns white and a tone starts in the same moment, both are recorded (zone and sound of the PC)
static int sync_test(const vector<wstring>& a) {
    fs::path out = a[0];
    std::error_code ec;
    fs::create_directories(out, ec);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"GrabberSyncTest";
    wc.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP | WS_VISIBLE, 300, 200, 640, 360, nullptr, nullptr, inst, nullptr);
    auto pump = [&](double sec) {
        double end = now() + sec;
        while (now() < end) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
            Sleep(2);
        }
    };
    pump(0.6);
    Target t;
    t.kind = Target::REGION;
    t.r = {300, 200, 640, 360};
    Recording rec(find_ffmpeg(), t, out, true, false, "web", L"", "br", false, L"");
    string log;
    bool ok = rec.start();
    if (ok) {
        pump(2.0);
        SetClassLongPtrW(h, GCLP_HBRBACKGROUND, (LONG_PTR)CreateSolidBrush(RGB(255, 255, 255)));
        InvalidateRect(h, nullptr, TRUE);
        UpdateWindow(h);
        PlaySoundW(a[1].c_str(), nullptr, SND_FILENAME | SND_ASYNC);
        pump(3.0);
        wstring path = rec.stop_and_save();
        log += "saved: " + U(path) + "\nerror: " + U(rec.get_error()) + "\n";
    } else {
        log += "error: " + U(rec.get_error()) + "\n";
    }
    DestroyWindow(h);
    write_text_file(out / L"result.txt", log);
    return ok ? 0 : 2;
}
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    vector<wstring> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    LocalFree(argv);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    gdip_start();
    int code = 0;
    if (args.size() >= 7 && args[0] == L"--rec-test") {
        code = rec_test(vector<wstring>(args.begin() + 1, args.end()));
    } else if (args.size() >= 2 && args[0] == L"--picker-test") {
        code = picker_test(args[1]);
    } else if (args.size() >= 3 && args[0] == L"--sync-test") {
        code = sync_test(vector<wstring>(args.begin() + 1, args.end()));
    } else if (args.size() >= 5 && args[0] == L"--apps-test") {
        code = apps_test(vector<wstring>(args.begin() + 1, args.end()));
    } else {
        code = run_app(inst, show, args.size() >= 2 && args[0] == L"--shot" ? args[1] : L"");
    }
    gdip_stop();
    CoUninitialize();
    return code;
}
