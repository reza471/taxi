#include "Key.h"

#include <stdbool.h>

unsigned int key_counter = 0;

const int K1_Pin = 34;
const int K2_Pin = 16;
const int K3_Pin = 27;
const int K4_Pin = 26;
const int K5_Pin = 25;

struct TFPanel KeyB = {0};

void Key_Init()
{
    pinMode(K1_Pin, INPUT_PULLUP);
    pinMode(K2_Pin, INPUT_PULLUP);
    pinMode(K3_Pin, INPUT_PULLUP);
    pinMode(K4_Pin, INPUT_PULLUP);
    pinMode(K5_Pin, INPUT_PULLUP);
}

void scankey()
{
    uint32_t Curr_key = 0;
    static uint32_t Old_key;

    if (!digitalRead(K1_Pin))
        Curr_key = 1;
    if (!digitalRead(K2_Pin))
        Curr_key = 2;
    if (!digitalRead(K3_Pin))
        Curr_key = 3;
    if (!digitalRead(K4_Pin))
        Curr_key = 4;
    if (!digitalRead(K5_Pin))
        Curr_key = 5;

    if (Old_key > 5)
        Old_key = 0;

    // Serial.println(String("Series : ") + Curr_key + "  " + Old_key);
    if (Curr_key != Old_key)
    {
        if (Curr_key != 0)
        {
            switch (Curr_key)
            {
            case 1:
               // Serial.println("K1 Pressed");
                KeyB.K1.pressed = true;
                break;
            case 2:
                KeyB.K2.pressed = true;
                break;
            case 3:
                KeyB.K3.pressed = true;
                break;
            case 4:
                KeyB.K4.pressed = true;
                break;
            case 5:
                KeyB.K5.pressed = true;
                break;
            }
        }
        else // Curr_key == 0
        {
            switch (Old_key)
            {
            case 1:
                //Serial.println("K1 Released");
                KeyB.K1.released = true;
                KeyB.K1.still_pressed = false;
                break;
            case 2:
                KeyB.K2.released = true;
                KeyB.K2.still_pressed = false;
                break;
            case 3:
                KeyB.K3.released = true;
                KeyB.K3.still_pressed = false;
                break;
            case 4:
                KeyB.K4.released = true;
                KeyB.K4.still_pressed = false;
                break;
            case 5:
                KeyB.K5.released = true;
                KeyB.K5.still_pressed = false;
                break;
            }
        }
    }

    if (Curr_key == Old_key)
    {
        if (Curr_key != 0)
        {
            switch (Curr_key)
            {
            case 1:
                KeyB.K1.pressed = false;
                KeyB.K1.still_pressed = true;
                break;
            case 2:
                KeyB.K2.pressed = false;
                KeyB.K2.still_pressed = true;
                break;
            case 3:
                KeyB.K3.pressed = false;
                KeyB.K3.still_pressed = true;
                break;
            case 4:
                KeyB.K4.pressed = false;
                KeyB.K4.still_pressed = true;
                break;
            case 5:
                KeyB.K5.pressed = false;
                KeyB.K5.still_pressed = true;
                break;
            }
        }
        else // Curr_key =0 & Old_key=0
        {
            KeyB.K1.pressed = false;
            KeyB.K1.still_pressed = false;
            KeyB.K1.released = false;

            KeyB.K2.pressed = false;
            KeyB.K2.still_pressed = false;
            KeyB.K2.released = false;

            KeyB.K3.pressed = false;
            KeyB.K3.still_pressed = false;
            KeyB.K3.released = false;

            KeyB.K4.pressed = false;
            KeyB.K4.still_pressed = false;
            KeyB.K4.released = false;

            KeyB.K5.pressed = false;
            KeyB.K5.still_pressed = false;
            KeyB.K5.released = false;
        }
    }

    if (Curr_key != 0)
        KeyB.Any_Key_Pressed = true;
    else
        KeyB.Any_Key_Pressed = false;

    Old_key = 6;
    Old_key = Curr_key;
    // if (Old_key > 5)
    //     Old_key = 0;
}

void Key_Proc()
{
    scankey();
}

void Key_Sync()
{
    if (KeyB.K1.still_pressed || KeyB.K2.still_pressed || KeyB.K3.still_pressed || KeyB.K4.still_pressed || KeyB.K5.still_pressed)
    {
        KeyB.Hold_Time++;
        KeyB.RestTime = 0;
    }
    else
    {
        KeyB.Hold_Time = 0;
        if (KeyB.RestTime < INT_MAX)
            KeyB.RestTime++;
    }
}

void Clear_Key()
{
    KeyB.K1.pressed = false;
    KeyB.K1.still_pressed = false;
    KeyB.K1.released = false;

    KeyB.K2.pressed = false;
    KeyB.K2.still_pressed = false;
    KeyB.K2.released = false;

    KeyB.K3.pressed = false;
    KeyB.K3.still_pressed = false;
    KeyB.K3.released = false;

    KeyB.K4.pressed = false;
    KeyB.K4.still_pressed = false;
    KeyB.K4.released = false;

    KeyB.K5.pressed = false;
    KeyB.K5.still_pressed = false;
    KeyB.K5.released = false;
}