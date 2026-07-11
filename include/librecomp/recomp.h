#pragma once
// The N64Recomp vintage paired with the gamemodes runtime branch emits
// generated code that includes "librecomp/recomp.h", but ships the header
// flat at N64Recomp/include/recomp.h — this shim bridges the layout
// (include/ and N64Recomp/include are both on the generated-code targets'
// include paths).
// Angle include: a quote include would resolve to this shim itself
// (same-directory-first) and #pragma once would turn it into a no-op.
#include <recomp.h>
