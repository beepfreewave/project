#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <vector>
#include <string>
#include <sstream>
#include <optional>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <cstdio>

class AimbotHeadScan
{
public:
    struct AddrPair
    {
        DWORD64 addr = 0;
        int origReadVal = 0;
        int origWriteVal = 0;
        int swappedWriteVal = 0;
        int swappedReadVal = 0;
    };

    struct Config
    {
        std::string pattern;
        DWORD64 offsetRead = 0;         // no default offset
        DWORD64 offsetWrite = 0;        // no default offset
        std::wstring processName;
        bool useProcessList = true;
    };

    static Config cfg;

    static std::vector<AddrPair> pairs;
    static std::atomic<bool> scanned;
    static std::atomic<bool> enabled;
    static HANDLE hProcess;
    static DWORD pid;
    static std::atomic<bool> running;
    static std::thread sustainThread;

    static const std::vector<std::wstring>& DefaultProcessList()
    {
        static const std::vector<std::wstring> list = {
            L"HD-Player.exe",
            L"HD-Player",
            L"LdVBoxHeadless.exe",
            L"MEmuHeadless.exe",
            L"AndroidProcess.exe",
            L"aow_exe.exe",
            L"NoxVMHandle.exe",
            L"Nox.exe"
        };
        return list;
    }

    static bool FindPID(DWORD& outPid)
    {
        outPid = 0;

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;

        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(PROCESSENTRY32W);

        bool found = false;

        if (Process32FirstW(snap, &pe))
        {
            do
            {
                if (!cfg.useProcessList && !cfg.processName.empty())
                {
                    if (_wcsicmp(pe.szExeFile, cfg.processName.c_str()) == 0)
                    {
                        outPid = pe.th32ProcessID;
                        found = true;
                        break;
                    }
                }
                else
                {
                    for (const auto& name : DefaultProcessList())
                    {
                        if (_wcsicmp(pe.szExeFile, name.c_str()) == 0)
                        {
                            outPid = pe.th32ProcessID;
                            found = true;
                            break;
                        }
                    }
                    if (found) break;
                }
            } while (Process32NextW(snap, &pe));
        }

        CloseHandle(snap);
        return found;
    }

    static bool OpenTarget()
    {
        if (!FindPID(pid)) return false;
        if (hProcess) { CloseHandle(hProcess); hProcess = nullptr; }
        hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        return hProcess != nullptr;
    }

    static const std::string& GetPattern() { return cfg.pattern; }

    struct Region { DWORD64 Base; SIZE_T Size; };

