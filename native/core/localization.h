// Interface strings.
//
// Upstream 3.0 reads these from locales/<language>.json so the bar, menus and popups switch
// language with Windows. Yunzhao ships one language: Tr() returns the key it was given, which the
// call sites already write in the form the user reads (English in the new modules, Chinese in the
// rest of the product). Adding a language later means filling a table here, not touching call
// sites.
#pragma once

#include <string>
#include <string_view>

namespace yunzhao {

inline std::wstring Tr(std::wstring_view text) { return std::wstring(text); }

} // namespace yunzhao
