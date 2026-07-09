#pragma once

// ─── Веб-интерфейс (порт 80, mDNS: miniscreen.local) ─────────────────────────
// GET  /                     — SPA: тумблеры экранов, настройки (генерируются из JSON)
// GET  /api/screens          — список экранов (id/title/enabled/active)
// POST /api/screens          — {"enabled":[...]} и/или {"active":"id"}
// GET/POST /api/config?screen=<id|hub> — настройки экрана / глобальные
// GET  /files (+ /upload /download /delete) — файл-менеджер SD (папка /gif)
// GET/POST /update           — OTA-прошивка (firmware.bin из .pio/build)

void webuiStart();   // повторные вызовы игнорируются (можно дёргать из loop)
void webuiLoop();
