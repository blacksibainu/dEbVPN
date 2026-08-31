// main.cpp
//
// Точка входа приложения DebVPN — клиент на основе VPN Gate (бесплатная
// сеть публичных серверов, университет Цукуба) с выбором страны сервера.
// Один сервер, один реальный VPN-туннель (не Tor, без множества узлов) —
// подключение через встроенный openvpn.exe. Окно на чистом Win32, внутри —
// WebView2 с интерфейсом (HTML/CSS/JS одним встроенным блоком).
//
// Мост между интерфейсом и бэкендом:
//   JS -> C++ : window.chrome.webview.postMessage(JSON.stringify({...}))
//   C++ -> JS : ICoreWebView2::PostWebMessageAsJson(...) -> событие "message"
//
// OpenVpnController работает в фоновых потоках, поэтому его события всегда
// сначала попадают в очередь оконных сообщений (PostMessageW) и уже оттуда,
// из основного потока, уходят в WebView2.

// winsock2.h должен быть включён раньше windows.h везде, где используется
// (иначе windows.h подтянет устаревший winsock.h и будет конфликт символов).
#include <winsock2.h>
#include <windows.h>

#include "Utf8.h"
#include "CountryData.h"
#include "WorldMapPath.h"
#include "OpenVpnService.h"

#include <wrl.h>
#include <WebView2.h>
#include <dwmapi.h>

#include <array>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

