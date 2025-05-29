#ifndef _DISPLAY_H_
#define _DISPLAY_H_

#include "main.h"

extern char DispStr1[9];
extern char DispStr2[5];

extern uint8_t Sec_tick;

void Display_Init();
void Display_Proc();

#endif