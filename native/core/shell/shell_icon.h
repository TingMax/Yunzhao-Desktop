// Shell item and executable icon loading.
//
// Upstream 3.0 split these out of shell/task_model.h into this file. Yunzhao keeps the
// implementations where they have always lived (shell/task_model.cpp) and this header forwards
// to them, so both the old callers and the new ones see the same functions.
#pragma once

#include "shell/task_model.h"

namespace yunzhao {

// Yunzhao's own programs, under the upstream name LoadAtriumIcon's callers expect.
inline bool LoadAtriumIcon(const std::wstring& exePath, int sizePx, IconImage& out) { return LoadYunzhaoIcon(exePath, sizePx, out); }

} // namespace yunzhao
