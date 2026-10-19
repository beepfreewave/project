#include "main.hpp"
#include <fstream>
#include <sstream>
#include <map>
#include <unordered_map>
#include <cmath>

static const char* CONFIG_FILE = "fake_lag_config.json";

bool WinDivertDllNextToExe() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) return false;
    *(slash + 1) = L'\0';
    std::wstring dllPath = std::wstring(exePath) + L"WinDivert.dll";
    DWORD attr = GetFileAttributesW(dllPath.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsSyncPacket(const BYTE* payload, UINT payloadLen, bool inVehicle) {
    if (payloadLen < 1) return false;
    if (inVehicle) {
        if (payloadLen == 74 || payloadLen == 77 || payloadLen == 87 ||
            payloadLen == 96 || payloadLen == 114) return true;
        return false;
    }
    if (payloadLen == 79 || payloadLen == 106) return true;
    return false;
}

void SaveConfig() {
    std::ofstream f(CONFIG_FILE);
    f << "{\n"
        << "  \"lag_hotkey\": " << g_hotkeyLag << ",\n"
        << "  \"delay\": " << g_delayMs.load() << ",\n"
        << "  \"in_vehicle\": " << (g_inVehicle.load() ? 1 : 0) << "\n"
        << "}\n";
}

void LoadConfig() {
    g_hotkeyLag = VK_F6;
    g_delayMs = 250;
    g_inVehicle = false;

    std::ifstream f(CONFIG_FILE);
    if (!f) { SaveConfig(); return; }
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();

    auto get = [&](const char* k) -> int {
        auto p = s.find(std::string("\"") + k + "\"");
        if (p == std::string::npos) return -1;
        p = s.find(':', p); if (p == std::string::npos) return -1;
        return (int)strtol(s.c_str() + p + 1, nullptr, 10);
        };

    int v = get("lag_hotkey"); if (v != -1) g_hotkeyLag = v;
    v = get("delay"); if (v != -1) g_delayMs = v;
    v = get("in_vehicle"); if (v != -1) g_inVehicle = (v != 0);
}

void SenderThread() {
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_bufMutex);
        while (!g_buffer.empty() && g_buffer.front().sendTime <= now) {
            auto& p = g_buffer.front();
            UINT s = 0;
            WinDivertSend(g_hDivert, p.data.data(), (UINT)p.data.size(), &s, &p.addr);
            g_buffer.pop();
        }
    }
}

void FlushBuffer() {
    std::lock_guard<std::mutex> lock(g_bufMutex);
    while (!g_buffer.empty()) {
        auto& p = g_buffer.front();
        UINT s = 0;
        WinDivertSend(g_hDivert, p.data.data(), (UINT)p.data.size(), &s, &p.addr);
        g_buffer.pop();
    }
}

