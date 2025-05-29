#ifndef _MENU_H_
#define _MENU_H_

#include <Arduino.h>
#include "TSetting.h"
#include "main.h"

#define Blink_C 4000
#define Blink_PR 480

enum
{
    main_bm = 0,
    rep_bm,
    speed_bm,
    pulse_bm,
    menu_bm,
    fareCh1_bm,
    fareCh4_bm,
    samar_bm,
    reserve_bm,
    off_bm
} Page_e;

extern char Active_Page;

extern int Hours, Min, Sec;
extern char nightstr[6]; // night day

void Menu_Proc();

void Menu_Sync_1ms();

#endif