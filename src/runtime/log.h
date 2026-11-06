#pragma once

#include <cstdint>
#include <string>

namespace rt {

// writes to the console and fci.log beside the executable
void Log(const char* fmt, ...);
// logs, prints the host call stack (recompiled functions show as f_<guest address>) and exits
[[noreturn]] void Fatal(const char* fmt, ...);

void InitLog();
// writes the queued lines now (a thread writes them otherwise): before the process ends
void FlushLog();
// CONTEXT* of a fault (or of a suspended thread), or nullptr for the current stack
void PrintStack(void* context, void* thread = nullptr);
std::string FormatStack(void* context, void* thread = nullptr);  // without taking the log lock
std::string SymbolAt(uint64_t host_address);  // "f_004B01C8+0x1A1"

}  // namespace rt