void DivertLoop() {
    std::vector<BYTE> buf(65535);
    WINDIVERT_ADDRESS addr{};
    UINT len = 0;

    while (g_running) {
        if (!WinDivertRecv(g_hDivert, buf.data(), (UINT)buf.size(), &len, &addr)) {
            DWORD e = GetLastError();
            if (e == ERROR_NO_DATA) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        WINDIVERT_IPHDR* ip = nullptr;
        WINDIVERT_IPV6HDR* ipv6 = nullptr;
        UINT8 protocol = 0;
        WINDIVERT_ICMPHDR* icmp = nullptr;
        WINDIVERT_ICMPV6HDR* icmpv6 = nullptr;
        WINDIVERT_TCPHDR* tcp = nullptr;
        WINDIVERT_UDPHDR* udp = nullptr;
        PVOID payload = nullptr;
        UINT payloadLen = 0;
        PVOID next = nullptr;
        UINT nextLen = 0;

        BOOL parsed = WinDivertHelperParsePacket(
            buf.data(), len,
            &ip, &ipv6, &protocol, &icmp, &icmpv6, &tcp, &udp,
            &payload, &payloadLen, &next, &nextLen);

        if (!parsed || !udp) {
            UINT s = 0; WinDivertSend(g_hDivert, buf.data(), len, &s, &addr);
            continue;
        }

        UINT16 dport = ntohs(udp->DstPort);
        if (dport != SERVER_PORT) {
            UINT s = 0; WinDivertSend(g_hDivert, buf.data(), len, &s, &addr);
            continue;
        }

        if (SERVER_IP != 0 && ip && ip->DstAddr != SERVER_IP) {
            UINT s = 0; WinDivertSend(g_hDivert, buf.data(), len, &s, &addr);
            continue;
        }

        bool isSync = IsSyncPacket((BYTE*)payload, payloadLen, g_inVehicle.load());

        if (g_lagEnabled && isSync) {
            BufferedPacket bp;
            bp.data.assign(buf.begin(), buf.begin() + len);
            bp.addr = addr;
            bp.sendTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(g_delayMs.load());

            std::lock_guard<std::mutex> lock(g_bufMutex);
            if (g_buffer.size() >= MAX_BUFFER) {
                g_lagEnabled = false;
                while (!g_buffer.empty()) {
                    auto& p = g_buffer.front();
                    UINT s = 0; WinDivertSend(g_hDivert, p.data.data(), (UINT)p.data.size(), &s, &p.addr);
                    g_buffer.pop();
                }
                UINT s = 0; WinDivertSend(g_hDivert, buf.data(), len, &s, &addr);
            }
            else {
                g_buffer.push(std::move(bp));
            }
        }
        else {
            UINT s = 0; WinDivertSend(g_hDivert, buf.data(), len, &s, &addr);
        }
    }
}

void HotkeyThread() {
    bool hk_prev = false;
    while (g_running) {
        if (g_hotkey_suppress.load()) {
            int svk = g_hotkey_suppress_vk.load();
            bool still_down = (svk != 0) && ((GetAsyncKeyState(svk) & 0x8000) != 0);
            if (!still_down) {
                g_hotkey_suppress = false;
                g_hotkey_suppress_vk = 0;
                hk_prev = (g_hotkeyLag && (GetAsyncKeyState(g_hotkeyLag) & 0x8000));
            }
            Sleep(10);
            continue;
        }

        if (g_hotkeyLag) {
            bool d = (GetAsyncKeyState(g_hotkeyLag) & 0x8000) != 0;
            if (d && !hk_prev) {
                g_lagEnabled = !g_lagEnabled.load();
                if (!g_lagEnabled.load()) FlushBuffer();
            }
            hk_prev = d;
        }
        Sleep(10);
    }
}

std::string GetKeyName(int vk) {
    if (vk == 0) return "NONE";
    if (vk == VK_LBUTTON) return "M1";
    if (vk == VK_RBUTTON) return "M2";
    if (vk == VK_MBUTTON) return "M3";
    if (vk == VK_XBUTTON1) return "M4";
    if (vk == VK_XBUTTON2) return "M5";
    if (vk == VK_CAPITAL) return "CAPS";
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) return "SHIFT";
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) return "CTRL";
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) return "ALT";
    if (vk == VK_SPACE) return "SPACE";
    if (vk == VK_RETURN) return "ENTER";
    if (vk == VK_TAB) return "TAB";
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    CHAR name[32] = { 0 };
    if (sc != 0 && GetKeyNameTextA(sc << 16, name, sizeof(name)) != 0) {
        std::string s(name);
        for (auto& c : s) c = (char)toupper((unsigned char)c);
        return s;
    }
    return "KEY " + std::to_string(vk);
}

static std::unordered_map<std::string, float> g_switchAnims;

bool CustomSwitch(const char* id, bool* v, ImVec4 active_col) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float height = 20.0f, width = 40.0f, radius = height * 0.5f;
    bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height));
    if (clicked) *v = !*v;
    std::string key(id);
    if (g_switchAnims.find(key) == g_switchAnims.end()) g_switchAnims[key] = *v ? 1.0f : 0.0f;
    float& anim = g_switchAnims[key];
    float target = *v ? 1.0f : 0.0f;
    anim += (target - anim) * 0.2f;
    ImVec4 bg_off(0.15f, 0.15f, 0.17f, 1.0f);
    ImVec4 bg(bg_off.x + (active_col.x - bg_off.x) * anim,
        bg_off.y + (active_col.y - bg_off.y) * anim,
        bg_off.z + (active_col.z - bg_off.z) * anim, 1.0f);
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), ImGui::ColorConvertFloat4ToU32(bg), radius);
    float kx = p.x + radius + (width - height) * anim;
    dl->AddCircleFilled(ImVec2(kx, p.y + radius), radius - 2.5f, IM_COL32(230, 230, 235, 255), 18);
    return clicked;
}

