#include "webui.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <ArduinoJson.h>
#include "config.h"
#include "screen_manager.h"
#include "storage.h"

static WebServer server(80);
static bool started = false;

static const char* GIF_DIR = "/gif";

// ─── Главная страница ─────────────────────────────────────────────────────────
// Формы настроек генерируются на клиенте из JSON getConfig() — экранам не нужен
// свой HTML. Без внешних зависимостей, вся страница в PROGMEM.

static const char MAIN_HTML[] PROGMEM = R"html(<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>MiniScreen Hub</title><style>
body{font-family:sans-serif;background:#111;color:#eee;max-width:460px;margin:20px auto;padding:0 12px}
h1{font-size:20px}h2{font-size:16px;color:#8cf;margin-top:24px}
.row{background:#1a1a1a;border:1px solid #333;border-radius:8px;padding:8px 10px;margin:6px 0}
.row.active{border-color:#17a05e}
.row .name{margin:0 8px;font-weight:bold}
button{padding:6px 10px;background:#333;border:1px solid #555;border-radius:6px;color:#eee;margin-left:4px;cursor:pointer}
button.primary{background:#17a05e;border:0}
.cfg{margin-top:8px;border-top:1px solid #333;padding-top:6px}
.cfg label{display:block;margin:6px 0;color:#8cf;font-size:13px}
.cfg input[type=text],.cfg input:not([type]){width:100%;padding:6px;background:#222;color:#eee;border:1px solid #444;border-radius:6px;box-sizing:border-box}
select{padding:6px;background:#222;color:#eee;border:1px solid #444;border-radius:6px;margin-bottom:6px}
p.links{margin-top:24px}a{color:#8cf}
</style></head><body>
<h1>MiniScreen Hub</h1>
<div id='list'></div>
<h2>Общие настройки</h2><div id='hub' class='row'><div class='cfg' style='display:block'></div></div>
<p class='links'><a href='/files'>Файлы на SD</a> | <a href='/update'>Обновление прошивки (OTA)</a></p>
<script>
const CITIES=[["Istanbul",41.01,28.98],["Ankara",39.93,32.86],["Moscow",55.76,37.62],
["London",51.51,-0.13],["Paris",48.86,2.35],["Berlin",52.52,13.4],["Madrid",40.42,-3.7],
["Rome",41.9,12.5],["Amsterdam",52.37,4.9],["Vienna",48.21,16.37],["Warsaw",52.23,21.01],
["Prague",50.08,14.44],["Kyiv",50.45,30.52],["Minsk",53.9,27.57],["Athens",37.98,23.73],
["Stockholm",59.33,18.06],["Helsinki",60.17,24.94],["Budapest",47.5,19.04],
["Belgrade",44.79,20.45],["Tbilisi",41.72,44.79],["Yerevan",40.18,44.51],["Baku",40.41,49.87],
["Astana",51.13,71.43],["Almaty",43.24,76.89],["Tashkent",41.3,69.24],["Bishkek",42.87,74.59],
["Dubai",25.2,55.27],["Cairo",30.04,31.24],["Tokyo",35.68,139.69],["Beijing",39.9,116.4],
["Seoul",37.57,126.98],["Delhi",28.61,77.21],["Bangkok",13.76,100.5],["Singapore",1.35,103.82]];
async function api(u,body){const o=body?{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}:undefined;
const r=await fetch(u,o);const ct=r.headers.get('content-type')||'';return ct.includes('json')?r.json():r.text()}
async function load(){const d=await api('/api/screens');render(d.screens)}
function render(screens){const el=document.getElementById('list');el.innerHTML='';
screens.forEach(s=>{const row=document.createElement('div');row.className='row'+(s.active?' active':'');
const cb=document.createElement('input');cb.type='checkbox';cb.checked=s.enabled;cb.dataset.id=s.id;cb.onchange=saveEnabled;
const name=document.createElement('span');name.className='name';name.textContent=s.title+(s.active?' ●':'');
const show=document.createElement('button');show.className='primary';show.textContent='Показать';
show.onclick=async()=>{await api('/api/screens',{active:s.id});load()};
const gear=document.createElement('button');gear.textContent='⚙';gear.onclick=()=>toggleCfg(s.id,row);
row.append(cb,name,show,gear);
const cfg=document.createElement('div');cfg.className='cfg';cfg.style.display='none';row.appendChild(cfg);
el.appendChild(row)})}
async function saveEnabled(){const en=[...document.querySelectorAll('#list input[type=checkbox]')]
.filter(c=>c.checked).map(c=>c.dataset.id);await api('/api/screens',{enabled:en});load()}
function buildForm(div,obj,id){div.innerHTML='';
for(const k in obj){const lab=document.createElement('label');lab.textContent=k+' ';
const inp=document.createElement('input');
if(typeof obj[k]==='boolean'){inp.type='checkbox';inp.checked=obj[k]}else{inp.value=obj[k]}
inp.dataset.k=k;inp.dataset.t=typeof obj[k];lab.appendChild(inp);div.appendChild(lab)}
const b=document.createElement('button');b.className='primary';b.textContent='Сохранить';
b.onclick=async()=>{const o={};div.querySelectorAll('input[data-k]').forEach(i=>{
o[i.dataset.k]=i.dataset.t==='boolean'?i.checked:(i.dataset.t==='number'?parseFloat(i.value):i.value)});
await api('/api/config?screen='+id,o);load()};div.appendChild(b)}
async function loadHub(){const div=document.querySelector('#hub .cfg');
const cfg=await api('/api/config?screen=hub');buildForm(div,cfg,'hub');
const sel=document.createElement('select');
sel.innerHTML='<option>— город (подставит координаты) —</option>'+
CITIES.map(c=>`<option data-la='${c[1]}' data-lo='${c[2]}'>${c[0]}</option>`).join('');
sel.onchange=()=>{const o=sel.selectedOptions[0];if(!o.dataset.la)return;
div.querySelector('input[data-k=homeLat]').value=o.dataset.la;
div.querySelector('input[data-k=homeLon]').value=o.dataset.lo};
div.prepend(sel)}
load();loadHub();
</script></body></html>)html";

// ─── /api/screens ─────────────────────────────────────────────────────────────

static void handleScreensGet() {
    int count;
    Screen* const* all = smAll(count);
    JsonDocument doc;
    JsonArray arr = doc["screens"].to<JsonArray>();
    for (int i = 0; i < count; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["id"]      = all[i]->id();
        o["title"]   = all[i]->title();
        o["enabled"] = screenEnabled(all[i]->id());
        o["active"]  = (all[i] == smActive());
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleScreensPost() {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "text/plain", "bad json");
        return;
    }
    if (doc["enabled"].is<JsonArray>()) {
        String csv;
        for (JsonVariant v : doc["enabled"].as<JsonArray>()) {
            if (csv.length()) csv += ',';
            csv += v.as<const char*>();
        }
        hubCfg.enabledCsv = csv;
        hubSave();
        smEnabledChanged();
    }
    if (doc["active"].is<const char*>()) smActivate(doc["active"].as<const char*>());
    server.send(200, "application/json", "{\"ok\":true}");
}

// ─── /api/config ──────────────────────────────────────────────────────────────

static Screen* findScreen(const String& id) {
    int count;
    Screen* const* all = smAll(count);
    for (int i = 0; i < count; i++)
        if (id == all[i]->id()) return all[i];
    return nullptr;
}

void timeApplyTz();   // из timesvc.h — не тянем весь заголовок ради одной функции

static void handleConfigGet() {
    String id = server.arg("screen");
    JsonDocument doc;
    if (id == "hub") {
        doc["homeLat"]   = hubCfg.homeLat;
        doc["homeLon"]   = hubCfg.homeLon;
        doc["tzHours"]   = hubCfg.tzHours;
        doc["rotateSec"] = hubCfg.rotateSec;
    } else {
        Screen* s = findScreen(id);
        if (!s) { server.send(404, "text/plain", "no such screen"); return; }
        s->getConfig(doc.to<JsonObject>());
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleConfigPost() {
    String id = server.arg("screen");
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "text/plain", "bad json");
        return;
    }
    if (id == "hub") {
        float oldTz = hubCfg.tzHours;
        if (!doc["homeLat"].isNull())   hubCfg.homeLat   = doc["homeLat"].as<double>();
        if (!doc["homeLon"].isNull())   hubCfg.homeLon   = doc["homeLon"].as<double>();
        if (!doc["tzHours"].isNull())   hubCfg.tzHours   = doc["tzHours"].as<float>();
        if (!doc["rotateSec"].isNull()) hubCfg.rotateSec = doc["rotateSec"].as<uint32_t>();
        hubSave();
        if (hubCfg.tzHours != oldTz) timeApplyTz();
        smNotifyConfigChanged("hub");
    } else {
        Screen* s = findScreen(id);
        if (!s) { server.send(404, "text/plain", "no such screen"); return; }
        s->applyConfig(doc.as<JsonObjectConst>());
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

// ─── Файл-менеджер SD (из firmware_gif_sd) ───────────────────────────────────
// Без авторизации — только для домашней сети, не выставлять наружу.

static String htmlEscape(const String& s) {
    String out = s;
    out.replace("&", "&amp;");
    out.replace("<", "&lt;");
    out.replace(">", "&gt;");
    return out;
}

static void handleFiles() {
    if (!sdMountIfNeeded()) {
        server.send(200, "text/html; charset=utf-8",
                    "<html><body><h3>Нет SD-карты</h3><a href='/'>назад</a></body></html>");
        return;
    }
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                  "<title>MiniScreen: файлы</title></head><body><a href='/'>&larr; назад</a>";
    html += "<h3>GIF на карте</h3><ul>";
    File dir = SD.open(GIF_DIR);
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                String n = htmlEscape(String(f.name()));
                html += "<li>" + n + " (" + String(f.size() / 1024) + " КБ)"
                        " &mdash; <a href='/download?name=" + n + "'>скачать</a>"
                        " | <a href='/delete?name=" + n + "' onclick=\"return confirm('Удалить " + n + "?')\">удалить</a></li>";
            }
            f.close();
            f = dir.openNextFile();
        }
        dir.close();
    }
    html += "</ul><h3>Загрузить GIF</h3>"
            "<form method='POST' action='/upload' enctype='multipart/form-data'>"
            "<input type='file' name='data' accept='.gif'> "
            "<input type='submit' value='Загрузить'></form></body></html>";
    server.send(200, "text/html; charset=utf-8", html);
}

static File uploadFile;

static void handleUploadData() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        if (!sdMountIfNeeded()) return;
        if (!SD.exists(GIF_DIR)) SD.mkdir(GIF_DIR);
        String path = String(GIF_DIR) + "/" + upload.filename;
        Serial.printf("Upload start: %s\n", path.c_str());
        uploadFile = SD.open(path, FILE_WRITE);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) uploadFile.close();
        Serial.printf("Upload done: %u bytes\n", (unsigned)upload.totalSize);
    }
}

static void handleUploadDone() {
    smNotifyConfigChanged("gif");   // GIF-плеер пересканирует список
    server.sendHeader("Location", "/files");
    server.send(303);
}

static void handleDownload() {
    if (!server.hasArg("name")) { server.send(400, "text/plain", "no name"); return; }
    if (!sdMountIfNeeded())     { server.send(500, "text/plain", "no sd"); return; }
    String name = server.arg("name");
    String path = String(GIF_DIR) + "/" + name;
    if (!SD.exists(path)) path = "/" + name;
    File f = SD.open(path);
    if (!f) { server.send(404, "text/plain", "not found"); return; }
    server.sendHeader("Content-Disposition", "attachment; filename=" + name);
    server.streamFile(f, "application/octet-stream");
    f.close();
}

static void handleDelete() {
    if (!server.hasArg("name")) { server.send(400, "text/plain", "no name"); return; }
    if (!sdMountIfNeeded())     { server.send(500, "text/plain", "no sd"); return; }
    String name = server.arg("name");
    String path = String(GIF_DIR) + "/" + name;
    if (!SD.exists(path)) path = "/" + name;
    SD.remove(path);
    smNotifyConfigChanged("gif");
    server.sendHeader("Location", "/files");
    server.send(303);
}

// ─── OTA ──────────────────────────────────────────────────────────────────────

static const char UPDATE_HTML[] PROGMEM = R"html(<!DOCTYPE html><html><head><meta charset='utf-8'>
<title>OTA</title></head><body><a href='/'>&larr; назад</a>
<h3>Обновление прошивки</h3>
<p>Файл: <code>.pio/build/esp32-s3/firmware.bin</code>. После загрузки девайс перезагрузится.</p>
<form method='POST' action='/update' enctype='multipart/form-data'>
<input type='file' name='update' accept='.bin'> <input type='submit' value='Прошить'></form>
</body></html>)html";

static void handleUpdatePost() {
    bool ok = !Update.hasError();
    server.send(200, "text/plain", ok ? "OK, rebooting..." : "UPDATE FAILED");
    delay(500);
    if (ok) ESP.restart();
}

static void handleUpdateUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("OTA start: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
            Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("OTA OK: %u bytes\n", upload.totalSize);
        else Update.printError(Serial);
    }
}

// ─── Старт ────────────────────────────────────────────────────────────────────

void webuiStart() {
    if (started) return;
    started = true;

    MDNS.begin("miniscreen");   // http://miniscreen.local

    server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", MAIN_HTML); });
    server.on("/api/screens", HTTP_GET,  handleScreensGet);
    server.on("/api/screens", HTTP_POST, handleScreensPost);
    server.on("/api/config",  HTTP_GET,  handleConfigGet);
    server.on("/api/config",  HTTP_POST, handleConfigPost);

    server.on("/files",    HTTP_GET,  handleFiles);
    server.on("/upload",   HTTP_POST, handleUploadDone, handleUploadData);
    server.on("/download", HTTP_GET,  handleDownload);
    server.on("/delete",   HTTP_GET,  handleDelete);

    server.on("/update", HTTP_GET, []() { server.send_P(200, "text/html", UPDATE_HTML); });
    server.on("/update", HTTP_POST, handleUpdatePost, handleUpdateUpload);

    server.begin();
    Serial.printf("Web UI: http://%s/ (http://miniscreen.local/)\n",
                  WiFi.localIP().toString().c_str());
}

void webuiLoop() {
    if (started) server.handleClient();
}