namespace {

constexpr UINT WM_APP_OVPN_EVENT = WM_APP + 1;
constexpr wchar_t kWindowClassName[] = L"DebVpnMainWindow";
constexpr wchar_t kWindowTitle[] = L"DebVPN — VPN Gate";

HWND g_hwnd = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
std::unique_ptr<tvpn::OpenVpnController> g_ovpn;

// Каталог, где лежит сам .exe (там же подпапка "openvpn" с openvpn.exe).
std::wstring GetExeDir() {
    std::array<wchar_t, MAX_PATH> path{};
    DWORD len = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
    std::wstring full(path.data(), len);
    size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : full.substr(0, slash);
}

std::wstring GetLocalAppDataDir() {
    std::array<wchar_t, MAX_PATH> buf{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf.data(), (DWORD)buf.size());
    if (n == 0 || n >= buf.size()) return GetExeDir();
    return std::wstring(buf.data(), n);
}

// Событие из фонового потока — передаём в основной поток окна через PostMessageW.
void OnOvpnEvent(std::string payloadUtf8) {
    if (!g_hwnd) return;
    auto owned = std::make_unique<std::string>(std::move(payloadUtf8));
    PostMessageW(g_hwnd, WM_APP_OVPN_EVENT, 0, reinterpret_cast<LPARAM>(owned.release()));
}

// Отправить в JS справочник стран (код, русское имя, флаг, координаты) —
// нужен только для расстановки точек на карте, к бэкенду не привязан.
void SendCountriesMeta() {
    json countries = json::array();
    for (const auto& c : tvpn::kCountries) {
        countries.push_back({
            {"code", std::string(c.code)},
            {"name", std::string(c.nameRu)},
            {"flag", std::string(c.flag)},
            {"lat", c.lat},
            {"lon", c.lon},
        });
    }
    json j;
    j["type"] = "countries_meta";
    j["countries"] = countries;
    std::wstring wide = tvpn::Utf8ToWide(j.dump());
    if (g_webview) g_webview->PostWebMessageAsJson(wide.c_str());
}

// ---------------------------------------------------------------------
// HTML/CSS/JS интерфейса — единой строкой, как и требовалось в задаче.
// Раздел бьётся на несколько соседних raw-string литералов (компилятор
// склеивает их в один) — MSVC ограничивает длину одного строкового
// литерала, а разметка страницы этот предел превышает.
// ---------------------------------------------------------------------
const wchar_t* const kHtmlPage =
LR"HTMLPAGE1(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8" />
<title>DebVPN</title>
<style>
  :root{
    --bg:#070a10; --panel:#0d131c; --panel-2:#111926; --line:#1c2735;
    --text:#e7edf5; --muted:#7c8aa0; --accent:#22d3ee; --accent-2:#7c5cff;
    --ok:#31e0a0; --warn:#ffb454; --err:#ff5c7a;
    --mono:'Cascadia Code','JetBrains Mono',ui-monospace,Consolas,monospace;
    --sans:'Segoe UI',ui-sans-serif,system-ui,sans-serif;
  }
  *{box-sizing:border-box;}
  html,body{height:100%;margin:0;}
  body{
    background:radial-gradient(1200px 800px at 78% -10%, #10203a 0%, var(--bg) 55%);
    color:var(--text); font-family:var(--sans); overflow:hidden;
  }
  #app{display:grid; grid-template-columns:1fr 380px; grid-template-rows:64px 1fr; height:100vh;}

  /* ---------- шапка ---------- */
  header{
    grid-column:1/3; display:flex; align-items:center; gap:18px;
    padding:0 22px; border-bottom:1px solid var(--line); background:rgba(10,14,20,.6);
    backdrop-filter:blur(6px);
  }
  .brand{display:flex; align-items:baseline; gap:8px;}
  .brand b{font-size:18px; letter-spacing:.5px;}
  .brand span{color:var(--muted); font-size:12px;}
  .spacer{flex:1;}
  .pill{
    display:flex; align-items:center; gap:8px; padding:7px 14px; border-radius:999px;
    background:var(--panel-2); border:1px solid var(--line); font-size:13px;
  }
  .dot{width:8px; height:8px; border-radius:50%; background:var(--muted); transition:.25s;}
  .dot.busy{background:var(--warn); box-shadow:0 0 10px var(--warn); animation:pulse 1.1s infinite;}
  .dot.on{background:var(--ok); box-shadow:0 0 10px var(--ok); animation:pulse 1.6s infinite;}
  .dot.err{background:var(--err); box-shadow:0 0 10px var(--err);}
  @keyframes pulse{0%,100%{opacity:1;}50%{opacity:.35;}}
  .headerDisconnectBtn{
    display:none; padding:7px 16px; border-radius:999px; font-size:13px; font-weight:600;
    background:linear-gradient(180deg,#3a1420,#2a0e17); border:1px solid var(--err); color:var(--err);
    cursor:pointer; transition:.2s;
  }
  .headerDisconnectBtn:hover{background:linear-gradient(180deg,#4a1a29,#33111d);}
  .headerDisconnectBtn.show{display:inline-block;}

  /* ---------- карта ---------- */
  #mapWrap{position:relative; overflow:hidden; border-right:1px solid var(--line);
    background:
      radial-gradient(900px 500px at 30% 20%, rgba(124,92,255,.10), transparent 60%),
      radial-gradient(700px 500px at 80% 70%, rgba(34,211,238,.08), transparent 60%),
      var(--bg);
  }
  #mapStage{
    position:absolute; inset:40px; transition:transform .7s cubic-bezier(.19,1,.22,1);
    transform-origin:50% 50%; will-change:transform;
  }
  #worldSvg{position:absolute; inset:0; width:100%; height:100%; overflow:visible;}
  #worldSvg path{
    fill:rgba(34,211,238,.05); stroke:rgba(124,164,196,.55); stroke-width:1.1;
    vector-effect:non-scaling-stroke;
  }
  #map{position:absolute; inset:0; }
  #mapReset{
    position:absolute; right:16px; top:14px; z-index:5; font-size:11px; color:var(--muted);
    background:rgba(13,19,28,.7); border:1px solid var(--line); border-radius:6px; padding:5px 10px;
    cursor:pointer; opacity:0; pointer-events:none; transition:.2s; font-family:var(--mono);
  }
  #mapReset.show{opacity:1; pointer-events:auto;}
  #mapReset:hover{border-color:var(--accent); color:var(--text);}
  .marker{
    position:absolute; transform:translate(-50%,-50%); cursor:pointer;
    display:flex; flex-direction:column; align-items:center; gap:4px;
  }
  .marker .core{
    width:var(--sz,10px); height:var(--sz,10px); border-radius:50%;
    background:radial-gradient(circle at 35% 30%, #bfe9ff, var(--accent) 55%, #0a5566 100%);
    box-shadow:0 0 8px rgba(34,211,238,.55), 0 0 2px #fff inset;
    transition:transform .15s ease;
  }
  .marker:hover .core{transform:scale(1.35);}
  .marker.selected .core{
    background:radial-gradient(circle at 35% 30%, #fff, var(--ok) 55%, #0a5566 100%);
    box-shadow:0 0 0 4px rgba(49,224,160,.18), 0 0 16px rgba(49,224,160,.8);
    animation:ringpulse 1.6s infinite;
  }
  @keyframes ringpulse{0%{box-shadow:0 0 0 0 rgba(49,224,160,.35),0 0 16px rgba(49,224,160,.8);}
    100%{box-shadow:0 0 0 10px rgba(49,224,160,0),0 0 16px rgba(49,224,160,.8);}}
  .marker .lbl{
    font-size:10px; color:var(--muted); background:rgba(7,10,16,.7); padding:1px 6px;
    border-radius:6px; white-space:nowrap; opacity:0; transform:translateY(2px); transition:.15s;
    pointer-events:none; border:1px solid var(--line);
  }
  .marker:hover .lbl, .marker.selected .lbl{opacity:1; transform:translateY(0);}

  #mapTotal{position:absolute; left:16px; bottom:14px; font-family:var(--mono); font-size:11px; color:var(--muted);}
  #mapTitle{position:absolute; left:20px; top:14px; font-size:12px; color:var(--muted); letter-spacing:1.5px; text-transform:uppercase;}

  /* ---------- боковая панель ---------- */
  aside{display:flex; flex-direction:column; background:var(--panel); min-height:0;}
  .sec{padding:16px 18px; border-bottom:1px solid var(--line);}
  .sec h3{margin:0 0 10px; font-size:11px; letter-spacing:1.2px; text-transform:uppercase; color:var(--muted); font-weight:600;}
  .secHead{display:flex; align-items:center; justify-content:space-between; margin-bottom:10px;}
  .secHead h3{margin:0;}
  .refreshBtn{
    background:none; border:1px solid var(--line); border-radius:6px; color:var(--muted);
    font-size:11px; padding:4px 9px; cursor:pointer; transition:.2s;
  }
  .refreshBtn:hover{border-color:var(--accent); color:var(--accent);}
  .refreshBtn:disabled{opacity:.5; cursor:not-allowed;}
  .refreshBtn span{display:inline-block;}
  .refreshBtn span.spin{animation:spin 1s linear infinite;}
  @keyframes spin{from{transform:rotate(0deg);}to{transform:rotate(360deg);}}

  .selCard{display:flex; align-items:center; gap:12px;}
  .selCard .flag{font-size:30px; line-height:1;}
  .selCard .name{font-size:15px; font-weight:600;}
  .selCard .sub{font-size:12px; color:var(--muted); font-family:var(--mono);}
  .selCard .placeholder{color:var(--muted); font-size:13px;}

  button.connectBtn{
    margin-top:12px; width:100%; padding:11px 14px; border-radius:10px; border:1px solid var(--line);
    background:linear-gradient(180deg, #17324a, #0e1f30); color:var(--text); font-size:14px; font-weight:600;
    cursor:pointer; transition:.15s;
  }
  button.connectBtn:hover{border-color:var(--accent);}
  button.connectBtn.on{background:linear-gradient(180deg,#123324,#0e2418); border-color:var(--ok); color:var(--ok);}
  button.connectBtn:disabled{opacity:.5; cursor:not-allowed;}

  #status-msg{margin-top:8px; font-size:12px; color:var(--muted); min-height:16px;}

  #search{
    width:100%; padding:8px 10px; border-radius:8px; border:1px solid var(--line);
    background:var(--panel-2); color:var(--text); font-size:13px; outline:none;
  }
  #search:focus{border-color:var(--accent);}

  #list{flex:1; overflow-y:auto; min-height:0;}
  .row{
    display:flex; align-items:center; gap:10px; padding:9px 18px; cursor:pointer; border-left:2px solid transparent;
  }
  .row:hover{background:rgba(255,255,255,.03);}
  .row.active{background:rgba(34,211,238,.07); border-left-color:var(--accent);}
  .row .flag{font-size:17px;}
  .row .name{flex:1; font-size:13px;}
  .row .cnt{font-family:var(--mono); font-size:11px; color:var(--muted); background:var(--panel-2); padding:2px 7px; border-radius:999px;}
  .row.active .cnt{color:var(--accent);}

  #logSec{border-top:1px solid var(--line); border-bottom:none;}
  #logSec summary{cursor:pointer; font-size:11px; letter-spacing:1.2px; text-transform:uppercase; color:var(--muted); padding:12px 18px; outline:none;}
  #log{
    height:140px; overflow-y:auto; font-family:var(--mono); font-size:11px; color:#7f97a8;
    padding:0 18px 12px; line-height:1.5;
  }
  #log div{white-space:pre-wrap; word-break:break-all;}

  ::-webkit-scrollbar{width:8px;} ::-webkit-scrollbar-thumb{background:#1c2735; border-radius:8px;}
</style>
)HTMLPAGE1"
LR"HTMLPAGE2(</head>
<body>
<div id="app">
  <header>
    <div class="brand"><b>DebVPN</b><span>powred by: VPN Gate</span></div>
    <div class="spacer"></div>
    <button class="headerDisconnectBtn" id="headerDisconnectBtn">Отключиться</button>
    <div class="pill"><span class="dot" id="statusDot"></span><span id="statusText">Загрузка списка серверов...</span></div>
  </header>

  <div id="mapWrap">
    <div id="mapStage">
      <svg id="worldSvg" viewBox="0 0 1000 500" preserveAspectRatio="none">
        <path d="%%WORLD_PATH%%" />
      </svg>
      <div id="map"></div>
    </div>
    <div id="mapTitle">Карта серверов VPN Gate</div>
    <div id="mapReset">⤢ Показать всю карту</div>
    <div id="mapTotal">серверов: --</div>
  </div>

  <aside>
    <div class="sec">
      <h3>Выбранная страна</h3>
      <div class="selCard" id="selCard"><span class="placeholder">Выберите точку на карте или страну из списка</span></div>
      <button class="connectBtn" id="connectBtn" disabled>Подключиться</button>
      <div id="status-msg"></div>
    </div>
    <div class="sec" style="padding-bottom:10px;">
      <div class="secHead">
        <h3>Страны</h3>
        <button class="refreshBtn" id="refreshBtn" title="Обновить список серверов VPN Gate"><span id="refreshIcon">&#8635;</span> Обновить</button>
      </div>
      <input id="search" placeholder="Поиск страны..." />
    </div>
    <div id="list"></div>
    <details id="logSec">
      <summary>Журнал OpenVPN</summary>
      <div id="log"></div>
    </details>
  </aside>
</div>

)HTMLPAGE2"
LR"HTMLPAGE3(<script>
  const state = {
    countryMeta: {},   // code -> {code,name,flag,lat,lon} — справочник координат
    countries: {},     // code -> {code,name,flag,lat,lon} — страны, где есть сервер VPN Gate
    counts: {},        // code -> число серверов
    total: 0,
    selected: null,     // code выбранной страны
    connected: null,    // code страны активного подключения (или null)
    connState: 'init',  // init|fetching|idle|connecting|connected|error
    best: {},           // code -> лучший (по score) сервер этой страны
  };

  function send(action, extra){
    window.chrome.webview.postMessage(JSON.stringify(Object.assign({action}, extra || {})));
  }

  // Проекция широта/долгота -> проценты по контейнеру карты (эквидистантная).
  function project(lat, lon){
    return { x: (lon + 180) / 360 * 100, y: (90 - lat) / 180 * 100 };
  }

)HTMLPAGE3"
LR"HTMLPAGE4(
  function markerSize(count){
    if (!count) return 6;
    return Math.max(7, Math.min(22, 7 + Math.sqrt(count) * 1.6));
  }

  const MAP_ZOOM = 3.2;

  function zoomMapTo(code){
    const stage = document.getElementById('mapStage');
    const resetBtn = document.getElementById('mapReset');
    const c = state.countries[code];
    if (!c || c.lat == null){
      stage.style.transform = 'translate(0px,0px) scale(1)';
      resetBtn.classList.remove('show');
      return;
    }
    const wrap = document.getElementById('mapWrap');
    const baseW = wrap.clientWidth - 80;
    const baseH = wrap.clientHeight - 80;
    const p = project(c.lat, c.lon);
    const px = p.x / 100 * baseW;
    const py = p.y / 100 * baseH;
    const tx = baseW / 2 - MAP_ZOOM * px;
    const ty = baseH / 2 - MAP_ZOOM * py;
    stage.style.transformOrigin = '0 0';
    stage.style.transform = `translate(${tx}px,${ty}px) scale(${MAP_ZOOM})`;
    resetBtn.classList.add('show');
  }

  function renderMap(){
    const map = document.getElementById('map');
    map.innerHTML = '';
    Object.values(state.countries).forEach(c => {
      if (c.lat == null) return;  // нет координат — покажется только в списке справа
      const p = project(c.lat, c.lon);
      const count = state.counts[c.code] || 0;
      const el = document.createElement('div');
      el.className = 'marker' + (state.selected === c.code ? ' selected' : '');
      el.style.left = p.x + '%';
      el.style.top = p.y + '%';
      el.style.setProperty('--sz', markerSize(count) + 'px');
      el.title = `${c.name}: ${count} сервер(ов)`;
      el.innerHTML = `<div class="core"></div><div class="lbl">${c.flag} ${c.name} · ${count}</div>`;
      el.addEventListener('click', () => selectCountry(c.code));
      map.appendChild(el);
    });
  }

  function renderList(filter){
    const list = document.getElementById('list');
    list.innerHTML = '';
    const f = (filter || '').trim().toLowerCase();
    const rows = Object.values(state.countries)
      .filter(c => !f || c.name.toLowerCase().includes(f) || c.code.includes(f))
      .sort((a, b) => (state.counts[b.code] || 0) - (state.counts[a.code] || 0));
    rows.forEach(c => {
      const row = document.createElement('div');
      row.className = 'row' + (state.selected === c.code ? ' active' : '');
      row.innerHTML = `<span class="flag">${c.flag}</span><span class="name">${c.name}</span><span class="cnt">${state.counts[c.code] || 0}</span>`;
      row.addEventListener('click', () => selectCountry(c.code));
      list.appendChild(row);
    });
  }

  function renderSelection(){
    // Кнопка в шапке видна всегда при активном подключении, вне зависимости от выбранной страны.
    document.getElementById('headerDisconnectBtn').classList.toggle('show', !!state.connected);

    const card = document.getElementById('selCard');
    const btn = document.getElementById('connectBtn');
    const c = state.countries[state.selected];
    if (!c){
      card.innerHTML = '<span class="placeholder">Выберите точку на карте или страну из списка</span>';
      btn.disabled = true;
      return;
    }
    const count = state.counts[c.code] || 0;
    const best = state.best[c.code];
    const pingTxt = best && best.pingMs > 0 ? `пинг ~${best.pingMs} мс` : 'пинг неизвестен';
    const protoTxt = best && best.protocol ? best.protocol.toUpperCase() : '';
    card.innerHTML = `<span class="flag">${c.flag}</span><div><div class="name">${c.name}</div>` +
      `<div class="sub">${count} сервер(ов) · ${pingTxt}${protoTxt ? ' · ' + protoTxt : ''}</div></div>`;
    const connectedHere = state.connected === c.code;
    // Отключение только через кнопку в шапке — здесь не дублируем.
    btn.disabled = connectedHere || state.connState === 'init' || state.connState === 'fetching';
    btn.textContent = connectedHere ? 'Подключено' : 'Подключиться';
    btn.classList.toggle('on', connectedHere);
  }

  function selectCountry(code){
    state.selected = code;
    renderMap(); renderList(document.getElementById('search').value); renderSelection();
    zoomMapTo(code);
  }

  document.getElementById('connectBtn').addEventListener('click', () => {
    if (!state.selected || state.connected === state.selected) return;
    const server = state.best[state.selected];
    if (!server) return;
    send('ovpn_connect', { hostName: server.hostName });
  });

  document.getElementById('headerDisconnectBtn').addEventListener('click', () => send('ovpn_disconnect'));
  document.getElementById('refreshBtn').addEventListener('click', () => send('ovpn_refresh'));
  document.getElementById('mapReset').addEventListener('click', () => zoomMapTo(null));
  document.getElementById('search').addEventListener('input', e => renderList(e.target.value));

)HTMLPAGE4"
LR"HTMLPAGE5(
  function setStatus(connState, message){
    state.connState = connState;
    const dot = document.getElementById('statusDot');
    const txt = document.getElementById('statusText');
    dot.className = 'dot';
    const labels = {
      init:'Инициализация...', fetching:'Загрузка списка серверов...', idle:'Не подключено',
      connecting:'Подключение...', connected:'Подключено', error:'Ошибка',
    };
    txt.textContent = labels[connState] || connState;
    if (connState === 'connected') dot.classList.add('on');
    else if (connState === 'error') dot.classList.add('err');
    else if (connState === 'fetching' || connState === 'connecting') dot.classList.add('busy');
    document.getElementById('status-msg').textContent = message || '';

    const refreshBtn = document.getElementById('refreshBtn');
    const fetching = connState === 'fetching';
    refreshBtn.disabled = fetching;
    document.getElementById('refreshIcon').classList.toggle('spin', fetching);

    renderSelection();
  }