bool KeybindButton(const char* id, int* vk_code, int* active_bind_id, int my_id,
    bool* just_opened, bool* ignore_lbutton, ImVec2 size, ImVec4 accent_col) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool is_binding = (*active_bind_id == my_id);
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    if (clicked) {
        if (is_binding) *active_bind_id = -1;
        else { *active_bind_id = my_id; *just_opened = true; *ignore_lbutton = true; }
    }
    ImU32 bg = is_binding
        ? IM_COL32((int)(accent_col.x * 255 * 0.25f), (int)(accent_col.y * 255 * 0.25f), (int)(accent_col.z * 255 * 0.25f), 255)
        : (hovered ? IM_COL32(28, 28, 36, 255) : IM_COL32(18, 18, 24, 255));
    ImU32 br = is_binding ? ImGui::ColorConvertFloat4ToU32(accent_col)
        : (hovered ? IM_COL32(90, 90, 105, 255) : IM_COL32(40, 40, 50, 255));
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, 6.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), br, 6.0f, 0, 1.2f);
    std::string label = is_binding ? "..." : GetKeyName(*vk_code);
    ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    ImU32 tc = is_binding ? ImGui::ColorConvertFloat4ToU32(accent_col)
        : (*vk_code != 0 ? IM_COL32(230, 230, 240, 255) : IM_COL32(110, 110, 120, 255));
    dl->AddText(ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f), tc, label.c_str());
    return clicked;
}

bool DrawSidebarTab(const char* label, bool selected, ImVec2 pos, ImVec2 size, ImVec4 accent_col) {
    ImGui::SetCursorScreenPos(pos);
    std::string id = std::string("##tab_") + label;
    bool clicked = ImGui::InvisibleButton(id.c_str(), size);
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) {
        ImU32 bg = IM_COL32((int)(accent_col.x * 255), (int)(accent_col.y * 255), (int)(accent_col.z * 255), 30);
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, 8.0f);
        dl->AddRectFilled(ImVec2(pos.x + 2, pos.y + 6), ImVec2(pos.x + 5, pos.y + size.y - 6), ImGui::ColorConvertFloat4ToU32(accent_col), 2.0f);
    }
    else if (hovered) {
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(255, 255, 255, 10), 8.0f);
    }
    ImU32 tc = selected ? IM_COL32(245, 245, 250, 255) : (hovered ? IM_COL32(200, 200, 205, 255) : IM_COL32(120, 120, 130, 255));
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(pos.x + 18.0f, pos.y + (size.y - ts.y) * 0.5f), tc, label);
    return clicked;
}

bool CreateD3D() {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwndMain;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL arr[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT r = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, arr, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &fl, &g_ctx);
    if (r == DXGI_ERROR_UNSUPPORTED)
        r = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, arr, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &fl, &g_ctx);
    if (r != S_OK) return false;

    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
        g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
    }
    return true;
}

void CleanupD3D() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