    static bool IsDataRegion(const MEMORY_BASIC_INFORMATION& mbi)
    {
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & PAGE_GUARD) return false;
        if (mbi.Protect & PAGE_NOACCESS) return false;
        DWORD prot = mbi.Protect;
        bool data = (prot & PAGE_READWRITE) || (prot & PAGE_EXECUTE_READWRITE);
        bool notImage = (mbi.Type != MEM_IMAGE);
        return data && notImage;
    }

    static void CollectDataRegions(HANDLE hp, std::vector<Region>& out)
    {
        out.clear();
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        DWORD64 minAddr = (DWORD64)si.lpMinimumApplicationAddress;
        DWORD64 maxAddr = (DWORD64)si.lpMaximumApplicationAddress;
        DWORD64 cur = minAddr;
        MEMORY_BASIC_INFORMATION mbi;
        while (cur < maxAddr)
        {
            if (VirtualQueryEx(hp, (LPCVOID)cur, &mbi, sizeof(mbi)) == 0) break;
            if (IsDataRegion(mbi))
                out.push_back({ (DWORD64)mbi.BaseAddress, mbi.RegionSize });
            cur = (DWORD64)mbi.BaseAddress + mbi.RegionSize;
        }
    }

    static void ScanRegionList(HANDLE hp, const std::string& pattern,
        const std::vector<Region>& regions, std::vector<DWORD64>& results,
        std::vector<size_t>* hotIdx = nullptr)
    {
        results.clear();
        if (hotIdx) hotIdx->clear();

        std::vector<std::optional<uint8_t>> parsed;
        std::istringstream iss(pattern);
        std::string token;
        while (iss >> token)
        {
            if (token == "??" || token == "?")
                parsed.push_back(std::nullopt);
            else
                parsed.push_back((uint8_t)std::stoi(token, nullptr, 16));
        }

        const size_t patLen = parsed.size();
        if (patLen == 0 || regions.empty()) return;

        bool anchorOk = false;
        uint8_t anchor1 = 0, anchor2 = 0;
        if (patLen >= 2 && parsed[patLen - 2].has_value() && parsed[patLen - 1].has_value())
        {
            anchorOk = true;
            anchor1 = parsed[patLen - 2].value();
            anchor2 = parsed[patLen - 1].value();
        }

        std::mutex mtx;
        std::atomic<size_t> nextIdx{ 0 };
        unsigned nThreads = std::thread::hardware_concurrency();
        if (nThreads == 0) nThreads = 4;
        if (nThreads > 32) nThreads = 32;

        std::vector<std::thread> workers;
        for (unsigned t = 0; t < nThreads; ++t)
        {
            workers.emplace_back([&]()
                {
                    for (;;)
                    {
                        size_t idx = nextIdx.fetch_add(1);
                        if (idx >= regions.size()) break;

                        const Region& r = regions[idx];
                        std::vector<uint8_t> buffer;
                        try { buffer.resize(r.Size); }
                        catch (...) { continue; }

                        SIZE_T bytesRead = 0;
                        if (!ReadProcessMemory(hp, (LPCVOID)r.Base, buffer.data(), r.Size, &bytesRead))
                            continue;
                        if (bytesRead < patLen) continue;

                        const uint8_t* p = buffer.data();
                        const size_t end = bytesRead - patLen + 1;

                        if (anchorOk)
                        {
                            for (size_t i = 0; i < end; ++i)
                            {
                                if (p[i + patLen - 2] != anchor1) continue;
                                if (p[i + patLen - 1] != anchor2) continue;

                                bool match = true;
                                for (size_t j = 0; j < patLen; ++j)
                                {
                                    if (parsed[j].has_value() && p[i + j] != parsed[j].value())
                                    {
                                        match = false; break;
                                    }
                                }
                                if (match)
                                {
                                    std::lock_guard<std::mutex> lock(mtx);
                                    results.push_back(r.Base + i);
                                    if (hotIdx) hotIdx->push_back(idx);
                                }
                            }
                        }
                        else
                        {
                            for (size_t i = 0; i < end; ++i)
                            {
                                bool match = true;
                                for (size_t j = 0; j < patLen; ++j)
                                {
                                    if (parsed[j].has_value() && p[i + j] != parsed[j].value())
                                    {
                                        match = false; break;
                                    }
                                }
                                if (match)
                                {
                                    std::lock_guard<std::mutex> lock(mtx);
                                    results.push_back(r.Base + i);
                                    if (hotIdx) hotIdx->push_back(idx);
                                }
                            }
                        }
                    }
                });
        }

        for (auto& w : workers) w.join();
    }

    static bool ScanPattern(HANDLE hp, const std::string& pattern, std::vector<DWORD64>& results)
    {
        std::vector<Region> regions;
        CollectDataRegions(hp, regions);
        ScanRegionList(hp, pattern, regions, results);
        return !results.empty();
    }

    static bool ReadInt(DWORD64 addr, int& outVal)
    {
        SIZE_T read;
        return ReadProcessMemory(hProcess, (LPCVOID)addr, &outVal, sizeof(int), &read) && read == sizeof(int);
    }

    static bool WriteInt(DWORD64 addr, int val)
    {
        SIZE_T written;
        return WriteProcessMemory(hProcess, (LPVOID)addr, &val, sizeof(int), &written) && written == sizeof(int);
    }

    static void BuildPairs(const std::vector<DWORD64>& addresses)
    {
        for (auto addr : addresses)
        {
            bool known = false;
            for (auto& p : pairs)
            {
                if (p.addr == addr) { known = true; break; }
            }
            if (known) continue;

            DWORD64 ra = addr + cfg.offsetRead;
            DWORD64 wa = addr + cfg.offsetWrite;

            int readVal = 0, writeVal = 0;
            if (!ReadInt(ra, readVal) || !ReadInt(wa, writeVal)) continue;

            AddrPair p;
            p.addr = addr;
            p.origReadVal = readVal;
            p.origWriteVal = writeVal;
            p.swappedWriteVal = readVal;
            p.swappedReadVal = writeVal;
            pairs.push_back(p);
        }
    }

    static bool Scan()
    {
        if (!OpenTarget()) return false;

        std::vector<Region> regions;
        CollectDataRegions(hProcess, regions);
        std::vector<DWORD64> addresses;
        std::vector<size_t> hot;
        ScanRegionList(hProcess, GetPattern(), regions, addresses, &hot);

        bool any = false;
        {
            std::lock_guard<std::mutex> lock(pairsMutex);
            if (!hot.empty())
            {
                hotRegions.clear();
                for (auto i : hot) hotRegions.push_back(regions[i]);
            }
            BuildPairs(addresses);
            any = !pairs.empty();
        }
        scanned = any;
        return any;
    }

    static void Enable()
    {
        if (enabled) return;
        enabled = true;

        {
            std::lock_guard<std::mutex> lock(pairsMutex);
            for (auto& p : pairs)
            {
                WriteInt(p.addr + cfg.offsetWrite, p.swappedWriteVal);
                WriteInt(p.addr + cfg.offsetRead, p.swappedReadVal);
            }
        }

        running = true;
        sustainThread = std::thread(SustainLoop);
    }

    static void Disable()
    {
        if (!enabled) return;
        running = false;
        if (sustainThread.joinable()) sustainThread.join();

        {
            std::lock_guard<std::mutex> lock(pairsMutex);
            for (auto& p : pairs)
            {
                WriteInt(p.addr + cfg.offsetWrite, p.origWriteVal);
                WriteInt(p.addr + cfg.offsetRead, p.origReadVal);
            }
        }

        enabled = false;
        scanned = false;
    }

    static void ResetPairs()
    {
        std::lock_guard<std::mutex> lock(pairsMutex);
        pairs.clear();
        hotRegions.clear();
        scanned = false;
    }

    static bool IsScanned() { return scanned; }
    static bool IsEnabled() { return enabled; }