  function appendLog(msg){
    const log = document.getElementById('log');
    const line = document.createElement('div');
    line.textContent = msg;
    log.appendChild(line);
    while (log.children.length > 300) log.removeChild(log.firstChild);
    log.scrollTop = log.scrollHeight;
  }

  function updateTotal(){
    document.getElementById('mapTotal').textContent = `серверов: ${state.total || '--'}`;
  }

  window.chrome.webview.addEventListener('message', ev => {
    const msg = ev.data;
    switch (msg.type){
      case 'countries_meta':
        state.countryMeta = {};
        msg.countries.forEach(c => state.countryMeta[c.code] = c);
        break;

      case 'ovpn_servers': {
        // Для игр важнее пинг и UDP-туннель, чем общий "счёт" VPN Gate — сортируем сперва по протоколу, потом по пингу.
        function protoRank(s){ return s.protocol === 'udp' ? 0 : (s.protocol === 'tcp' ? 1 : 2); }
        function isBetter(a, b){
          const ra = protoRank(a), rb = protoRank(b);
          if (ra !== rb) return ra < rb;
          const pa = a.pingMs > 0 ? a.pingMs : Infinity;
          const pb = b.pingMs > 0 ? b.pingMs : Infinity;
          if (pa !== pb) return pa < pb;
          return a.score > b.score;
        }
        const best = {};
        msg.servers.forEach(s => {
          const code = s.countryShort || s.countryLong || '??';
          if (!best[code] || isBetter(s, best[code])) best[code] = s;
        });
        state.best = best;
        const countries = {}, counts = {};
        Object.keys(best).forEach(code => {
          // Если страны нет в справочнике координат — всё равно показываем
          // в списке (без точки на карте), а не прячем молча.
          countries[code] = state.countryMeta[code] ||
            { code, name: best[code].countryLong || code, flag: '🌐', lat: null, lon: null };
          counts[code] = msg.servers.filter(s => (s.countryShort || s.countryLong || '??') === code).length;
        });
        state.countries = countries; state.counts = counts; state.total = msg.servers.length;
        renderMap(); renderList(document.getElementById('search').value); renderSelection(); updateTotal();
        break;
      }
      case 'ovpn_status':
        setStatus(msg.state, msg.message);
        break;
      case 'ovpn_log':
        appendLog(msg.message);
        break;
      case 'ovpn_connected':
        state.connected = state.selected;
        renderMap(); renderList(document.getElementById('search').value); renderSelection();
        break;
      case 'ovpn_disconnected':
        state.connected = null;
        renderMap(); renderList(document.getElementById('search').value); renderSelection();
        break;
    }
  });

