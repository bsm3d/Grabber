// Grabber
// © Benoit Saint-Moulin - 2026
// Version du fichier : 1.2.0
// Fonction du fichier : Small helpers for strings (UTF-8 and UTF-16), formatting, text file writing, JSON and INI settings, and processes.

#include "grabber.h"

#include <chrono>
#include <cstdlib>

// ------------------------------------------------------------------------------------------------ strings
wstring W(const string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

string U(const wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &u[0], n, nullptr, nullptr);
    return u;
}

string F(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

wstring quote_arg(const wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == wstring::npos) return a;
    wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++bs;
        } else if (c == L'"') {
            r.append(bs * 2 + 1, L'\\');
            r += L'"';
            bs = 0;
        } else {
            r.append(bs, L'\\');
            bs = 0;
            r += c;
        }
    }
    r.append(bs * 2, L'\\');
    r += L'"';
    return r;
}

wstring trim(const wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

vector<wstring> split_lines(const string& utf8) {
    vector<wstring> out;
    size_t i = 0;
    while (i <= utf8.size()) {
        size_t j = utf8.find('\n', i);
        if (j == string::npos) j = utf8.size();
        string line = utf8.substr(i, j - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(W(line));
        i = j + 1;
    }
    return out;
}

void write_text_file(const fs::path& p, const string& utf8) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    fwrite(utf8.data(), 1, utf8.size(), f);
    fclose(f);
    MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool read_text_file(const fs::path& p, string& out) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) out.erase(0, 3);
    return true;
}

// ------------------------------------------------------------------------------------------------ JSON
const JV* JV::get(const string& k) const {
    for (auto& kv : o)
        if (kv.first == k) return &kv.second;
    return nullptr;
}

void JV::set(const string& k, const JV& v) {
    t = OBJ;
    for (auto& kv : o)
        if (kv.first == k) {
            kv.second = v;
            return;
        }
    o.emplace_back(k, v);
}

namespace {
struct Parser {
    const string& s;
    size_t i = 0;
    explicit Parser(const string& str) : s(str) {}
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    }
    static void put_utf8(string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    bool str(string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    unsigned cp = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        unsigned lo = (unsigned)strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                        if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: out += e;
            }
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    }
    bool value(JV& v, int depth = 0) {
        if (depth > 16) return false;
        ws();
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '{') {
            ++i;
            v = JV::obj();
            ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            while (true) {
                ws();
                string k;
                if (!str(k)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return false;
                ++i;
                JV item;
                if (!value(item, depth + 1)) return false;
                v.o.emplace_back(k, item);
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return false;
            }
        }
        if (c == '[') {                                        // arrays are not used: read and dropped
            ++i;
            ws();
            if (i < s.size() && s[i] == ']') { ++i; v = JV(); return true; }
            while (true) {
                JV item;
                if (!value(item, depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; v = JV(); return true; }
                return false;
            }
        }
        if (c == '"') {
            v.t = JV::STR;
            return str(v.s);
        }
        if (s.compare(i, 4, "true") == 0) { v = JV::boolean(true); i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { v = JV::boolean(false); i += 5; return true; }
        if (s.compare(i, 4, "null") == 0) { v = JV(); i += 4; return true; }
        char* end = nullptr;
        double d = strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) return false;
        i = end - s.c_str();
        v = JV::num(d);
        return true;
    }
};

void dump_str(string& out, const string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) out += F("\\u%04x", c);
                else out += (char)c;
        }
    }
    out += '"';
}

void dump(string& out, const JV& v, int indent, int level) {
    switch (v.t) {
        case JV::NUL: out += "null"; break;
        case JV::BOOL: out += v.b ? "true" : "false"; break;
        case JV::NUM:
            if (v.n == (long long)v.n) out += std::to_string((long long)v.n);
            else out += F("%g", v.n);
            break;
        case JV::STR: dump_str(out, v.s); break;
        case JV::OBJ: {
            if (v.o.empty()) { out += "{}"; break; }
            out += '{';
            bool first = true;
            for (auto& kv : v.o) {
                if (!first) out += ',';
                first = false;
                if (indent) out += '\n' + string((level + 1) * indent, ' ');
                dump_str(out, kv.first);
                out += indent ? ": " : ":";
                dump(out, kv.second, indent, level + 1);
            }
            if (indent) out += '\n' + string(level * indent, ' ');
            out += '}';
            break;
        }
    }
}
}  // namespace

bool json_parse(const string& text, JV& out) {
    Parser p(text);
    if (!p.value(out)) return false;
    p.ws();
    return true;
}

string json_dump(const JV& v, int indent) {
    string out;
    dump(out, v, indent, 0);
    return out;
}

// ------------------------------------------------------------------------------------------------ settings
static string esc(const string& s) {
    string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    return o;
}

