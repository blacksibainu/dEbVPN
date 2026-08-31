// OpenVpnService.h
//
// Подключение к серверам VPN Gate через встроенный openvpn.exe.
//
// Изначально планировалась установка официальной Windows-службы OpenVPN
// ("Interactive Service", openvpnserv2.exe) — так работает официальный
// OpenVPN GUI, без запроса прав на каждое подключение. Отказались: тот
// openvpnserv2.exe, что реально лежит в MSI-дистрибутиве 2.6.14, оказался
// .NET/Mono-сборкой с другим протоколом именованного канала, а не тем,
// что описан в открытых исходниках (interactive.c) — переписывать под
// неизвестный протокол смысла нет.
//
// Вместо этого — просто: openvpn.exe умеет поднимать TUN-адаптер только
// с правами администратора, поэтому каждое подключение запускает его
// через ShellExecute("runas") — один запрос UAC на подключение. Управляем
// уже запущенным процессом через стандартный OpenVPN Management Interface
// (обычный TCP на localhost, без пароля — соединение доверенное, второй
// конец процесса создали мы же).

#pragma once

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "VpnGateClient.h"

namespace tvpn {

using OvpnEventCallback = std::function<void(std::string jsonUtf8)>;

class OpenVpnController {
public:
    struct Config {
        std::wstring ovpnBinDir;  // <exeDir>\openvpn\bin — здесь лежат openvpn.exe и DLL/wintun
        std::wstring workDir;     // %LOCALAPPDATA%\DebVPN\ovpn — сюда пишем конфиг и лог
    };

    OpenVpnController(Config cfg, OvpnEventCallback onEvent);
    ~OpenVpnController();

    OpenVpnController(const OpenVpnController&) = delete;
    OpenVpnController& operator=(const OpenVpnController&) = delete;

    void FetchServerListAsync();
    void ConnectByHostNameAsync(const std::string& hostName);
    void DisconnectAsync();
    void Shutdown();  // синхронно, вызывается при закрытии приложения

private:
    void EnqueueJob(std::function<void()> job);
    void WorkerLoop();

    void DoFetchServerList();
    void DoConnect(VpnGateServer server);
    void DoDisconnect();

    bool ConnectManagementSocket(int port);
    void ManagementReaderLoop();

    void EmitStatus(const std::string& state, const std::string& message);
    void EmitLog(const std::string& message);
    void EmitServers(const std::vector<VpnGateServer>& servers);
    void EmitConnected(const std::string& country, const std::string& ip);
    void EmitDisconnected();

    Config cfg_;
    OvpnEventCallback cb_;

    std::thread worker_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<std::function<void()>> queue_;
    std::atomic<bool> workerAlive_{true};

    HANDLE ovpnProcess_ = nullptr;
    SOCKET mgmtSocket_ = INVALID_SOCKET;
    std::thread mgmtReaderThread_;
    std::atomic<bool> mgmtReaderRun_{false};
    std::atomic<bool> connected_{false};
    std::string lastCountry_;

    std::mutex serversMutex_;
    std::vector<VpnGateServer> lastServers_;
};

}  // namespace tvpn