  // Без этого countries_meta, отправленный сразу после NavigateToString, теряется — точек на карте не будет.
  send('ready');

  renderSelection();
</script>
</body>
</html>
)HTMLPAGE5";

// ---------------------------------------------------------------------
// Win32 + WebView2
// ---------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE: {
            if (g_controller) {
                RECT bounds;
                GetClientRect(hwnd, &bounds);
                g_controller->put_Bounds(bounds);
            }
            return 0;
        }
        case WM_APP_OVPN_EVENT: {
            // забираем владение строкой, выделенной в OnOvpnEvent (через unique_ptr::release)
            std::unique_ptr<std::string> payload(reinterpret_cast<std::string*>(lParam));
            if (payload && g_webview) {
                std::wstring wide = tvpn::Utf8ToWide(*payload);
                g_webview->PostWebMessageAsJson(wide.c_str());
            }
            return 0;
        }
        case WM_DESTROY:
            if (g_ovpn) g_ovpn->Shutdown();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void HandleWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args) {
    LPWSTR raw = nullptr;
    if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return;
    std::wstring wide(raw);
    CoTaskMemFree(raw);

    try {
        json j = json::parse(tvpn::WideToUtf8(wide));
        std::string action = j.value("action", "");
        if (action == "ready") {
            // Страница зарегистрировала слушатель — теперь можно слать countries_meta.
            SendCountriesMeta();
            if (g_ovpn) g_ovpn->FetchServerListAsync();
        } else if (action == "ovpn_connect") {
            std::string hostName = j.value("hostName", "");
            if (g_ovpn && !hostName.empty()) g_ovpn->ConnectByHostNameAsync(hostName);
        } else if (action == "ovpn_disconnect") {
            if (g_ovpn) g_ovpn->DisconnectAsync();
        } else if (action == "ovpn_refresh") {
            if (g_ovpn) g_ovpn->FetchServerListAsync();
        }
    } catch (const std::exception&) {
        // некорректное сообщение от UI — игнорируем, не роняем приложение
    }
}