static string unesc(const string& s) {
    string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            o += n == 'n' ? '\n' : n == 'r' ? '\r' : n;
        } else {
            o += s[i];
        }
    }
    return o;
}

void Ini::load() {
    kv.clear();
    string text;
    if (!read_text_file(path, text)) return;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find('\n', i);
        if (j == string::npos) j = text.size();
        string line = text.substr(i, j - i);
        i = j + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '[' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == string::npos) continue;
        kv[line.substr(0, eq)] = unesc(line.substr(eq + 1));
    }
}

void Ini::save() const {
    string t = "[General]\r\n";
    for (auto& e : kv) t += e.first + "=" + esc(e.second) + "\r\n";
    write_text_file(path, t);
}

string Ini::get(const string& k, const string& def) const {
    auto it = kv.find(k);
    return it == kv.end() ? def : it->second;
}

bool Ini::get_bool(const string& k, bool def) const {
    auto it = kv.find(k);
    if (it == kv.end()) return def;
    return it->second == "true" || it->second == "1";
}

int Ini::get_int(const string& k, int def) const {
    auto it = kv.find(k);
    if (it == kv.end()) return def;
    return atoi(it->second.c_str());
}

// ------------------------------------------------------------------------------------------------ processes
bool spawn(const vector<wstring>& args, Proc& p, bool pipe_in, bool pipe_out, bool pipe_err) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    auto pipe = [&](HANDLE& r, HANDLE& w, bool child_reads) {
        if (!CreatePipe(&r, &w, &sa, 1 << 16)) return false;
        SetHandleInformation(child_reads ? w : r, HANDLE_FLAG_INHERIT, 0);          // the parent's end is not inherited
        return true;
    };
    bool ok = true;
    if (pipe_in) ok = ok && pipe(in_r, in_w, true);
    if (pipe_out) ok = ok && pipe(out_r, out_w, false);
    if (pipe_err) ok = ok && pipe(err_r, err_w, false);
    wstring cmd;
    for (size_t i = 0; i < args.size(); ++i) cmd += (i ? L" " : L"") + quote_arg(args[i]);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = pipe_in ? in_r : nul;
    si.hStdOutput = pipe_out ? out_w : nul;
    si.hStdError = pipe_err ? err_w : nul;
    PROCESS_INFORMATION pi{};
    // only these three handles go to the child: a program started at the same time by another thread must not inherit the pipes
    HANDLE inherit[3] = {si.hStdInput, si.hStdOutput, si.hStdError};
    int ninherit = 0;
    HANDLE uniq[3];
    for (HANDLE h : inherit) {
        bool dup = false;
        for (int i = 0; i < ninherit; ++i) dup = dup || uniq[i] == h;
        if (!dup) uniq[ninherit++] = h;
    }
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &asz);
    vector<char> abuf(asz);
    LPPROC_THREAD_ATTRIBUTE_LIST al = (LPPROC_THREAD_ATTRIBUTE_LIST)abuf.data();
    bool al_ok = InitializeProcThreadAttributeList(al, 1, 0, &asz) &&
                 UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, uniq, ninherit * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW sx{};
    sx.StartupInfo = si;
    sx.StartupInfo.cb = sizeof sx;
    sx.lpAttributeList = al;
    BOOL created = ok && al_ok && CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                                 nullptr, &sx.StartupInfo, &pi);
    if (al_ok) DeleteProcThreadAttributeList(al);
    for (HANDLE h : {in_r, out_w, err_w, nul})
        if (h) CloseHandle(h);
    if (!created) {
        for (HANDLE h : {in_w, out_r, err_r})
            if (h) CloseHandle(h);
        return false;
    }
    CloseHandle(pi.hThread);
    p.process = pi.hProcess;
    p.in = in_w;
    p.out = out_r;
    p.err = err_r;
    return true;
}

static void drain(HANDLE h, string& into) {
    char buf[8192];
    DWORD n;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) into.append(buf, n);
}

int run_capture(const vector<wstring>& args, string& out, string& err, DWORD timeout_ms) {
    out.clear();
    err.clear();
    Proc p;
    if (!spawn(args, p, false, true, true)) return -1;
    std::thread t1(drain, p.out, std::ref(out)), t2(drain, p.err, std::ref(err));
    int code = -1;
    if (WaitForSingleObject(p.process, timeout_ms) == WAIT_OBJECT_0) {
        DWORD c = 0;
        GetExitCodeProcess(p.process, &c);
        code = (int)c;
    } else {
        TerminateProcess(p.process, 1);
        WaitForSingleObject(p.process, 5000);
    }
    t1.join();
    t2.join();
    CloseHandle(p.out);
    CloseHandle(p.err);
    CloseHandle(p.process);
    return code;
}
