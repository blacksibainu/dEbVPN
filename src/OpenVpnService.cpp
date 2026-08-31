// OpenVpnService.cpp
#include "OpenVpnService.h"  // подключает winsock2.h раньше windows.h
#include "Utf8.h"

#include <ws2tcpip.h>
#include <shellapi.h>
#include <iphlpapi.h>

#include <array>
#include <fstream>
#include <vector>

#include <nlohmann/json.hpp>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "iphlpapi.lib")

using json = nlohmann::json;

namespace tvpn {

namespace {

constexpr wchar_t kTaskName[] = L"DebVpnOpenVpnLauncher";
constexpr int kFixedMgmtPort = 25340;  // фиксированный порт — команда задачи в Планировщике статична
constexpr wchar_t kTunAdapterName[] = L"DebVpnTun";

// openvpn.exe сам не создаёт Wintun-адаптер, а без --dev-node ищет "любой
// свободный" — конфликтует с чужими VPN и падает, если ни одного нет.
bool TunAdapterExists(const wchar_t* name) {
    ULONG sz = 15000;
    std::vector<uint8_t> buf(sz);
    ULONG flags = GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_ALL_INTERFACES;
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, aa, &sz) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(sz);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    }
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, aa, &sz) != NO_ERROR) return false;

    for (auto* p = aa; p; p = p->Next) {
        if (p->FriendlyName && wcscmp(p->FriendlyName, name) == 0) return true;
    }
    return false;
}

// Создаёт именованный Wintun-адаптер через tapctl.exe — только один раз,
// при первом подключении (tapctl.exe требует прав администратора).
bool CreateTunAdapter(const std::wstring& binDir) {
    std::wstring tapctl = binDir + L"\\tapctl.exe";
    std::wstring params = L"create --hwid wintun --name \"" + std::wstring(kTunAdapterName) + L"\"";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = tapctl.c_str();
    sei.lpParameters = params.c_str();
    sei.lpDirectory = binDir.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return false;

    WaitForSingleObject(sei.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}

// Запускает команду и ждёт завершения, возвращает код выхода (-1 при ошибке запуска).
int RunAndWait(std::wstring cmdLine, DWORD timeoutMs) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

// Регистрирует задачу с /rl highest — так подключение не спрашивает UAC
// каждый раз. Команда задачи статична, поэтому порт управления фиксированный.
bool EnsureScheduledTask(const std::wstring& ovpnExe, const std::wstring& configPath, const std::wstring& logPath) {
    std::wstring action = L"\\\"" + ovpnExe + L"\\\" --config \\\"" + configPath + L"\\\" --log \\\"" + logPath +
                          L"\\\" --management 127.0.0.1 " + std::to_wstring(kFixedMgmtPort) +
                          L" --management-hold --verb 3";
    std::wstring cmd = L"schtasks /create /f /tn \"" + std::wstring(kTaskName) + L"\" /tr \"" + action +
                       L"\" /sc once /sd 01/01/1990 /st 00:00 /rl highest /it";
    return RunAndWait(cmd, 15000) == 0;
}

bool RunScheduledTask() {
    std::wstring cmd = L"schtasks /run /tn \"" + std::wstring(kTaskName) + L"\"";
    return RunAndWait(cmd, 15000) == 0;
}

int FindFreeTcpPort() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return 0;
    }
    int len = sizeof(addr);
    getsockname(s, (sockaddr*)&addr, &len);
    int port = ntohs(addr.sin_port);
    closesocket(s);
    return port;
}

std::vector<std::string> SplitCsv(const std::string& line) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t comma = line.find(',', start);
        parts.push_back(line.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return parts;
}

}  // namespace

OpenVpnController::OpenVpnController(Config cfg, OvpnEventCallback onEvent)
    : cfg_(std::move(cfg)), cb_(std::move(onEvent)) {
    worker_ = std::thread(&OpenVpnController::WorkerLoop, this);
}

OpenVpnController::~OpenVpnController() {
    Shutdown();
}

void OpenVpnController::EnqueueJob(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.push_back(std::move(job));
    }
    queueCv_.notify_one();
}