void InitWebView() {
    std::wstring userDataFolder = GetLocalAppDataDir() + L"\\DebVPN\\WebView2";
    CreateDirectoryW((GetLocalAppDataDir() + L"\\DebVPN").c_str(), nullptr);
    CreateDirectoryW(userDataFolder.c_str(), nullptr);

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
                env->CreateCoreWebView2Controller(
                    g_hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                [](HRESULT, ICoreWebView2Controller* controller) -> HRESULT {
                                    if (!controller) return S_OK;
                                    g_controller = controller;
                                    g_controller->get_CoreWebView2(&g_webview);

                                    RECT bounds;
                                    GetClientRect(g_hwnd, &bounds);
                                    g_controller->put_Bounds(bounds);

                                    EventRegistrationToken token;
                                    g_webview->add_WebMessageReceived(
                                        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                                HandleWebMessage(args);
                                                return S_OK;
                                            })
                                            .Get(),
                                        &token);

                                    // подставляем контур материков (узкая ASCII-строка) в широкую
                                    // HTML-страницу вместо плейсхолдера
                                    std::wstring html = kHtmlPage;
                                    std::wstring worldPath = tvpn::Utf8ToWide(tvpn::kWorldLandPathUtf8);
                                    size_t ph = html.find(L"%%WORLD_PATH%%");
                                    if (ph != std::wstring::npos) html.replace(ph, 14, worldPath);
                                    g_webview->NavigateToString(html.c_str());

                                    tvpn::OpenVpnController::Config ovpnCfg;
                                    ovpnCfg.ovpnBinDir = GetExeDir() + L"\\openvpn\\bin";
                                    ovpnCfg.workDir = GetLocalAppDataDir() + L"\\DebVPN\\ovpn";
                                    g_ovpn = std::make_unique<tvpn::OpenVpnController>(ovpnCfg, OnOvpnEvent);

                                    // countries_meta и загрузка списка серверов запускаются по 'ready' из JS — не здесь.

                                    return S_OK;
                                })
                                .Get());
                return S_OK;
            })
            .Get());
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    // Нужен и списку серверов, и management-сокету OpenVPN — без него sockets/bind/connect молча проваливаются.
    WSADATA wsaData{};
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, kWindowClassName, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, 1180, 760, nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) return 0;

    // тёмная системная рамка окна (заголовок, кнопки свернуть/закрыть)
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    COLORREF captionColor = 0x00100A07;  // BGR для --bg (#070a10)
    DwmSetWindowAttribute(g_hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
    COLORREF captionTextColor = 0x00F5EDE7;  // BGR для --text (#e7edf5)
    DwmSetWindowAttribute(g_hwnd, DWMWA_TEXT_COLOR, &captionTextColor, sizeof(captionTextColor));

    InitWebView();

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_ovpn.reset();
    CoUninitialize();
    WSACleanup();
    return (int)msg.wParam;
}
