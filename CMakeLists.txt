# CMakeLists.txt — сборка DebVPN (Windows, MSVC, WebView2 + VPN Gate/OpenVPN).
#
# Сборка (из папки проекта, PowerShell):
#   cmake --preset default
#   cmake --build --preset default --config Release
#
# Зависимости ставятся через vcpkg-манифест (vcpkg.json) автоматически при
# конфигурировании, если передан VCPKG_ROOT / toolchain-файл (см. CMakePresets.json).

cmake_minimum_required(VERSION 3.21)
project(DebVpn CXX)

if (NOT WIN32)
    message(FATAL_ERROR "DebVPN использует WebView2 и Win32 API — сборка только под Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(unofficial-webview2 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(DebVpn WIN32
    src/main.cpp
    src/VpnGateClient.cpp
    src/OpenVpnService.cpp
)

target_include_directories(DebVpn PRIVATE src)

target_compile_options(DebVpn PRIVATE
    /utf-8      # исходники и строковые литералы в UTF-8 (комментарии и тексты на русском)
    /W4
)

target_compile_definitions(DebVpn PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
)

target_link_libraries(DebVpn PRIVATE
    unofficial::webview2::webview2
    nlohmann_json::nlohmann_json
    ws2_32
    dwmapi     # тёмная системная рамка окна (DwmSetWindowAttribute)
    winhttp    # загрузка списка серверов VPN Gate (WinHttpOpen/...)
    crypt32    # Base64-декодирование конфигов VPN Gate (CryptStringToBinary)
    shell32    # ShellExecuteEx("runas") — запрос прав на запуск openvpn.exe
    iphlpapi   # проверка наличия своего Wintun-адаптера (GetAdaptersAddresses)
)

# Приложение сознательно НЕ запрашивает права администратора на весь процесс
# (WebView2/Chromium официально не поддерживает запуск от имени администратора —
# на этом словили реальный краш при тестировании). Вместо этого права
# запрашиваются точечно, только на сам openvpn.exe, при каждом подключении —
# см. OpenVpnController::DoConnect в OpenVpnService.cpp.

# Бинарники OpenVPN (openvpn.exe, DLL, wintun.dll) должны лежать рядом с
# итоговым .exe в подпапке openvpn\bin.
add_custom_command(TARGET DebVpn POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/openvpn"
        "$<TARGET_FILE_DIR:DebVpn>/openvpn"
)

# WebView2Loader.dll должен лежать рядом с итоговым .exe
add_custom_command(TARGET DebVpn POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:unofficial::webview2::webview2>"
        "$<TARGET_FILE_DIR:DebVpn>"
    COMMAND_EXPAND_LISTS
)
