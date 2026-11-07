#ifndef _MAIN_H_
#define _MAIN_H_

#include <Arduino.h>
#include "Network.h"
#include "Menu.h"
#include "Key.h"
#include "TSetting.h"
#include "TripData.h"
#include "Display.h"
#include "Sync/Sync.h"
#include "Odometer.h"

// ANSI escape codes for color
#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define RESET "\033[0m"

// #define PM 46          // dar 10 meter
// #define pri_cost 26200 // Rial
// #define Start_night 22
// #define Stop_night 06
// #define Cost_200 1318
// #define Cost_stop 500000 // 1 min
// #define Max_stop_km 20
// #define free_trip 0      // meter
// #define free_time 0      // min
// #define Percent_night 10 // 20%
// #define fraction 50

// #define pri_costch4 5000 // Rial
// #define Cost_200ch4 384
// #define Cost_stopch4 334 // 1 min

extern uint8_t Net_Busy;

extern String SERIAL_NUMBER;

extern TSetting mySetting;
extern TripData myTripData;

extern float Cost_1PM;
extern float Cost_1s;
extern float Sum_Cost1PM[4];
extern float Sum_Cost1s[4];
extern long int StopSec[4];
extern float Sum_Cost[4];
// extern float Pri_Cost;
//  extern float Prim_Cost;
extern float Final_Cost[4];
extern unsigned long int Trip_KMeter[4];

extern unsigned int pulse;
extern unsigned int vpulse;
extern unsigned int kpulse;

extern bool Is_Blink;
extern bool Is_Off;  // start off on  routine
extern bool IsPause; // start stop routine
extern bool IsNight;

extern bool ShowClock;

extern bool IsCh1_Start;
extern bool IsCh2_Start;
extern bool IsCh3_Start;
extern bool IsCh4_Start;

extern char Active_Page;

extern signed int kasri[4];
extern bool IsFirst[4];

extern int speed;

extern uint8_t blink_500ms;

extern uint8_t Authorized;

void night_proc();
void init_ch(char ch, uint8_t is4ch);
void beep(int freq, int duration);

#endif