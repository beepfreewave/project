#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include "memory.h"
#include <commctrl.h>
#include <string>
#include <vector>
#include <fstream>
#include <ctime>
#include <shlobj.h>

#pragma warning(disable: 28251)
#pragma warning(disable: 4996)

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------- Control IDs ----------
#define IDC_EDIT_PATTERN    1001
#define IDC_EDIT_READ       1002
#define IDC_EDIT_WRITE      1003
#define IDC_EDIT_PROCNAME   1004
#define IDC_CHK_AUTOLIST    1005
#define IDC_BTN_SCAN        1006
#define IDC_BTN_ENABLE      1007
#define IDC_BTN_DISABLE     1008
#define IDC_BTN_STOP        1009
#define IDC_BTN_CLEARLOG    1010
#define IDC_LOG             1011
#define IDC_STATUS          1012
#define IDC_LBL_FOUND       1013
#define IDC_BTN_PROC_LIST   1014
#define IDC_BTN_EXPORT      1015

// ---------- Globals ----------
HWND g_hPattern, g_hRead, g_hWrite, g_hProcName, g_hChkAutoList;
HWND g_hBtnScan, g_hBtnEnable, g_hBtnDisable, g_hBtnStop;
HWND g_hBtnClearLog, g_hBtnProcList, g_hBtnExport;
HWND g_hLog, g_hStatus, g_hFound;
HFONT g_hFont, g_hFontBold, g_hFontMono;

// ---------- Helpers ----------
static std::wstring ToW(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string ToA(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring GetText(HWND h)
{
    int len = GetWindowTextLengthW(h);
    std::wstring s(len, 0);
    GetWindowTextW(h, &s[0], len + 1);
    return s;
}

static void SetText(HWND h, const std::wstring& s) { SetWindowTextW(h, s.c_str()); }

static void Log(const std::wstring& msg)
{
    int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, len, len);
    std::wstring line = msg + L"\r\n";
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
}

static void SetStatus(const std::wstring& msg) { SetText(g_hStatus, msg); }

static void UpdateFoundLabel()
{
    size_t n = AimbotHeadScan::pairs.size();
    wchar_t buf[128];
    swprintf_s(buf, L"Pairs found: %zu   |   PID: %lu", n, AimbotHeadScan::pid);
    SetText(g_hFound, buf);
}

// ---------- Desktop path helper ----------
static std::wstring GetDesktopPath()
{
    // 1) SHGetFolderPath (handles moved Desktop or OneDrive)
    wchar_t buf[MAX_PATH] = { 0 };
    HRESULT hr = SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, buf);
    if (SUCCEEDED(hr) && buf[0] != 0)
    {
        std::wstring p = buf;
        if (!p.empty() && p.back() != L'\\' && p.back() != L'/')
            p += L"\\";
        return p;
    }

    // 2) USERPROFILE env variable + Desktop folder name
    wchar_t userProfile[MAX_PATH] = { 0 };
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", userProfile, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        std::wstring p = userProfile;
        p += L"\\Desktop\\";
        return p;
    }

    // 3) Fallback: folder with exe
    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring p = exePath;
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        p = p.substr(0, slash + 1);
    else
        p = L".\\";
    return p;
}

