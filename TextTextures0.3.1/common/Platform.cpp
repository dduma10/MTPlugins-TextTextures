// Platform.cpp: see Platform.h.
#include "Platform.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace tt
{
std::wstring Join(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring Utf8W(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string WUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool ReadWholeFile(const std::wstring& path, std::string& out)
{
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1ll << 31);
    if (ok)
    {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = size.QuadPart == 0 || (ReadFile(h, &out[0], (DWORD)size.QuadPart, &got, nullptr) && got == (DWORD)size.QuadPart);
    }
    CloseHandle(h);
    return ok;
}

bool FileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool EnsureDirectory(const std::wstring& dir)
{
    if (dir.empty()) return false;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    int r = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

bool WriteFileAtomic(const std::wstring& path, const void* data, size_t size, bool backup, std::string& error)
{
    std::wstring tmp = path + L".tttmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        error = "Could not write " + WUtf8(tmp) + " (Windows error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    DWORD wrote = 0;
    bool ok = size == 0 || (WriteFile(h, data, (DWORD)size, &wrote, nullptr) && wrote == size);
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (!ok)
    {
        DeleteFileW(tmp.c_str());
        error = "Could not write " + WUtf8(tmp) + " (disk full?).";
        return false;
    }
    if (backup && FileExists(path) && !CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE))
    {
        DeleteFileW(tmp.c_str());
        error = "Could not back up " + WUtf8(path) + "; it was left unchanged.";
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DWORD e = GetLastError();
        DeleteFileW(tmp.c_str());
        error = "Could not replace " + WUtf8(path) + " (Windows error " + std::to_string(e) + "; is it open in another program?).";
        return false;
    }
    return true;
}

int RunProcess(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd, const std::function<bool()>& stop,
               unsigned timeoutMs, std::string* tail)
{
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return -1;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    // Give the child only its pipe and NUL, not every inheritable handle Radiant owns.
    HANDLE handles[2] = {writeEnd, nul};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrBuf(attrSize);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    bool haveAttrs = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                     UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                               (nul != INVALID_HANDLE_VALUE ? 2 : 1) * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
    si.StartupInfo.hStdOutput = writeEnd;
    si.StartupInfo.hStdError = writeEnd;
    si.lpAttributeList = haveAttrs ? attrs : nullptr;
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW | (haveAttrs ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr,
                                  cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    if (haveAttrs) DeleteProcThreadAttributeList(attrs);
    CloseHandle(writeEnd);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started)
    {
        CloseHandle(readEnd);
        return -1;
    }
    CloseHandle(pi.hThread);
    std::string output;
    auto drain = [&] {
        DWORD avail = 0;
        while (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &avail, nullptr) && avail)
        {
            char buf[4096];
            DWORD got = 0;
            if (!ReadFile(readEnd, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || !got) break;
            output.append(buf, got);
            if (output.size() > 64 * 1024) output.erase(0, output.size() - 32 * 1024);
        }
    };
    ULONGLONG begin = GetTickCount64();
    int code = -2;
    for (;;)
    {
        DWORD w = WaitForSingleObject(pi.hProcess, 50);
        drain();
        if (w == WAIT_OBJECT_0)
        {
            DWORD c = 0;
            GetExitCodeProcess(pi.hProcess, &c);
            code = (int)c;
            break;
        }
        if ((stop && stop()) || GetTickCount64() - begin > timeoutMs || w == WAIT_FAILED)
        {
            TerminateProcess(pi.hProcess, 124);
            WaitForSingleObject(pi.hProcess, 5000);
            drain();
            code = -2;
            break;
        }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(readEnd);
    if (tail) *tail = output.size() > 2000 ? output.substr(output.size() - 2000) : output;
    return code;
}

std::string NewGuidText()
{
    GUID g{};
    if (FAILED(CoCreateGuid(&g)))
    {
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        g.Data1 = (unsigned long)t.LowPart ^ GetCurrentProcessId();
        g.Data2 = (unsigned short)(t.HighPart);
        g.Data3 = (unsigned short)(0x4000 | (GetTickCount() & 0x0FFF));
        for (int i = 0; i < 8; ++i) g.Data4[i] = (unsigned char)((t.QuadPart >> (i * 5)) ^ (i * 37));
    }
    char buf[64];
    snprintf(buf, sizeof buf, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0],
             g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

bool GdtDbFind(const std::wstring& root, const std::string& name, const char* gdf, std::string& gdt)
{
    gdt.clear();
    using OpenFn = int (*)(const char*, void**, int, const char*);
    using PrepareFn = int (*)(void*, const char*, int, void**, const char**);
    using BindFn = int (*)(void*, int, const char*, int, void (*)(void*));
    using StepFn = int (*)(void*);
    using ColumnFn = const unsigned char* (*)(void*, int);
    using FinalizeFn = int (*)(void*);
    using CloseFn = int (*)(void*);
    static HMODULE module = nullptr;
    if (!module) module = LoadLibraryW(Join(Join(root, L"bin"), L"sqlite64r.dll").c_str());
    if (!module) return false;
    auto open = reinterpret_cast<OpenFn>(GetProcAddress(module, "sqlite3_open_v2"));
    auto prepare = reinterpret_cast<PrepareFn>(GetProcAddress(module, "sqlite3_prepare_v2"));
    auto bind = reinterpret_cast<BindFn>(GetProcAddress(module, "sqlite3_bind_text"));
    auto step = reinterpret_cast<StepFn>(GetProcAddress(module, "sqlite3_step"));
    auto column = reinterpret_cast<ColumnFn>(GetProcAddress(module, "sqlite3_column_text"));
    auto finalize = reinterpret_cast<FinalizeFn>(GetProcAddress(module, "sqlite3_finalize"));
    auto close = reinterpret_cast<CloseFn>(GetProcAddress(module, "sqlite3_close"));
    if (!open || !prepare || !bind || !step || !column || !finalize || !close) return false;
    std::wstring dbPath = Join(Join(root, L"gdtdb"), L"gdt.db");
    if (!FileExists(dbPath)) return false;
    void* db = nullptr;
    if (open(WUtf8(dbPath).c_str(), &db, 0x00000001 /* SQLITE_OPEN_READONLY */, nullptr) != 0 || !db)
    {
        if (db) close(db);
        return false;
    }
    const char* sql = "SELECT t.name FROM _entity e JOIN _gdf g ON e.FK_gdf=g.PK_id JOIN _gdt t ON e.FK_gdt=t.PK_id "
                      "WHERE e.name=?1 COLLATE NOCASE AND g.name=?2";
    void* stmt = nullptr;
    bool ok = prepare(db, sql, -1, &stmt, nullptr) == 0 && stmt;
    if (ok)
    {
        bind(stmt, 1, name.c_str(), -1, nullptr);
        bind(stmt, 2, gdf, -1, nullptr);
        if (step(stmt) == 100 /* SQLITE_ROW */)
            if (const unsigned char* t = column(stmt, 0)) gdt = reinterpret_cast<const char*>(t);
        finalize(stmt);
    }
    close(db);
    return ok;
}

bool PlainTextReadable()
{
    HMODULE widgets = GetModuleHandleW(L"Qt5Widgets64r.dll");
    HMODULE core = GetModuleHandleW(L"Qt5Core64r.dll");
    return widgets && core && GetProcAddress(widgets, "?toPlainText@QPlainTextEdit@@QEBA?AVQString@@XZ") &&
           GetProcAddress(core, "?deallocate@QArrayData@@SAXPEAU1@_K1@Z");
}

bool ReadPlainTextDirect(void* edit, std::string& utf8)
{
    utf8.clear();
    HMODULE widgets = GetModuleHandleW(L"Qt5Widgets64r.dll");
    HMODULE core = GetModuleHandleW(L"Qt5Core64r.dll");
    if (!widgets || !core || !edit) return false;
    // QString QPlainTextEdit::toPlainText() const: MSVC x64 passes `this`, then the return slot.
    using ToPlainTextFn = void* (*)(const void*, void*);
    using DeallocateFn = void (*)(void*, size_t, size_t);
    auto toPlain = reinterpret_cast<ToPlainTextFn>(GetProcAddress(widgets, "?toPlainText@QPlainTextEdit@@QEBA?AVQString@@XZ"));
    auto dealloc = reinterpret_cast<DeallocateFn>(GetProcAddress(core, "?deallocate@QArrayData@@SAXPEAU1@_K1@Z"));
    if (!toPlain || !dealloc) return false;
    void* d = nullptr; // a QString is one pointer to its QArrayData
    toPlain(edit, &d);
    if (!d) return true;
    // Qt 5 QArrayData: int ref, int size, uint alloc:31 + capacityReserved:1, qptrdiff offset; UTF-16 at d + offset.
    auto* base = static_cast<unsigned char*>(d);
    volatile LONG* ref = reinterpret_cast<volatile LONG*>(base);
    int size = *reinterpret_cast<int*>(base + 4);
    ptrdiff_t offset = *reinterpret_cast<ptrdiff_t*>(base + 8 + 8);
    if (size > 0 && size < (1 << 24)) utf8 = WUtf8(std::wstring(reinterpret_cast<const wchar_t*>(base + offset), (size_t)size));
    // ~QString as Qt 5's RefCount::deref does it: -1 is static data (never freed), 0 is unsharable (freed now).
    LONG r = *ref;
    if (r == 0 || (r != -1 && InterlockedDecrement(ref) == 0)) dealloc(d, 2, 8);
    return true;
}
} // namespace tt
