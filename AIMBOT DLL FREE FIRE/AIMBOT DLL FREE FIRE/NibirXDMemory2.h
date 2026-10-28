#pragma once
#include <Windows.h>
#include <vector>
#include <string>
#include <iostream>
#include <TlHelp32.h>
#include <tchar.h>
#include <winternl.h>
#include <mmsystem.h>
#include <unordered_map>
#include <mutex>
#include <future>
#include <fstream>
#include <shlobj.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "shell32.lib")

typedef NTSTATUS(NTAPI* pNtReadVirtualMemory)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
typedef NTSTATUS(NTAPI* pNtWriteVirtualMemory)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
typedef NTSTATUS(NTAPI* pNtProtectVirtualMemory)(HANDLE, PVOID*, PSIZE_T, ULONG, PULONG);

class Memory {
public:
    DWORD ProcessId = 0;
    HANDLE ProcessHandle = nullptr;

    pNtReadVirtualMemory fnNtRead = nullptr;
    pNtWriteVirtualMemory fnNtWrite = nullptr;
    pNtProtectVirtualMemory fnNtProtect = nullptr;

    Memory() {
        HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
        if (hNtdll) {
            fnNtRead = (pNtReadVirtualMemory)GetProcAddress(hNtdll, "NtReadVirtualMemory");
            fnNtWrite = (pNtWriteVirtualMemory)GetProcAddress(hNtdll, "NtWriteVirtualMemory");
            fnNtProtect = (pNtProtectVirtualMemory)GetProcAddress(hNtdll, "NtProtectVirtualMemory");
        }
    }

    typedef struct _MEMORY_REGION {
        DWORD_PTR dwBaseAddr;
        DWORD_PTR dwMemorySize;
    } MEMORY_REGION;

