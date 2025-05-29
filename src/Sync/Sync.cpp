#include "Sync.h"

uint32_t Panel_Sync_Counter = 0;

uint32_t TimeoutCounter[5];
uint32_t TimeoutContent[5];
void TimeOut_Proc();

void Sync_hdlr()
{
  static uint32_t Counter_1Sec;

  TimeOut_Proc();

  if (Counter_1Sec++ > 1000)
  {
    Counter_1Sec = 0;
  }
}

void Start_TimeOut(uint8_t i, uint16_t time)
{
  TimeoutContent[i] = time;
  TimeoutCounter[i] = 0;
}

bool IsTimeOut(uint8_t i)
{

  if (TimeoutCounter[i] >= TimeoutContent[i])
    return true;
  else
    return false;
}

void TimeOut_Proc()
{
  uint8_t i;

  for (i = 0; i < 5; i++)
  {
    if (TimeoutCounter[i] < TimeoutContent[i])
      TimeoutCounter[i] += 1;
  }
}

uint16_t TimeOut_CNT(uint8_t i)
{
  return TimeoutCounter[i];
}