void OpenVpnController::WorkerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] { return !queue_.empty() || !workerAlive_; });
            if (!workerAlive_ && queue_.empty()) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        if (job) job();
    }
}

void OpenVpnController::FetchServerListAsync() {
    EnqueueJob([this] { DoFetchServerList(); });
}

void OpenVpnController::ConnectByHostNameAsync(const std::string& hostName) {
    EnqueueJob([this, hostName] {
        VpnGateServer server;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(serversMutex_);
            for (const auto& s : lastServers_) {
                if (s.hostName == hostName) {
                    server = s;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            EmitStatus("error", "Сервер не найден в загруженном списке — обновите список.");
            return;
        }
        DoConnect(server);
    });
}

void OpenVpnController::DisconnectAsync() {
    EnqueueJob([this] { DoDisconnect(); });
}

void OpenVpnController::DoFetchServerList() {
    EmitStatus("fetching", "Получение списка серверов VPN Gate (может занять до 30 секунд)...");
    std::string diagnostic;
    auto servers = FetchVpnGateServers(&diagnostic);
    if (!diagnostic.empty()) EmitLog(diagnostic);
    if (servers.empty()) {
        EmitStatus("error", "Не удалось получить список серверов VPN Gate: " + diagnostic);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(serversMutex_);
        lastServers_ = servers;
    }
    EmitServers(servers);
    EmitStatus("idle", "Готово. Выберите сервер для подключения.");
}

void OpenVpnController::DoConnect(VpnGateServer server) {
    // Защита от повторного клика "Подключиться" — без неё старый поток/сокет теряется.
    if (mgmtReaderRun_ || mgmtSocket_ != INVALID_SOCKET || ovpnProcess_) {
        DoDisconnect();
    }

    EmitStatus("connecting", "Подключение через " + server.hostName + " (" + server.countryLong +
                                 ") — подтвердите запрос прав администратора...");

    std::string ovpnText = DecodeBase64(server.openvpnConfigBase64);
    if (ovpnText.empty()) {
        EmitStatus("error", "Не удалось декодировать конфигурацию сервера.");
        return;
    }
    // --dev-node — свой именованный адаптер, без него openvpn.exe ищет "любой свободный".
    ovpnText += "\nwindows-driver wintun\ndev-node " + tvpn::WideToUtf8(kTunAdapterName) + "\n";

    // Дефолтные сокет-буферы OpenVPN на Windows — 16 МБ (bufferbloat, плохо для пинга в играх).
    ovpnText += "sndbuf 65536\nrcvbuf 65536\n";
    // fast-io снижает задержку I/O, но несовместим с --proto tcp.
    if (server.protocol == "udp") {
        ovpnText += "fast-io\n";
    }

    // DNS leak protection на уровне Mullvad: весь трафик и DNS идут через
    // туннель, block-outside-dns сам ставит/снимает WFP-фильтры на утечки.
    ovpnText += "redirect-gateway def1\n";
    ovpnText += "dhcp-option DNS 1.1.1.1\ndhcp-option DNS 1.0.0.1\n";
    ovpnText += "register-dns\n";
    ovpnText += "block-outside-dns\n";

    // VPN Gate требует непустую пару логин/пароль, но значения не проверяет.
    CreateDirectoryW(cfg_.workDir.c_str(), nullptr);
    std::wstring authPath = cfg_.workDir + L"\\vpngate_auth.txt";
    {
        std::ofstream f(authPath, std::ios::binary | std::ios::trunc);
        f << "vpn\nvpn\n";
    }
    // Внутри .ovpn-файла (в отличие от argv) '\' — escape-символ, ломает путь.
    std::string authPathForConfig = tvpn::WideToUtf8(authPath);
    for (auto& ch : authPathForConfig) {
        if (ch == '\\') ch = '/';
    }
    ovpnText += "auth-user-pass \"" + authPathForConfig + "\"\n";

    if (!TunAdapterExists(kTunAdapterName)) {
        EmitLog("Создаю Wintun-адаптер " + tvpn::WideToUtf8(kTunAdapterName) +
                " (нужно один раз, потребуется UAC)...");
        if (!CreateTunAdapter(cfg_.ovpnBinDir)) {
            EmitStatus("error", "Не удалось создать Wintun-адаптер через tapctl.exe.");
            return;
        }
    }

    std::wstring configPath = cfg_.workDir + L"\\vpngate.ovpn";
    std::wstring logPath = cfg_.workDir + L"\\vpngate.log";
    {
        std::ofstream f(configPath, std::ios::binary | std::ios::trunc);
        if (!f) {
            EmitStatus("error", "Нет доступа к рабочей папке " + tvpn::WideToUtf8(cfg_.workDir));
            return;
        }
        f.write(ovpnText.data(), (std::streamsize)ovpnText.size());
    }

    std::wstring ovpnExe = cfg_.ovpnBinDir + L"\\openvpn.exe";
    int port = kFixedMgmtPort;
    bool launchedViaTask = false;

    // Основной путь — через Планировщик, чтобы не спрашивать UAC каждый раз.
    if (EnsureScheduledTask(ovpnExe, configPath, logPath) && RunScheduledTask()) {
        launchedViaTask = true;
    } else {
        // Запасной путь: прямой запуск с запросом прав при каждом подключении.
        EmitLog("Не удалось запустить через Планировщик заданий, пробуем напрямую (потребуется UAC)...");
        port = FindFreeTcpPort();
        if (port == 0) {
            EmitStatus("error", "Не удалось выделить локальный порт для управления OpenVPN (код ошибки Winsock " +
                                    std::to_string(WSAGetLastError()) + ").");
            return;
        }

        std::wstring params;
        params += L"--config \"" + configPath + L"\" ";
        params += L"--log \"" + logPath + L"\" ";
        params += L"--management 127.0.0.1 " + std::to_wstring(port) + L" ";
        params += L"--management-hold --verb 3";

        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = ovpnExe.c_str();
        sei.lpParameters = params.c_str();
        sei.lpDirectory = cfg_.ovpnBinDir.c_str();  // чтобы wintun.dll и остальные DLL нашлись рядом
        sei.nShow = SW_HIDE;  // видимая консоль рискует зависнуть в режиме QuickEdit при клике

        EmitLog("Вызываю ShellExecuteExW(runas) для " + tvpn::WideToUtf8(ovpnExe) + "...");
        BOOL seeOk = ShellExecuteExW(&sei);
        DWORD seeErr = GetLastError();
        EmitLog("ShellExecuteExW вернул " + std::string(seeOk ? "TRUE" : "FALSE") +
                ", hProcess=" + (sei.hProcess ? "есть" : "null") +
                ", GetLastError=" + std::to_string(seeErr));
        if (!seeOk || !sei.hProcess) {
            EmitStatus("error", "Запуск отменён или не удалось запросить права администратора (код " +
                                     std::to_string(seeErr) + ").");
            return;
        }
        ovpnProcess_ = sei.hProcess;
    }
    (void)launchedViaTask;  // процесс дальше отслеживаем через management-сокет в обоих случаях

    if (!ConnectManagementSocket(port)) {
        EmitStatus("error", "Не удалось подключиться к управляющему интерфейсу OpenVPN.");
        if (ovpnProcess_) {
            TerminateProcess(ovpnProcess_, 1);
            CloseHandle(ovpnProcess_);
            ovpnProcess_ = nullptr;
        }
        return;
    }

    lastCountry_ = server.countryLong;
    mgmtReaderRun_ = true;
    mgmtReaderThread_ = std::thread(&OpenVpnController::ManagementReaderLoop, this);
}

bool OpenVpnController::ConnectManagementSocket(int port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    // Сокет с неудачным connect() по Winsock больше не годен для повтора — пересоздаём каждый раз.
    for (int i = 0; i < 75; i++) {  // до ~15с — openvpn.exe нужно время подняться
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;
        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
            mgmtSocket_ = s;
            return true;
        }
        closesocket(s);
        Sleep(200);
    }
    return false;
}

