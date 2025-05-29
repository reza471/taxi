#ifndef _KEY_H_
#define _KEY_H_
#include <Arduino.h>

typedef struct
{
  bool pressed;
  bool still_pressed;
  bool released;
} Tkey;

struct TFPanel
{
  Tkey K1;
  Tkey K2;
  Tkey K3;
  Tkey K4;
  Tkey K5;
  bool Any_Key_Pressed;
  int Hold_Time;
  int RestTime;
};

extern struct TFPanel KeyB;

void Key_Init();

void Key_Proc();

void Key_Sync();

void Clear_Key();

#endif