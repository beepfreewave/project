#pragma once
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <string>
#include <queue>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>

#include "../imgui/imgui.h"
#include "../imgui/imgui_impl_win32.h"
#include "../imgui/imgui_impl_dx11.h"
#include "../windivert/windivert.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "windivert/WinDivert.lib")

#pragma warning(disable: 6387)
#pragma warning(disable: 28251)

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static const int MAIN_W = 540;
static const int MAIN_H = 340;

static const char* FILTER = "outbound and udp";
static UINT16 SERVER_PORT = 7777;
static UINT32 SERVER_IP = 0;
static size_t MAX_BUFFER = 5000;

struct BufferedPacket {
    std::vector<BYTE> data;
    WINDIVERT_ADDRESS addr{};
    std::chrono::steady_clock::time_point sendTime{};
};

static std::queue<BufferedPacket> g_buffer;
static std::mutex g_bufMutex;
static std::atomic<bool> g_lagEnabled{ false };
static std::atomic<bool> g_running{ true };
static std::atomic<int>  g_delayMs{ 250 };
static std::atomic<bool> g_inVehicle{ false };
static HANDLE g_hDivert = INVALID_HANDLE_VALUE;

static int g_hotkeyLag = VK_F6;
static bool g_waitingBindLag = false;
static std::atomic<bool> g_hotkey_suppress{ false };
static std::atomic<int>  g_hotkey_suppress_vk{ 0 };

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static HWND g_hwndMain = nullptr;

bool IsSyncPacket(const BYTE* payload, UINT payloadLen, bool inVehicle);
void SenderThread();
void FlushBuffer();
void DivertLoop();
void HotkeyThread();
void LoadConfig();
void SaveConfig();
std::string GetKeyName(int vk);
bool CreateD3D();
void CleanupD3D();
bool CustomSwitch(const char* id, bool* v, ImVec4 active_col);
bool KeybindButton(const char* id, int* vk_code, int* active_bind_id, int my_id, bool* just_opened, bool* ignore_lbutton, ImVec2 size, ImVec4 accent_col);
bool DrawSidebarTab(const char* label, bool selected, ImVec2 pos, ImVec2 size, ImVec4 accent_col);
bool WinDivertDllNextToExe();

struct WhiteDot {
    float x, y;
    float baseSpeed;
    float vy;
    float pathT;
    float radius;
    float alpha;
    float alpha_speed;
};

struct PurpleDot {
    float x, y;
    float baseSpeed;
    float vx;
    float pathT;
    float radius;
    float alpha;
    float alpha_speed;
    float hue;
};

static inline float SpeedProfile(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return 1.0f - 0.65f * sinf(3.14159265f * t);
}