private:
    static std::mutex pairsMutex;
    static std::vector<Region> hotRegions;

    static void LogPairCount(size_t n)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, "AimbotPairs.txt", "a") == 0 && f)
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(f, "%02d:%02d:%02d pairs=%zu\n", st.wHour, st.wMinute, st.wSecond, n);
            fclose(f);
        }
    }

    static void SustainLoop()
    {
        auto lastRescan = std::chrono::steady_clock::now();

        while (running)
        {
            {
                std::lock_guard<std::mutex> lock(pairsMutex);
                for (auto& p : pairs)
                {
                    WriteInt(p.addr + cfg.offsetWrite, p.swappedWriteVal);
                    WriteInt(p.addr + cfg.offsetRead, p.swappedReadVal);
                }
            }

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastRescan).count() >= 30)
            {
                lastRescan = now;

                if (!hProcess)
                {
                    if (!OpenTarget()) continue;
                }

                std::vector<Region> regions;
                {
                    std::lock_guard<std::mutex> lock(pairsMutex);
                    regions = hotRegions;
                }
                if (regions.empty())
                    CollectDataRegions(hProcess, regions);
                if (regions.empty()) continue;

                std::vector<DWORD64> addresses;
                std::vector<size_t> hot;
                ScanRegionList(hProcess, GetPattern(), regions, addresses, &hot);

                if (!hot.empty())
                {
                    std::lock_guard<std::mutex> lock(pairsMutex);
                    hotRegions.clear();
                    for (auto i : hot) hotRegions.push_back(regions[i]);
                }

                size_t before = 0, after = 0;
                if (!addresses.empty())
                {
                    std::lock_guard<std::mutex> lock(pairsMutex);
                    before = pairs.size();
                    BuildPairs(addresses);
                    after = pairs.size();
                }

                if (after != before)
                    LogPairCount(after);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};