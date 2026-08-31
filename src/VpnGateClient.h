// VpnGateClient.h
//
// Клиент публичного списка серверов VPN Gate (академический проект
// университета Цукуба, Япония) — http://www.vpngate.net/api/iphone/.
// Список отдаётся простым CSV, последнее поле каждой строки — конфиг
// OpenVPN в Base64, готовый к использованию как есть.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tvpn {

struct VpnGateServer {
    std::string hostName;
    std::string ip;
    std::string countryLong;
    std::string countryShort;  // ISO 3166-1 alpha-2, как в CountryData.h (в нижнем регистре)
    std::string operatorName;
    long score = 0;
    long speedBps = 0;
    long pingMs = 0;
    long numSessions = 0;
    long uptimeMs = 0;
    std::string protocol;  // "tcp" или "udp" — из строки "proto ..." конфига сервера
    std::string openvpnConfigBase64;
};

// Загружает и разбирает список серверов. Возвращает пустой вектор при
// сетевой ошибке. Блокирующий вызов — предполагается запуск из фонового потока.
// diagnosticOut, если передан, заполняется подробностями (код ошибки/HTTP-
// статус, число полученных байт, число разобранных строк) — для журнала.
std::vector<VpnGateServer> FetchVpnGateServers(std::string* diagnosticOut = nullptr);

// Декодирует Base64-конфиг конкретного сервера в текст .ovpn.
std::string DecodeBase64(const std::string& base64);

}  // namespace tvpn
