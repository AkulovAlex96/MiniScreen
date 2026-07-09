#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>

// ─── Wi-Fi (2 сети, приоритет первой) + общие сетевые ресурсы ────────────────
// Экраны делают свои HTTP-запросы сами (внутри tick), но через общее:
//  - secureClient — один WiFiClientSecure на всех (TLS-хендшейку нужно ~50КБ
//    heap разом, два одновременных не потянем — и не нужно, экран активен один);
//  - netBuf — общий 48КБ буфер скачивания (PSRAM), см. netBufGet();
//  - fetchToBuffer — скачать URL в буфер (для PNG-тайлов/карт).

bool netConnect();        // блокирующе перебирает сети (статус на экран)
void netLoop();           // реконнект раз в 30с, если связь упала
bool netUp();

extern WiFiClientSecure secureClient;

// Корневые CA для экранов, которым нужен настоящий verify (см. firmware_map:
// http.begin(url) с https на этой связке плата+framework ловит start_ssl_client: -1,
// обход — проверка настоящей цепочки сертификатов).
extern const char* ROOT_CA_AMAZON;   // api.mapbox.com
extern const char* ROOT_CA_ISRG;     // api.open-meteo.com

static const size_t NET_BUF_SIZE = 48 * 1024;
uint8_t* netBufGet();     // общий буфер скачивания (PSRAM, ленивая аллокация)

// Скачать URL в буфер. client==nullptr — обычный http.begin(url)
// (работает и для https на части хостов — так делали прототипы).
bool fetchToBuffer(const String& url, uint8_t* buf, size_t bufSize, size_t& outLen,
                   WiFiClient* client = nullptr);