static bool   g_dragMain = false;
static POINT  g_dragMainOffset{ 0, 0 };

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_NCHITTEST: return HTCLIENT;
    case WM_LBUTTONDOWN:
    {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (pt.y >= 0 && pt.y <= 44) {
            ClientToScreen(hWnd, &pt);
            RECT wr; GetWindowRect(hWnd, &wr);
            g_dragMain = true;
            g_dragMainOffset.x = pt.x - wr.left;
            g_dragMainOffset.y = pt.y - wr.top;
            SetCapture(hWnd);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        if (g_dragMain) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hWnd, &pt);
            SetWindowPos(hWnd, nullptr, pt.x - g_dragMainOffset.x, pt.y - g_dragMainOffset.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g_dragMain) { g_dragMain = false; ReleaseCapture(); return 0; }
        break;
    case WM_SIZE:
        if (g_dev && wParam != SIZE_MINIMIZED) {
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
            g_swap->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            ID3D11Texture2D* bb = nullptr;
            if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
                g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
                bb->Release();
            }
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR pCmd, int nShow) {
    (void)hPrev; (void)pCmd; (void)nShow;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (!WinDivertDllNextToExe()) {
        MessageBoxW(nullptr,
            L"WinDivert.dll not found next to the .exe.\n\n"
            L"Put WinDivert.dll and WinDivert64.sys in the same folder as this program.",
            L"WinDivert not found", MB_OK | MB_ICONERROR);
        return 1;
    }

    LoadConfig();

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"FakeLagClassUI", nullptr };
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int mainX = (sw - MAIN_W) / 2;
    int mainY = (sh - MAIN_H) / 2;

    g_hwndMain = CreateWindowExW(0, wc.lpszClassName, L"FakeLag", WS_POPUP, mainX, mainY, MAIN_W, MAIN_H, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateD3D()) { CleanupD3D(); UnregisterClassW(wc.lpszClassName, wc.hInstance); return 1; }
    ShowWindow(g_hwndMain, SW_SHOWDEFAULT);
    UpdateWindow(g_hwndMain);

    g_hDivert = WinDivertOpen(FILTER, WINDIVERT_LAYER_NETWORK, 0, 0);
    if (g_hDivert == INVALID_HANDLE_VALUE) {
        MessageBoxW(nullptr, L"WinDivertOpen failed. Run as Admin.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    WinDivertSetParam(g_hDivert, WINDIVERT_PARAM_QUEUE_LENGTH, 8192);
    WinDivertSetParam(g_hDivert, WINDIVERT_PARAM_QUEUE_TIME, 2000);
    SERVER_IP = inet_addr("185.169.134.107");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImFontConfig fc; fc.OversampleH = 4; fc.OversampleV = 4; fc.PixelSnapH = false;
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 15.0f, &fc);
    if (!font) font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arialbd.ttf", 15.0f, &fc);
    if (!font) font = io.Fonts->AddFontDefault();

    ImGui_ImplWin32_Init(g_hwndMain);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    ImVec4 accentBlue = ImVec4(0.12f, 0.60f, 1.00f, 1.00f);

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.WindowPadding = ImVec2(0, 0);
    style.FrameRounding = 8.0f;
    style.GrabRounding = 12.0f;
    style.GrabMinSize = 14.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.07f, 1.00f);
    style.Colors[ImGuiCol_SliderGrab] = accentBlue;
    style.Colors[ImGuiCol_SliderGrabActive] = accentBlue;
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);

    bool lag_enabled_ui = false;
    bool in_vehicle_ui = false;
    int current_tab = 0;

    int active_bind_id = -1;
    bool bind_just_opened = false;
    bool ignore_lbutton = false;

    std::vector<WhiteDot> whites;
    for (int i = 0; i < 24; i++) {
        WhiteDot d;
        d.x = (float)(rand() % MAIN_W);
        d.y = (float)(rand() % MAIN_H);
        d.baseSpeed = 1.1f + (rand() % 100) / 110.0f;
        d.vy = ((rand() % 100) / 100.0f - 0.5f) * 0.30f;
        d.pathT = (float)(rand() % 100) / 100.0f;
        d.radius = 2.0f + (rand() % 14) / 6.0f;
        d.alpha = 0.5f + (rand() % 50) / 100.0f;
        d.alpha_speed = 0.003f + (rand() % 10) / 1600.0f;
        whites.push_back(d);
    }

    std::vector<PurpleDot> purples;
    for (int i = 0; i < 18; i++) {
        PurpleDot d;
        d.x = (float)(rand() % MAIN_W);
        d.y = (float)(rand() % MAIN_H);
        d.baseSpeed = 0.75f + (rand() % 100) / 150.0f;
        d.vx = ((rand() % 100) / 100.0f - 0.5f) * 0.20f;
        d.pathT = (float)(rand() % 100) / 100.0f;
        d.radius = 2.2f + (rand() % 16) / 5.0f;
        d.alpha = 0.5f + (rand() % 50) / 100.0f;
        d.alpha_speed = 0.003f + (rand() % 10) / 1600.0f;
        d.hue = (float)(rand() % 100) / 100.0f;
        purples.push_back(d);
    }

    std::thread(DivertLoop).detach();
    std::thread(SenderThread).detach();
    std::thread(HotkeyThread).detach();

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (active_bind_id != -1) {
            if (ignore_lbutton) {
                if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) ignore_lbutton = false;
            }
            if (!bind_just_opened) {
                for (int vk = 1; vk < 256; vk++) {
                    if (vk == VK_LBUTTON && ignore_lbutton) continue;
                    if (vk == VK_CANCEL) continue;
                    if (GetAsyncKeyState(vk) & 0x8000) {
                        if (active_bind_id == 1) g_hotkeyLag = (vk == VK_ESCAPE) ? 0 : vk;
                        SaveConfig();
                        if (vk != VK_ESCAPE) {
                            g_hotkey_suppress_vk = vk;
                            g_hotkey_suppress = true;
                        }
                        active_bind_id = -1;
                        break;
                    }
                }
            }
            bind_just_opened = false;
        }

        lag_enabled_ui = g_lagEnabled.load();
        in_vehicle_ui = g_inVehicle.load();

        ImVec4 accent = accentBlue;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::PushFont(font);

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2((float)MAIN_W, (float)MAIN_H), ImGuiCond_Always);
        ImGui::Begin("FakeLagMain", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();

        for (auto& d : whites) {
            float sp = SpeedProfile(d.pathT);
            d.x += d.baseSpeed * sp;
            d.y += d.vy * sp;
            d.pathT += 0.0045f * sp;
            if (d.pathT > 1.0f) d.pathT -= 1.0f;
            d.alpha += d.alpha_speed;
            if (d.alpha > 1.0f || d.alpha < 0.20f) d.alpha_speed = -d.alpha_speed;
            if (d.x > ws.x + 40) { d.x = -40; }
            if (d.x < -40) { d.x = ws.x + 40; }
            if (d.y < -40) d.y = ws.y + 40;
            if (d.y > ws.y + 40) d.y = -40;
            ImVec2 c(wp.x + d.x, wp.y + d.y);
            const int steps = 10;
            float maxR = d.radius * 4.5f;
            for (int i = steps; i >= 1; --i) {
                float t = (float)i / (float)steps;
                float rr = maxR * t;
                float a = (1.0f - t) * (1.0f - t) * d.alpha * 55.0f;
                if (a < 0.0f) a = 0.0f;
                if (a > 255.0f) a = 255.0f;
                dl->AddCircleFilled(c, rr, IM_COL32(255, 255, 255, (int)a), 24);
            }
            float coreA = d.alpha * 255.0f;
            if (coreA > 255.0f) coreA = 255.0f;
            dl->AddCircleFilled(c, d.radius * 0.55f, IM_COL32(255, 255, 255, (int)coreA), 16);
        }

        for (auto& d : purples) {
            float sp = SpeedProfile(d.pathT);
            d.x += d.vx * sp;
            d.y += d.baseSpeed * sp;
            d.pathT += 0.0045f * sp;
            if (d.pathT > 1.0f) d.pathT -= 1.0f;
            d.alpha += d.alpha_speed;
            if (d.alpha > 1.0f || d.alpha < 0.20f) d.alpha_speed = -d.alpha_speed;
            if (d.x > ws.x + 40) d.x = -40;
            if (d.x < -40) d.x = ws.x + 40;
            if (d.y > ws.y + 40) d.y = -40;
            if (d.y < -40) d.y = ws.y + 40;
            ImVec2 c(wp.x + d.x, wp.y + d.y);
            const ImVec4 cA(0.55f, 0.20f, 1.00f, 1.0f);
            const ImVec4 cB(0.85f, 0.15f, 0.95f, 1.0f);
            float m = d.hue;
            float r = cA.x + (cB.x - cA.x) * m;
            float g = cA.y + (cB.y - cA.y) * m;
            float b = cA.z + (cB.z - cA.z) * m;
            const int steps = 10;
            float maxR = d.radius * 4.5f;
            for (int i = steps; i >= 1; --i) {
                float t = (float)i / (float)steps;
                float rr = maxR * t;
                float a = (1.0f - t) * (1.0f - t) * d.alpha * 90.0f;
                if (a < 0.0f) a = 0.0f;
                if (a > 255.0f) a = 255.0f;
                dl->AddCircleFilled(c, rr, IM_COL32((int)(r * 255.0f), (int)(g * 255.0f), (int)(b * 255.0f), (int)a), 24);
            }
            float coreA = d.alpha * 255.0f;
            if (coreA > 255.0f) coreA = 255.0f;
            float cr = r + (1.0f - r) * 0.45f;
            float cg = g + (1.0f - g) * 0.45f;
            float cb = b + (1.0f - b) * 0.45f;
            dl->AddCircleFilled(c, d.radius * 0.55f, IM_COL32((int)(cr * 255.0f), (int)(cg * 255.0f), (int)(cb * 255.0f), (int)coreA), 16);
        }

        float sbw = 135.0f;
        dl->AddRectFilled(wp, ImVec2(wp.x + sbw, wp.y + ws.y), IM_COL32(11, 11, 14, 230), 0.0f);
        dl->AddLine(ImVec2(wp.x + sbw, wp.y), ImVec2(wp.x + sbw, wp.y + ws.y), IM_COL32(32, 32, 38, 255));
        dl->AddText(ImVec2(wp.x + 18.0f, wp.y + 18.0f), IM_COL32(245, 245, 250, 255), "PANEL");

        if (DrawSidebarTab("Lag", current_tab == 0, ImVec2(wp.x + 10, wp.y + 60), ImVec2(115, 38), accent)) current_tab = 0;
        if (DrawSidebarTab("Settings", current_tab == 1, ImVec2(wp.x + 10, wp.y + 104), ImVec2(115, 38), accent)) current_tab = 1;

        float bw = 200.0f, bh = 32.0f;
        float cx = sbw + (ws.x - sbw - bw) * 0.5f;
        ImVec2 bp(wp.x + cx, wp.y + 12.0f);
        dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bh), IM_COL32(18, 18, 22, 240), 16.0f);
        ImVec2 ts = ImGui::CalcTextSize("FAKE LAG");
        dl->AddText(ImVec2(bp.x + (bw - ts.x) * 0.5f, bp.y + (bh - ts.y) * 0.5f), IM_COL32(245, 245, 250, 255), "FAKE LAG");

        float start_y = 68.0f;
        float main_left = sbw + 30.0f;
        float bind_x = ws.x - 132.0f;
        float switch_x = ws.x - 65.0f;

        if (current_tab == 0) {
            dl->AddText(ImVec2(wp.x + main_left, wp.y + start_y + 2), IM_COL32(205, 205, 210, 255), "Lag Switch");
            ImGui::SetCursorPos(ImVec2(bind_x, start_y));
            KeybindButton("##HkLag", &g_hotkeyLag, &active_bind_id, 1, &bind_just_opened, &ignore_lbutton, ImVec2(56, 24), accent);
            ImGui::SetCursorPos(ImVec2(switch_x, start_y + 2));
            if (CustomSwitch("##SwLag", &lag_enabled_ui, accent)) {
                g_lagEnabled = lag_enabled_ui;
                if (!lag_enabled_ui) FlushBuffer();
            }
            bool connected = g_running.load();
            const char* status = connected ? "RUNNING" : "STOPPED";
            ImU32 col = connected ? IM_COL32(50, 230, 100, 255) : IM_COL32(50, 50, 50, 255);
            dl->AddText(ImVec2(wp.x + main_left, wp.y + start_y + 52 + 8), col, status);
        }
        else {
            dl->AddText(ImVec2(wp.x + main_left, wp.y + start_y + 2), IM_COL32(205, 205, 210, 255), "In vehicle");
            ImGui::SetCursorPos(ImVec2(switch_x, start_y + 2));
            if (CustomSwitch("##SwVehicle", &in_vehicle_ui, accent)) {
                g_inVehicle = in_vehicle_ui;
                g_delayMs = 250;
                SaveConfig();
            }

            dl->AddText(ImVec2(wp.x + main_left, wp.y + start_y + 52 + 2), IM_COL32(205, 205, 210, 255), "Delay");
            char delayBuf[32];
            sprintf_s(delayBuf, "%d ms", g_delayMs.load());
            ImVec2 dts = ImGui::CalcTextSize(delayBuf);
            dl->AddText(ImVec2(wp.x + ws.x - dts.x - 30, wp.y + start_y + 52 + 2), IM_COL32(150, 150, 160, 255), delayBuf);

            ImGui::SetCursorPos(ImVec2(main_left, start_y + 52 + 26));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(18, 18, 24, 255));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, accent);
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, accent);
            ImGui::PushItemWidth(ws.x - main_left - 30);
            int maxDelay = g_inVehicle.load() ? 400 : 1500;
            int d = g_delayMs.load();
            if (ImGui::SliderInt("##delay", &d, 50, maxDelay, "")) {
                g_delayMs = d;
                SaveConfig();
            }
            ImGui::PopItemWidth();
            ImGui::PopStyleColor(3);
        }

        ImGui::End();
        ImGui::PopFont();
        ImGui::Render();

        const float clear[4] = { 0,0,0,0 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    g_running = false;
    Sleep(200);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupD3D();
    WinDivertClose(g_hDivert);
    DestroyWindow(g_hwndMain);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}