#include "../core/screen.h"

// ─── Реестр экранов — единственная точка регистрации ──────────────────────────
// Новый экран: создать src/screens/<имя>.cpp с классом-наследником Screen и
// функцией-фабрикой, объявить её здесь и добавить в массив. Всё остальное
// (веб, кнопки, ротация, NVS) подхватится само.

Screen* clockAnalogScreen();
Screen* clockDigitalScreen();
Screen* calendarScreen();
Screen* ringLoaderScreen();
Screen* progressBarScreen();
Screen* gaugeScreen();
Screen* matrixScreen();
Screen* starfieldScreen();
Screen* radarScreen();
Screen* gifPlayerScreen();
Screen* cloudsScreen();
Screen* rainRadarScreen();
Screen* cityMapScreen();
Screen* sdInfoScreen();

// extern обязателен: const-массив/const int без него получили бы internal
// linkage и не слинковались бы с extern-объявлениями в screen_manager.cpp
extern Screen* const kScreens[] = {
    clockAnalogScreen(),
    clockDigitalScreen(),
    calendarScreen(),
    radarScreen(),
    gifPlayerScreen(),
    cloudsScreen(),
    rainRadarScreen(),
    cityMapScreen(),
    sdInfoScreen(),
    ringLoaderScreen(),
    progressBarScreen(),
    gaugeScreen(),
    matrixScreen(),
    starfieldScreen(),
};

extern const int kScreenCount = sizeof(kScreens) / sizeof(kScreens[0]);
