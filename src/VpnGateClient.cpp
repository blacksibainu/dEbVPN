// VpnGateClient.cpp
// winsock2.h должен идти раньше windows.h (иначе конфликт с legacy winsock.h,
// которое подтягивает Utf8.h через <windows.h>).
#include <winsock2.h>
#include <windows.h>

#include "VpnGateClient.h"
#include "Utf8.h"

#include <winhttp.h>
#include <wincrypt.h>
#include <ws2tcpip.h>

#include <array>
#include <chrono>
#include <sstream>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace tvpn {

namespace {

std::vector<std::string> SplitCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    size_t start = 0;
    for (;;) {
        size_t comma = line.find(',', start);
        if (comma == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, comma - start));
        start = comma + 1;
    }
    return fields;
}

long ToLong(const std::string& s) {
    try {
        return s.empty() ? 0 : std::stol(s);
    } catch (...) {
        return 0;
    }
}

// UDP-туннель лучше для игр — TCP-в-TCP (игровой UDP внутри TCP-тоннеля) даёт задержки/джиттер.
std::string ExtractProtocol(const std::string& decodedConfig) {
    size_t pos = decodedConfig.find("proto ");
    if (pos == std::string::npos) return "";
    size_t start = pos + 6;
    size_t end = decodedConfig.find_first_of("\r\n", start);
    std::string val = decodedConfig.substr(start, end == std::string::npos ? std::string::npos : end - start);
    while (!val.empty() && isspace((unsigned char)val.back())) val.pop_back();
    for (auto& ch : val) ch = (char)tolower((unsigned char)ch);
    if (val == "tcp" || val == "tcp-client") return "tcp";
    if (val == "udp") return "udp";
    return "";
}

// VPN Gate публикует полные названия стран по-английски; сводим самые
// частые к ISO-коду, который уже понимает CountryData.h/JS-карта.
// Остальные (менее популярные) просто пропускаем без точки на карте — они
// всё равно попадут в общий список по имени.
std::string GuessCountryCode(const std::string& countryLong) {
    static const std::pair<const char*, const char*> kMap[] = {
        {"United States", "us"}, {"Japan", "jp"}, {"Korea Republic of", "kr"},
        {"Korea, Republic of", "kr"}, {"China", "cn"}, {"Germany", "de"},
        {"France", "fr"}, {"United Kingdom", "gb"}, {"Netherlands", "nl"},
        {"Russian Federation", "ru"}, {"Russia", "ru"}, {"Canada", "ca"},
        {"Brazil", "br"}, {"India", "in"}, {"Indonesia", "id"},
        {"Thailand", "th"}, {"Vietnam", "vn"}, {"Singapore", "sg"},
        {"Taiwan", "tw"}, {"Hong Kong", "hk"}, {"Australia", "au"},
        {"Iran, ISLAMIC Republic Of", "ir"}, {"Iran (ISLAMIC Republic Of)", "ir"},
        {"Ukraine", "ua"}, {"Poland", "pl"}, {"Spain", "es"}, {"Italy", "it"},
        {"Sweden", "se"}, {"Norway", "no"}, {"Finland", "fi"}, {"Turkey", "tr"},
        {"Mexico", "mx"}, {"Argentina", "ar"}, {"South Africa", "za"},
        {"Malaysia", "my"}, {"Philippines", "ph"}, {"Romania", "ro"},
        {"Switzerland", "ch"}, {"Austria", "at"}, {"Belgium", "be"},
        {"Czechia", "cz"}, {"Czech Republic", "cz"}, {"Israel", "il"},
        {"Kazakhstan", "kz"},
    };
    for (const auto& [name, code] : kMap) {
        if (countryLong == name) return code;
    }
    return "";
}

// Пробует TCP-подключение параллельно ко всем IPv4-адресам хоста, берёт первый ответивший.
std::string RaceForWorkingIPv4(const std::string& host, int port, int timeoutMs) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addrList = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addrList) != 0 || !addrList) {
        return "";
    }

    std::vector<SOCKET> sockets;
    std::vector<std::string> ips;
    for (addrinfo* p = addrList; p; p = p->ai_next) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        connect(s, p->ai_addr, (int)p->ai_addrlen);  // неблокирующий: сразу возвращает WSAEWOULDBLOCK

        char ipStr[INET_ADDRSTRLEN]{};
        auto* sa = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        InetNtopA(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));

        sockets.push_back(s);
        ips.push_back(ipStr);
    }
    freeaddrinfo(addrList);
    if (sockets.empty()) return "";

    std::string winner;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (winner.empty() && std::chrono::steady_clock::now() < deadline) {
        fd_set writeSet, errSet;
        FD_ZERO(&writeSet);
        FD_ZERO(&errSet);
        for (auto s : sockets) {
            FD_SET(s, &writeSet);
            FD_SET(s, &errSet);
        }
        timeval tv{0, 200000};  // проверяем готовность раз в 200мс
        if (select(0, nullptr, &writeSet, &errSet, &tv) > 0) {
            for (size_t i = 0; i < sockets.size(); i++) {
                if (FD_ISSET(sockets[i], &writeSet)) {
                    int err = 0;
                    int len = sizeof(err);
                    getsockopt(sockets[i], SOL_SOCKET, SO_ERROR, (char*)&err, &len);
                    if (err == 0) {
                        winner = ips[i];
                        break;
                    }
                }
            }
        }
    }
    for (auto s : sockets) closesocket(s);
    return winner;
}

}  // namespace

