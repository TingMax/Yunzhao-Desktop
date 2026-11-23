// Append-only log at %LOCALAPPDATA%\Yunzhao\logs\core.log, written by a thread of its own.
#pragma once

namespace yunzhao {

void Log(const wchar_t* format, ...);

} // namespace yunzhao