// ---------- Find latest exported file on Desktop ----------
static std::wstring FindLatestExportFile(const std::wstring& desktopPath)
{
    std::wstring searchMask = desktopPath + L"AimbotTest_*.txt";

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(searchMask.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return L"";

    std::wstring bestPath;
    ULARGE_INTEGER bestTime{};
    bestTime.QuadPart = 0;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        ULARGE_INTEGER ft;
        ft.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        ft.HighPart = fd.ftLastWriteTime.dwHighDateTime;

        if (ft.QuadPart > bestTime.QuadPart)
        {
            bestTime.QuadPart = ft.QuadPart;
            bestPath = desktopPath + fd.cFileName;
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return bestPath;
}

// ---------- Export TXT (smart: update existing or create new) ----------
static void DoExport()
{
    std::wstring wPat = GetText(g_hPattern);
    std::wstring wRead = GetText(g_hRead);
    std::wstring wWrite = GetText(g_hWrite);
    std::wstring wProc = GetText(g_hProcName);
    bool autoList = (SendMessageW(g_hChkAutoList, BM_GETCHECK, 0, 0) == BST_CHECKED);

    std::string pattern = ToA(wPat);
    auto trim = [](std::string& s) {
        while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
        while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
        };
    trim(pattern);

    if (pattern.empty())
    {
        Log(L"[!] Nothing to export: AOB pattern is empty.");
        SetStatus(L"Export canceled: empty pattern");
        return;
    }

    std::string readStr = ToA(wRead);
    std::string writeStr = ToA(wWrite);

    SYSTEMTIME st;
    GetLocalTime(&st);

    std::wstring desktop = GetDesktopPath();

    std::wstring existing = FindLatestExportFile(desktop);

    std::wstring fullPath;
    bool isUpdate = false;

    if (!existing.empty())
    {
        fullPath = existing;
        isUpdate = true;
    }
    else
    {
        wchar_t filename[128];
        swprintf_s(filename, L"AimbotTest_%04d-%02d-%02d_%02d-%02d.txt",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        fullPath = desktop + filename;
    }

    std::ios::openmode mode = isUpdate
        ? (std::ios::binary | std::ios::app)
        : (std::ios::binary | std::ios::trunc);

    std::ofstream f(ToA(fullPath).c_str(), mode);
    if (!f.is_open())
    {
        Log(L"[!] Cannot create file:");
        Log(std::wstring(L"    ") + fullPath);
        SetStatus(L"Export failed: cannot write file");
        return;
    }

    char timebuf[64];
    sprintf_s(timebuf, "%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    if (isUpdate)
    {
        f << "\r\n\r\n";
        f << "#####################################################\r\n";
        f << "# UPDATED EXPORT @ " << timebuf << "\r\n";
        f << "#####################################################\r\n\r\n";
    }

    f << "=====================================================\r\n";
    f << " AimbotTest AOB FF - Exported Config\r\n";
    f << "=====================================================\r\n";
    f << " Date: " << timebuf << "\r\n";
    f << " Pairs found: " << AimbotHeadScan::pairs.size() << "\r\n";
    f << " PID: " << AimbotHeadScan::pid << "\r\n";
    f << "=====================================================\r\n\r\n";

    f << "[AOB PATTERN]\r\n";
    f << pattern << "\r\n\r\n";

    f << "[OFFSETS]\r\n";
    f << "Read  = 0x" << readStr << "\r\n";
    f << "Write = 0x" << writeStr << "\r\n\r\n";

    f << "[PROCESS]\r\n";
    if (autoList)
    {
        f << "Mode = AUTO-LIST\r\n";
        f << "List:\r\n";
        for (const auto& n : AimbotHeadScan::DefaultProcessList())
            f << "  - " << ToA(n) << "\r\n";
    }
    else
    {
        f << "Mode = MANUAL\r\n";
        f << "Name = " << ToA(wProc) << "\r\n";
    }
    f << "\r\n";

    if (!AimbotHeadScan::pairs.empty())
    {
        f << "[FOUND ADDRESSES] (first 10)\r\n";
        size_t cnt = 0;
        for (const auto& p : AimbotHeadScan::pairs)
        {
            if (cnt++ >= 10) break;
            char line[128];
            sprintf_s(line, "  0x%016llX  read=%d write=%d",
                (unsigned long long)p.addr, p.origReadVal, p.origWriteVal);
            f << line << "\r\n";
        }
        f << "\r\n";
    }

    f << "=====================================================\r\n";
    f << " End of export\r\n";
    f << "=====================================================\r\n";

    f.close();

    if (isUpdate)
    {
        Log(L"[+] Updated existing export file:");
        Log(std::wstring(L"    ") + fullPath);
        SetStatus(L"Updated: " + std::wstring(fullPath.substr(desktop.size())));
    }
    else
    {
        Log(L"[+] Exported successfully to Desktop:");
        Log(std::wstring(L"    ") + fullPath);
        SetStatus(L"Saved: " + std::wstring(fullPath.substr(desktop.size())));
    }
}

// ---------- Actions ----------
static void DoScan()
{
    std::wstring wPat = GetText(g_hPattern);
    std::wstring wRead = GetText(g_hRead);
    std::wstring wWrite = GetText(g_hWrite);

    std::string pattern = ToA(wPat);
    auto trim = [](std::string& s) {
        while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
        while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
        };
    trim(pattern);

    if (pattern.empty())
    {
        Log(L"[!] Empty AOB pattern.");
        SetStatus(L"Error: empty pattern");
        return;
    }

    std::string readStr = ToA(wRead);
    std::string writeStr = ToA(wWrite);

    // trim offsets as well
    while (!readStr.empty() && isspace((unsigned char)readStr.front())) readStr.erase(readStr.begin());
    while (!readStr.empty() && isspace((unsigned char)readStr.back()))  readStr.pop_back();
    while (!writeStr.empty() && isspace((unsigned char)writeStr.front())) writeStr.erase(writeStr.begin());
    while (!writeStr.empty() && isspace((unsigned char)writeStr.back()))  writeStr.pop_back();

    if (readStr.empty() || writeStr.empty())
    {
        Log(L"[!] Read and Write offsets must be filled (HEX, e.g. B7 / B3).");
        SetStatus(L"Error: empty offset");
        return;
    }

    DWORD64 offRead = 0, offWrite = 0;
    try
    {
        offRead = std::stoull(readStr, nullptr, 16);
        offWrite = std::stoull(writeStr, nullptr, 16);
    }
    catch (...)
    {
        Log(L"[!] Invalid offset format (HEX required, e.g. B7 / B3).");
        SetStatus(L"Error: invalid offset");
        return;
    }

    AimbotHeadScan::cfg.pattern = pattern;
    AimbotHeadScan::cfg.offsetRead = offRead;
    AimbotHeadScan::cfg.offsetWrite = offWrite;

    std::wstring proc = GetText(g_hProcName);
    if (!proc.empty())
        AimbotHeadScan::cfg.processName = proc;

    bool autoList = (SendMessageW(g_hChkAutoList, BM_GETCHECK, 0, 0) == BST_CHECKED);
    AimbotHeadScan::cfg.useProcessList = autoList;

    Log(L"[*] Configuration updated:");
    {
        wchar_t buf[256];
        swprintf_s(buf, L"    Read=0x%llX  Write=0x%llX  ProcMode=%s",
            (unsigned long long)offRead,
            (unsigned long long)offWrite,
            autoList ? L"AUTO-LIST" : (proc.empty() ? L"AUTO-LIST(fallback)" : L"MANUAL"));
        Log(buf);
    }

    AimbotHeadScan::ResetPairs();

    Log(L"[*] Scanning memory...");
    SetStatus(L"Scanning...");

    bool ok = AimbotHeadScan::Scan();

    if (ok)
    {
        wchar_t buf[128];
        swprintf_s(buf, L"[+] Pairs found: %zu (PID %lu)",
            AimbotHeadScan::pairs.size(), AimbotHeadScan::pid);
        Log(buf);
        SetStatus(L"Scan completed successfully");
    }
    else
    {
        Log(L"[-] Nothing found / process not found.");
        SetStatus(L"Scan failed");
    }

    UpdateFoundLabel();
}

static void DoEnable()
{
    if (!AimbotHeadScan::scanned)
    {
        Log(L"[!] Run SCAN first.");
        return;
    }
    if (AimbotHeadScan::enabled)
    {
        Log(L"[i] Already enabled.");
        return;
    }
    Log(L"[*] Enabling...");
    AimbotHeadScan::Enable();
    Log(L"[+] Enabled. Sustain thread started.");
    SetStatus(L"ENABLED");
    UpdateFoundLabel();
}

static void DoDisable()
{
    if (!AimbotHeadScan::enabled)
    {
        Log(L"[i] Already disabled.");
        return;
    }
    Log(L"[*] Disabling...");
    AimbotHeadScan::Disable();
    Log(L"[+] Disabled. Original values restored.");
    SetStatus(L"DISABLED");
    UpdateFoundLabel();
}

static void DoStop()
{
    Log(L"[*] STOP: disable + reset pairs...");
    if (AimbotHeadScan::IsEnabled())
        AimbotHeadScan::Disable();
    AimbotHeadScan::ResetPairs();
    Log(L"[+] Stopped. Everything cleared.");
    SetStatus(L"STOPPED");
    UpdateFoundLabel();
}

static void DoShowProcList()
{
    std::wstring s = L"Built-in process list:\r\n\r\n";
    for (const auto& n : AimbotHeadScan::DefaultProcessList())
        s += L"  - " + n + L"\r\n";
    MessageBoxW(nullptr, s.c_str(), L"Process List", MB_OK | MB_ICONINFORMATION);
}

// ---------- Fonts ----------
static void CreateFonts()
{
    g_hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_hFontBold = CreateFontW(-15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_hFontMono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
}

// ---------- Window Proc ----------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        CreateFonts();

        HWND hLbl1 = CreateWindowW(L"STATIC", L"AOB pattern:",
            WS_CHILD | WS_VISIBLE, 15, 12, 200, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hLbl1, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hPattern = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
            15, 34, 720, 80, hwnd, (HMENU)IDC_EDIT_PATTERN, nullptr, nullptr);
        SendMessageW(g_hPattern, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);

        HWND hLbl2 = CreateWindowW(L"STATIC", L"Read offset (hex):",
            WS_CHILD | WS_VISIBLE, 15, 122, 130, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hLbl2, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        g_hRead = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            150, 119, 90, 24, hwnd, (HMENU)IDC_EDIT_READ, nullptr, nullptr);
        SendMessageW(g_hRead, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);

        HWND hLbl3 = CreateWindowW(L"STATIC", L"Write offset (hex):",
            WS_CHILD | WS_VISIBLE, 260, 122, 130, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hLbl3, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        g_hWrite = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            400, 119, 90, 24, hwnd, (HMENU)IDC_EDIT_WRITE, nullptr, nullptr);
        SendMessageW(g_hWrite, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);

        HWND hLbl4 = CreateWindowW(L"STATIC", L"Process name (if not AUTO):",
            WS_CHILD | WS_VISIBLE, 15, 152, 200, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hLbl4, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hProcName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            220, 149, 220, 24, hwnd, (HMENU)IDC_EDIT_PROCNAME, nullptr, nullptr);
        SendMessageW(g_hProcName, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);

        g_hChkAutoList = CreateWindowW(L"BUTTON", L"Auto process list",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            455, 151, 200, 22, hwnd, (HMENU)IDC_CHK_AUTOLIST, nullptr, nullptr);
        SendMessageW(g_hChkAutoList, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(g_hChkAutoList, BM_SETCHECK, BST_CHECKED, 0);

        g_hBtnProcList = CreateWindowW(L"BUTTON", L"Show list",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            655, 148, 80, 26, hwnd, (HMENU)IDC_BTN_PROC_LIST, nullptr, nullptr);
        SendMessageW(g_hBtnProcList, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hBtnScan = CreateWindowW(L"BUTTON", L"SCAN",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            15, 185, 170, 40, hwnd, (HMENU)IDC_BTN_SCAN, nullptr, nullptr);
        SendMessageW(g_hBtnScan, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);

        g_hBtnEnable = CreateWindowW(L"BUTTON", L"ENABLE",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            195, 185, 170, 40, hwnd, (HMENU)IDC_BTN_ENABLE, nullptr, nullptr);
        SendMessageW(g_hBtnEnable, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);

        g_hBtnDisable = CreateWindowW(L"BUTTON", L"DISABLE",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            375, 185, 170, 40, hwnd, (HMENU)IDC_BTN_DISABLE, nullptr, nullptr);
        SendMessageW(g_hBtnDisable, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);

        g_hBtnStop = CreateWindowW(L"BUTTON", L"STOP",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            555, 185, 180, 40, hwnd, (HMENU)IDC_BTN_STOP, nullptr, nullptr);
        SendMessageW(g_hBtnStop, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);

        g_hFound = CreateWindowW(L"STATIC", L"Pairs found: 0   |   PID: 0",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            15, 235, 500, 22, hwnd, (HMENU)IDC_LBL_FOUND, nullptr, nullptr);
        SendMessageW(g_hFound, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);

        HWND hLblLog = CreateWindowW(L"STATIC", L"Log:",
            WS_CHILD | WS_VISIBLE, 15, 262, 100, 20, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hLblLog, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hBtnExport = CreateWindowW(L"BUTTON", L"Export TXT",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            545, 258, 100, 24, hwnd, (HMENU)IDC_BTN_EXPORT, nullptr, nullptr);
        SendMessageW(g_hBtnExport, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hBtnClearLog = CreateWindowW(L"BUTTON", L"Clear log",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            645, 258, 90, 24, hwnd, (HMENU)IDC_BTN_CLEARLOG, nullptr, nullptr);
        SendMessageW(g_hBtnClearLog, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            15, 285, 720, 180, hwnd, (HMENU)IDC_LOG, nullptr, nullptr);
        SendMessageW(g_hLog, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);

        g_hStatus = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"Ready",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            15, 475, 720, 26, hwnd, (HMENU)IDC_STATUS, nullptr, nullptr);
        SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);

        Log(L"=== AimbotTest AOB FF ===");
        Log(L"Everything is configured inside this window.");
        Log(L"1. Paste AOB pattern.");
        Log(L"2. Set Read / Write offsets (HEX) - fields are empty by default.");
        Log(L"3. Choose process mode: AUTO-LIST or type a name.");
        Log(L"4. Press SCAN, then ENABLE. DISABLE / STOP via buttons.");
        Log(L"5. Press Export TXT - updates the existing file if present.");
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == IDC_BTN_SCAN) { DoScan();         return 0; }
        if (id == IDC_BTN_ENABLE) { DoEnable();       return 0; }
        if (id == IDC_BTN_DISABLE) { DoDisable();      return 0; }
        if (id == IDC_BTN_STOP) { DoStop();         return 0; }
        if (id == IDC_BTN_PROC_LIST) { DoShowProcList(); return 0; }
        if (id == IDC_BTN_EXPORT) { DoExport();       return 0; }
        if (id == IDC_BTN_CLEARLOG) { SetText(g_hLog, L""); return 0; }
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        HWND hCtl = (HWND)lParam;
        if (hCtl == g_hStatus)
        {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(20, 90, 20));
        }
        else if (hCtl == g_hFound)
        {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(0, 70, 140));
        }
        else
        {
            SetBkMode(hdc, TRANSPARENT);
        }
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_CLOSE:
    {
        if (AimbotHeadScan::IsEnabled())
            AimbotHeadScan::Disable();
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY:
    {
        if (g_hFont)     DeleteObject(g_hFont);
        if (g_hFontBold) DeleteObject(g_hFontBold);
        if (g_hFontMono) DeleteObject(g_hFontMono);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------- Entry point ----------
int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    const wchar_t* CLASS_NAME = L"AimbotTestAOBFFWnd";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc))
    {
        MessageBoxW(nullptr, L"RegisterClassEx failed", L"Error", MB_ICONERROR);
        return 1;
    }

    int wndW = 770, wndH = 560;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"AimbotTest AOB FF",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sw - wndW) / 2, (sh - wndH) / 2, wndW, wndH,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd)
    {
        MessageBoxW(nullptr, L"CreateWindowEx failed", L"Error", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}