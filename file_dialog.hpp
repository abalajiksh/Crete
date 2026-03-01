#pragma once
// ============================================================================
// file_dialog.hpp — Portable native file/folder dialogs
//
// Linux:   zenity (GNOME) or kdialog (KDE) via popen
// macOS:   osascript (AppleScript)
// Windows: Win32 GetOpenFileName / IFileDialog
//
// No external library dependencies.
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace nfd {

// ── Helper: run a command and capture stdout ───────────────────────────────
#ifndef _WIN32
inline std::string exec_cmd(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    // Trim trailing newlines
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

inline bool has_command(const char* cmd) {
    std::string check = std::string("which ") + cmd + " >/dev/null 2>&1";
    return system(check.c_str()) == 0;
}
#endif

// ── Open file dialog ───────────────────────────────────────────────────────
inline std::string open_file(const char* title = "Select Audio File") {
#ifdef _WIN32
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrTitle = title;
    ofn.lpstrFilter =
        "Audio Files\0*.wav;*.flac;*.aif;*.aiff;*.dsf;*.dff\0"
        "All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return filename;
    return "";

#elif defined(__APPLE__)
    return exec_cmd(
        "osascript -e 'set f to POSIX path of (choose file with prompt \""
        + std::string(title) +
        "\" of type {\"wav\",\"flac\",\"aif\",\"aiff\",\"dsf\",\"dff\"})' 2>/dev/null");

#else
    // Linux: try zenity, then kdialog
    if (has_command("zenity")) {
        return exec_cmd(
            "zenity --file-selection"
            " --title='" + std::string(title) + "'"
            " --file-filter='Audio files|*.wav *.flac *.aif *.aiff *.dsf *.dff'"
            " --file-filter='All files|*'"
            " 2>/dev/null");
    }
    if (has_command("kdialog")) {
        return exec_cmd(
            "kdialog --getopenfilename ~ 'Audio files (*.wav *.flac *.aif *.aiff *.dsf *.dff)'"
            " 2>/dev/null");
    }
    return "";
#endif
}

// ── Open folder dialog ─────────────────────────────────────────────────────
inline std::string open_folder(const char* title = "Select Folder") {
#ifdef _WIN32
    BROWSEINFOA bi = {};
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl) {
        char path[MAX_PATH];
        SHGetPathFromIDListA(pidl, path);
        CoTaskMemFree(pidl);
        return path;
    }
    return "";

#elif defined(__APPLE__)
    return exec_cmd(
        "osascript -e 'set f to POSIX path of (choose folder with prompt \""
        + std::string(title) + "\")' 2>/dev/null");

#else
    if (has_command("zenity")) {
        return exec_cmd(
            "zenity --file-selection --directory"
            " --title='" + std::string(title) + "'"
            " 2>/dev/null");
    }
    if (has_command("kdialog")) {
        return exec_cmd(
            "kdialog --getexistingdirectory ~ --title '" + std::string(title) + "'"
            " 2>/dev/null");
    }
    return "";
#endif
}

} // namespace nfd