std::string DecodeBase64(const std::string& base64) {
    DWORD outLen = 0;
    if (!CryptStringToBinaryA(base64.c_str(), (DWORD)base64.size(), CRYPT_STRING_BASE64, nullptr,
                              &outLen, nullptr, nullptr)) {
        return {};
    }
    std::string out(outLen, '\0');
    if (!CryptStringToBinaryA(base64.c_str(), (DWORD)base64.size(), CRYPT_STRING_BASE64,
                              (BYTE*)out.data(), &outLen, nullptr, nullptr)) {
        return {};
    }
    out.resize(outLen);
    return out;
}

std::vector<VpnGateServer> FetchVpnGateServers(std::string* diagnosticOut) {
    std::vector<VpnGateServer> result;
    auto diag = [&](const std::string& s) { if (diagnosticOut) *diagnosticOut = s; };

    // WinHTTP вместо WinINet — WinINet зависал на автоопределении прокси (WPAD) у части пользователей.
    HINTERNET hSession = WinHttpOpen(L"DebVPN/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        diag("WinHttpOpen не удался, код ошибки " + std::to_string(GetLastError()));
        return result;
    }
    WinHttpSetTimeouts(hSession, 10000, 10000, 15000, 20000);

    // Часть IP-адресов www.vpngate.net блокируется провайдером — пробуем все параллельно, берём первый живой.
    std::string workingIp = RaceForWorkingIPv4("www.vpngate.net", 443, 8000);
    diag(workingIp.empty() ? "Не нашли ни одного отвечающего адреса www.vpngate.net за 8с, пробуем по имени..."
                            : ("Найден отвечающий адрес: " + workingIp + ", подключаемся напрямую к нему"));

    std::wstring connectTarget =
        workingIp.empty() ? L"www.vpngate.net" : tvpn::Utf8ToWide(workingIp);
    HINTERNET hConnect = WinHttpConnect(hSession, connectTarget.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        diag("WinHttpConnect не удался, код ошибки " + std::to_string(GetLastError()));
        WinHttpCloseHandle(hSession);
        return result;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/api/iphone/", nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        diag("WinHttpOpenRequest не удался, код ошибки " + std::to_string(GetLastError()));
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    if (!workingIp.empty()) {
        // Подключение по голому IP — сертификат выписан на домен, отключаем только проверку имени.
        DWORD secFlags = SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
        WinHttpAddRequestHeaders(hRequest, L"Host: www.vpngate.net", (DWORD)-1,
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    BOOL sent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    BOOL received = sent && WinHttpReceiveResponse(hRequest, nullptr);

    if (!received) {
        diag("Сетевой запрос не удался, код ошибки " + std::to_string(GetLastError()));
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    DWORD httpStatus = 0, statusSize = sizeof(httpStatus);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &httpStatus, &statusSize, WINHTTP_NO_HEADER_INDEX);

    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &available) || available == 0) break;
        std::vector<char> buf(available);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, buf.data(), available, &bytesRead) || bytesRead == 0) break;
        body.append(buf.data(), bytesRead);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    diag("HTTP " + std::to_string(httpStatus) + ", получено байт: " + std::to_string(body.size()));

    // Служебные строки (начинаются на '*' или '#') пропускаем, остальное — CSV-записи.
    std::istringstream stream(body);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '*' || line[0] == '#') continue;

        auto f = SplitCsvLine(line);
        // #HostName,IP,Score,Ping,Speed,CountryLong,CountryShort,NumVpnSessions,
        // Uptime,TotalUsers,TotalTraffic,LogType,Operator,Message,OpenVPN_ConfigData_Base64
        if (f.size() < 15) continue;

        VpnGateServer s;
        s.hostName = f[0];
        s.ip = f[1];
        s.score = ToLong(f[2]);
        s.pingMs = ToLong(f[3]);
        s.speedBps = ToLong(f[4]);
        s.countryLong = f[5];
        s.countryShort = GuessCountryCode(f[5]);
        if (s.countryShort.empty()) {
            // запасной вариант: CountryShort у VPN Gate — двухбуквенный код в верхнем регистре
            std::string cs = f[6];
            for (auto& ch : cs) ch = (char)tolower((unsigned char)ch);
            s.countryShort = cs;
        }
        s.numSessions = ToLong(f[7]);
        s.uptimeMs = ToLong(f[8]);
        s.operatorName = f[12];
        s.openvpnConfigBase64 = f[14];
        s.protocol = ExtractProtocol(DecodeBase64(s.openvpnConfigBase64));

        if (s.openvpnConfigBase64.empty() || s.ip.empty()) continue;
        result.push_back(std::move(s));
    }

    diag("HTTP " + std::to_string(httpStatus) + ", байт: " + std::to_string(body.size()) +
        ", разобрано серверов: " + std::to_string(result.size()));
    return result;
}

}  // namespace tvpn
