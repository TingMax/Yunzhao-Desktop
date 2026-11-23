#pragma once

#include <string>

namespace yunzhao {

inline constexpr wchar_t kAppId[] = L"Yunzhao.Desktop";

bool IsRegisteredApplication(const std::wstring& directory);
// Development builds never register, including complete development packages.
void RegisterApplication(const std::wstring& directory);

} // namespace yunzhao
