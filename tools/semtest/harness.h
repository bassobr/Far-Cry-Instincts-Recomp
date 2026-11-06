// Shared between the generated cases (generated/semtest_cases.cpp) and the test driver.
#pragma once

#include <cstddef>
#include <cstdint>

#include "runtime/guest.h"

struct TestCase {
    const char* text;
    void (*emulate)(rt::Cpu& c, rt::Flags& f);
    uint8_t native[16];  // the instruction encoded for x86-64
    size_t length;
    uint32_t flags;      // status flags the instruction defines
    uint32_t undefined;  // and those it leaves undefined
    int mnemonic;        // ZydisMnemonic
};

extern const TestCase kCases[];
extern const size_t kCaseCount;
