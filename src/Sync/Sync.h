#ifndef __SYNC_H
#define __SYNC_H

#include "main.h" 

void Sync_hdlr();
void Start_TimeOut(uint8_t i,uint16_t time);
bool IsTimeOut(uint8_t i);

#endif