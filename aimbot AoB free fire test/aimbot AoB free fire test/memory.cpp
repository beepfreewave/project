#include "memory.h"

AimbotHeadScan::Config AimbotHeadScan::cfg;
std::vector<AimbotHeadScan::AddrPair> AimbotHeadScan::pairs;
std::atomic<bool> AimbotHeadScan::scanned{ false };
std::atomic<bool> AimbotHeadScan::enabled{ false };
std::atomic<bool> AimbotHeadScan::running{ false };
HANDLE AimbotHeadScan::hProcess = nullptr;
DWORD AimbotHeadScan::pid = 0;
std::mutex AimbotHeadScan::pairsMutex;
std::thread AimbotHeadScan::sustainThread;
std::vector<AimbotHeadScan::Region> AimbotHeadScan::hotRegions;