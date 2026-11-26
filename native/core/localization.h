// Interface strings.
//
// Upstream 3.0 reads these from locales/<language>.json so the bar, menus and popups switch
// language with Windows (the context argument tells apart the same English word in different
// places). Yunzhao ships one language: Tr() returns the key it was given, which the call sites
// already write in the form the user reads (English in the new modules, Chinese in the rest of
// the product). Adding a language later means filling a table here, not touching call sites.
#pragma once

#include <string>

namespace yunzhao {

inline std::wstring Tr(const wchar_t* key, const wchar_t* = nullptr) { return std::wstring(key ? key : L""); }

} // namespace yunzhao
