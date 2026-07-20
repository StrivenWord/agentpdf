#include "agentpdf/util.hpp"

// Filesystem helpers live in string_util.cpp to keep util translation units small.
// This unit exists so the CMake source list matches the planned layout.
namespace agentpdf {
namespace {
const char kFsUtilAnchor = 0;
}
const char* fs_util_anchor() { return &kFsUtilAnchor; }
}  // namespace agentpdf
