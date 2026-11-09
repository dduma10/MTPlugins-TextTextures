// Platform.h: Windows plumbing for TextTextures: files, child processes, gdt.db and a fallback Qt text read.
#pragma once
#include <functional>
#include <string>

namespace tt
{
std::wstring Join(const std::wstring& a, const std::wstring& b);
std::wstring Utf8W(const std::string& s);
std::string WUtf8(const std::wstring& s);

bool ReadWholeFile(const std::wstring& path, std::string& out);
// Writes to "<path>.tttmp" then swaps it in, so a crash never leaves a half-written file. `backup` keeps the old
// file as "<path>.bak" first.
bool WriteFileAtomic(const std::wstring& path, const void* data, size_t size, bool backup, std::string& error);
bool EnsureDirectory(const std::wstring& dir);
bool FileExists(const std::wstring& path);

// Runs exe without a console window. Returns its exit code, -1 when it could not start, -2 when stopped or timed
// out. `tail` receives the end of its combined output.
int RunProcess(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd, const std::function<bool()>& stop,
               unsigned timeoutMs, std::string* tail);

// "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
std::string NewGuidText();

// The GDT (as gdt.db stores it, usually relative to the game root) that defines asset `name` of type `gdf`
// ("material", "image"). Returns false when gdt.db or BO3's sqlite DLL is unavailable; gdt is empty when no GDT does.
bool GdtDbFind(const std::wstring& root, const std::string& name, const char* gdf, std::string& gdt);

// Reads a QPlainTextEdit's text straight from Radiant's Qt (Qt5Widgets64r.dll). The host's ui->EditText must not be
// used on one: it calls QLineEdit::text without checking the class. false when those Qt exports are missing.
// Only call it on a widget made with ui->PlainTextEdit.
bool ReadPlainTextDirect(void* plainTextEdit, std::string& utf8);
// Radiant's Qt exports the two functions ReadPlainTextDirect needs.
bool PlainTextReadable();
} // namespace tt