void OpenVpnController::ManagementReaderLoop() {
    auto sendLine = [this](const std::string& line) {
        EmitLog("> " + line);
        std::string l = line + "\r\n";
        int sent = send(mgmtSocket_, l.c_str(), (int)l.size(), 0);
        if (sent == SOCKET_ERROR) {
            EmitLog("> (ошибка отправки, WSA " + std::to_string(WSAGetLastError()) + ")");
        }
    };
    sendLine("state on");
    // ">HOLD:" уходит "в никуда" ещё до нашего подключения — не ждём его, шлём release сразу.
    sendLine("hold release");

    std::string buf;
    std::array<char, 1024> chunk{};

    while (mgmtReaderRun_) {
        int n = recv(mgmtSocket_, chunk.data(), (int)chunk.size(), 0);
        if (n <= 0) break;
        buf.append(chunk.data(), n);

        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            EmitLog(line);

            if (line.rfind(">HOLD:", 0) == 0) {
                sendLine("hold release");
            } else if (line.rfind(">STATE:", 0) == 0) {
                auto parts = SplitCsv(line.substr(7));
                std::string state = parts.size() > 1 ? parts[1] : "";
                if (state == "CONNECTED") {
                    std::string ip = parts.size() > 3 ? parts[3] : "";
                    connected_ = true;
                    EmitConnected(lastCountry_, ip);
                    EmitStatus("connected", "Подключено (" + lastCountry_ + ")");
                } else if (state == "EXITING") {
                    mgmtReaderRun_ = false;
                    if (connected_) {
                        connected_ = false;
                        EmitDisconnected();
                    }
                    EmitStatus("idle", "Соединение завершено.");
                } else if (!state.empty()) {
                    EmitStatus("connecting", "OpenVPN: " + state);
                }
            }
        }
    }
}

