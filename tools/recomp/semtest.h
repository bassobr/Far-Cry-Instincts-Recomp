#pragma once

#include <string>

#include "codegen.h"

namespace recomp {

// writes the generated half of the instruction semantics test (see tools/semtest)
int WriteSemanticsTest(Codegen& cg, const std::string& path);

}  // namespace recomp
