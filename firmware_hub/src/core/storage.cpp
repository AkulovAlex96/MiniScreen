#include "storage.h"

static SPIClass sdSPI(HSPI);   // HSPI = независимая от дисплея (SPI2) шина
static bool mounted = false;

void sdInitBus() {
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
}

bool sdMounted() { return mounted; }

// Сначала штатная скорость, при неудаче — медленная (400 кГц) на случай
// шумных проводов на макетке/длинных джамперов.
static bool trySdBegin() {
    if (SD.begin(SD_CS, sdSPI, 4000000)) return true;
    Serial.println("SD.begin @4MHz failed, retry @400kHz...");
    SD.end();
    delay(50);
    if (SD.begin(SD_CS, sdSPI, 400000)) {
        Serial.println("SD.begin @400kHz OK");
        return true;
    }
    Serial.println("SD.begin failed at both speeds");
    return false;
}

bool sdMountIfNeeded() {
    if (mounted) {
        if (SD.cardType() == CARD_NONE) {   // вынули на горячую
            sdUnmount();
            return false;
        }
        return true;
    }
    SD.end();
    mounted = trySdBegin();
    return mounted;
}

void sdUnmount() {
    SD.end();
    mounted = false;
}
