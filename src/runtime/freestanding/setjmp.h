// <setjmp.h> for the game code DLL (compiled without the MSVC and Windows SDK headers, see <cmath> here): the
// x64 jmp_buf exactly as vcruntime's setjmp.h has it (rt::Landing returns one, and its mangled name holds the
// struct's tag), and _setjmp, which clang turns into the returns-twice call with the frame address the CRT's longjmp
// unwinds to. The executable exports its own C runtime's _setjmp (src/runtime/runtime.cpp), so the setjmp in the
// generated code and the longjmp in rt::Call are one implementation, as when both lived in the executable.
#pragma once

typedef struct alignas(16) _SETJMP_FLOAT128 {
    unsigned long long Part[2];
} SETJMP_FLOAT128;

#define _JBLEN 16
typedef SETJMP_FLOAT128 _JBTYPE;
typedef _JBTYPE jmp_buf[_JBLEN];

#define setjmp _setjmp

extern "C" int __cdecl setjmp(jmp_buf buf);
