#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

// ─── MicroSD на отдельной SPI-шине (HSPI) ─────────────────────────────────────
// Не общая с дисплеем (LovyanGFX держит SPI2 монопольно).
// CATALEX-модуль: GND->GND  VCC->5V(!)  MISO->41  MOSI->40  SCK->39  CS->38
// VCC именно 5V — на плате свой AMS1117 с просадкой ~1В.

#define SD_CS   38
#define SD_SCK  39
#define SD_MOSI 40
#define SD_MISO 41

void sdInitBus();          // sdSPI.begin — один раз в setup
bool sdMounted();
bool sdMountIfNeeded();    // true = карта смонтирована (пробует 4МГц, потом 400кГц)
void sdUnmount();          // после горячего извлечения (SD.cardType()==CARD_NONE)