    int GetPid(const wchar_t* procname) {
        if (procname == nullptr) return 0;
        DWORD pid = 0;
        DWORD threadCount = 0;
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(PROCESSENTRY32);
        if (Process32First(hSnap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, procname) == 0) {
                    if ((int)pe.cntThreads > threadCount) {
                        threadCount = pe.cntThreads;
                        pid = pe.th32ProcessID;
                    }
                }
            } while (Process32Next(hSnap, &pe));
        }
        CloseHandle(hSnap);
        return pid;
    }

    const wchar_t* GetEmulatorRunning() {
        if (GetPid(L"HD-Player.exe") != 0) return L"HD-Player.exe";
        if (GetPid(L"LdVBoxHeadless.exe") != 0) return L"LdVBoxHeadless.exe";
        if (GetPid(L"MEmuHeadless.exe") != 0) return L"MEmuHeadless.exe";
        if (GetPid(L"AndroidProcess.exe") != 0) return L"AndroidProcess.exe";
        if (GetPid(L"aow_exe.exe") != 0) return L"aow_exe.exe";
        if (GetPid(L"NoxVMHandle.exe") != 0) return L"NoxVMHandle.exe";
        return nullptr;
    }

    std::vector<BYTE> ScanAimbot = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', '?', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA5, 0x43 };

    std::unordered_map<DWORD_PTR, int> Nibirxd1;
    std::unordered_map<DWORD_PTR, int> Nibirxd2;
    std::unordered_map<DWORD_PTR, int> Nibirxd3;
    std::unordered_map<DWORD_PTR, int> Nibirxd4;
    std::unordered_map<DWORD_PTR, int> Nibirxd5;

    void EnableAimHead() {
        std::thread([=]() {
            std::vector<DWORD_PTR> aimbotAddresses;
            if (!AttackProcess(GetEmulatorRunning())) {
                std::cerr << "[!] Unable to attach to target process." << std::endl;
                return;
            }
            SYSTEM_INFO memoryInfo;
            GetSystemInfo(&memoryInfo);
            DWORD_PTR rangeStart = reinterpret_cast<DWORD_PTR>(memoryInfo.lpMinimumApplicationAddress);
            DWORD_PTR rangeEnd = reinterpret_cast<DWORD_PTR>(memoryInfo.lpMaximumApplicationAddress);
            std::vector<DWORD_PTR>().swap(aimbotAddresses);

            FindPattern(rangeStart, rangeEnd, ScanAimbot.data(), aimbotAddresses);
            if (aimbotAddresses.empty()) {
                std::cerr << "[!] Memory pattern not found." << std::endl;
                return;
            }
            for (auto patternBase : aimbotAddresses) {
                DWORD_PTR readAddr = patternBase + static_cast<DWORD_PTR>(170.4999999);
                DWORD_PTR writeAddr = patternBase + 166;
                int readBuffer = 0;
                int writeBuffer = 0;

                fnNtRead(ProcessHandle, (PVOID)writeAddr, &writeBuffer, sizeof(writeBuffer), nullptr);
                Nibirxd1.insert({ writeAddr, writeBuffer });

                fnNtRead(ProcessHandle, (PVOID)readAddr, &readBuffer, sizeof(readBuffer), nullptr);
                Nibirxd2.insert({ readAddr, readBuffer });

                fnNtWrite(ProcessHandle, (PVOID)writeAddr, &readBuffer, sizeof(readBuffer), nullptr);
                Nibirxd3[writeAddr] = readBuffer;

                fnNtWrite(ProcessHandle, (PVOID)readAddr, &writeBuffer, sizeof(writeBuffer), nullptr);
                Nibirxd4[readAddr] = writeBuffer;
            }
            Beep(500, 500);
            CloseHandle(ProcessHandle);
            }).detach();
    }

    void Restore() {
        std::thread([=]() {
            if (!AttackProcess(GetEmulatorRunning())) {
                std::cerr << "[!] Failed to attach to process for restoration!" << std::endl;
                return;
            }
            for (const auto& entry : Nibirxd1) {
                DWORD_PTR address = entry.first;
                int originalValue = entry.second;
                fnNtWrite(ProcessHandle, (PVOID)address, &originalValue, sizeof(originalValue), NULL);
            }
            for (const auto& entry : Nibirxd2) {
                DWORD_PTR address = entry.first;
                int originalValue = entry.second;
                fnNtWrite(ProcessHandle, (PVOID)address, &originalValue, sizeof(originalValue), NULL);
            }
            CloseHandle(ProcessHandle);
            }).detach();
    }

    void Reapply() {
        std::thread([=]() {
            if (!AttackProcess(GetEmulatorRunning())) {
                std::cerr << "[!] Failed to attach to process for reapplying changes!" << std::endl;
                return;
            }
            for (const auto& entry : Nibirxd3) {
                DWORD_PTR address = entry.first;
                int valueToReapply = entry.second;
                fnNtWrite(ProcessHandle, (PVOID)address, &valueToReapply, sizeof(valueToReapply), NULL);
            }
            for (const auto& entry : Nibirxd4) {
                DWORD_PTR address = entry.first;
                int valueToReapply = entry.second;
                fnNtWrite(ProcessHandle, (PVOID)address, &valueToReapply, sizeof(valueToReapply), NULL);
            }
            CloseHandle(ProcessHandle);
            }).detach();
    }

    BOOL AttackProcess(const wchar_t* procname) {
        DWORD ProcId = GetPid(procname);
        if (ProcId == 0) return false;
        ProcessId = ProcId;
        ProcessHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, ProcessId);
        return ProcessHandle != nullptr;
    }

    bool ReplacePattern(DWORD_PTR dwStartRange, DWORD_PTR dwEndRange, BYTE* SearchAob, BYTE* ReplaceAob) {
        SIZE_T RepByteSize = _msize(ReplaceAob);
        if (RepByteSize <= 0) return false;

        std::vector<DWORD_PTR> foundedAddress;
        FindPattern(dwStartRange, dwEndRange, SearchAob, foundedAddress);
        if (foundedAddress.empty()) return false;

        DWORD OldProtect;
        for (auto address : foundedAddress) {
            PVOID pAddress = (PVOID)address;
            fnNtProtect(ProcessHandle, &pAddress, &RepByteSize, PAGE_EXECUTE_READWRITE, &OldProtect);
            fnNtWrite(ProcessHandle, (PVOID)address, ReplaceAob, RepByteSize, 0);
            fnNtProtect(ProcessHandle, &pAddress, &RepByteSize, OldProtect, &OldProtect);
        }
        return true;
    }

    bool FindPattern(DWORD_PTR StartRange, DWORD_PTR EndRange, BYTE* SearchBytes, std::vector<DWORD_PTR>& AddressRet) {
        BYTE* pCurrMemoryData = nullptr;
        MEMORY_BASIC_INFORMATION mbi;
        std::vector<MEMORY_REGION> m_vMemoryRegion;
        mbi.RegionSize = 0x1000;
        DWORD_PTR dwAddress = StartRange;
        SIZE_T nSearchSize = _msize(SearchBytes);

        while (VirtualQueryEx(ProcessHandle, (LPCVOID)dwAddress, &mbi, sizeof(mbi)) && (dwAddress < EndRange)) {
            if ((mbi.State == MEM_COMMIT) && ((mbi.Protect & PAGE_GUARD) == 0) && (mbi.Protect != PAGE_NOACCESS)) {
                MEMORY_REGION mData = { (DWORD_PTR)mbi.BaseAddress, mbi.RegionSize };
                m_vMemoryRegion.push_back(mData);
            }
            dwAddress = (DWORD_PTR)mbi.BaseAddress + mbi.RegionSize;
        }
        for (const auto& mData : m_vMemoryRegion) {
            SIZE_T dwNumberOfBytesRead = 0;
            pCurrMemoryData = new BYTE[mData.dwMemorySize];
            ZeroMemory(pCurrMemoryData, mData.dwMemorySize);
            fnNtRead(ProcessHandle, (PVOID)mData.dwBaseAddr, pCurrMemoryData, mData.dwMemorySize, &dwNumberOfBytesRead);

            if ((int)dwNumberOfBytesRead > 0) {
                DWORD_PTR dwOffset = 0;
                int iOffset = Memfind(pCurrMemoryData, dwNumberOfBytesRead, SearchBytes, nSearchSize);
                while (iOffset != -1) {
                    dwOffset += iOffset;
                    AddressRet.push_back(dwOffset + mData.dwBaseAddr);
                    dwOffset += nSearchSize;
                    iOffset = Memfind(pCurrMemoryData + dwOffset, dwNumberOfBytesRead - dwOffset - nSearchSize, SearchBytes, nSearchSize);
                }
            }
            delete[] pCurrMemoryData;
        }
        return true;
    }

    int Memfind(BYTE* buffer, SIZE_T dwBufferSize, BYTE* bstr, SIZE_T dwStrLen) {
        for (SIZE_T i = 0; i < dwBufferSize; i++) {
            for (SIZE_T j = 0; j < dwStrLen; j++) {
                if (buffer[i + j] != bstr[j] && bstr[j] != '?') break;
                if (j == dwStrLen - 1) return i;
            }
        }
        return -1;
    }
};