void OpenVpnController::DoDisconnect() {
    mgmtReaderRun_ = false;

    if (mgmtSocket_ != INVALID_SOCKET) {
        std::string cmd = "signal SIGTERM\r\n";
        send(mgmtSocket_, cmd.c_str(), (int)cmd.size(), 0);
        closesocket(mgmtSocket_);
        mgmtSocket_ = INVALID_SOCKET;
    }
    if (mgmtReaderThread_.joinable()) mgmtReaderThread_.join();

    if (ovpnProcess_) {
        // Ждём мягкой остановки после SIGTERM, иначе завершаем принудительно.
        if (WaitForSingleObject(ovpnProcess_, 3000) != WAIT_OBJECT_0) {
            TerminateProcess(ovpnProcess_, 0);
        }
        CloseHandle(ovpnProcess_);
        ovpnProcess_ = nullptr;
    }

    bool wasConnected = connected_.exchange(false);
    if (wasConnected) EmitDisconnected();
    EmitStatus("idle", "Отключено.");
}

void OpenVpnController::Shutdown() {
    if (connected_ || mgmtSocket_ != INVALID_SOCKET || ovpnProcess_) {
        DoDisconnect();
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.clear();
    }
    workerAlive_ = false;
    queueCv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void OpenVpnController::EmitStatus(const std::string& state, const std::string& message) {
    json j;
    j["type"] = "ovpn_status";
    j["state"] = state;
    j["message"] = message;
    if (cb_) cb_(j.dump());
}

void OpenVpnController::EmitLog(const std::string& message) {
    json j;
    j["type"] = "ovpn_log";
    j["message"] = message;
    if (cb_) cb_(j.dump());
}

void OpenVpnController::EmitServers(const std::vector<VpnGateServer>& servers) {
    json arr = json::array();
    for (const auto& s : servers) {
        arr.push_back({
            {"hostName", s.hostName},
            {"ip", s.ip},
            {"countryLong", s.countryLong},
            {"countryShort", s.countryShort},
            {"score", s.score},
            {"pingMs", s.pingMs},
            {"speedMbps", s.speedBps / 1000000},
            {"numSessions", s.numSessions},
            {"protocol", s.protocol},
        });
    }
    json j;
    j["type"] = "ovpn_servers";
    j["servers"] = arr;
    if (cb_) cb_(j.dump());
}

void OpenVpnController::EmitConnected(const std::string& country, const std::string& ip) {
    json j;
    j["type"] = "ovpn_connected";
    j["country"] = country;
    j["ip"] = ip;
    if (cb_) cb_(j.dump());
}

void OpenVpnController::EmitDisconnected() {
    json j;
    j["type"] = "ovpn_disconnected";
    if (cb_) cb_(j.dump());
}

}  // namespace tvpn
