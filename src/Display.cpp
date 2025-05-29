#include "Display.h"
#include "main.h"
#include "Wire.h"

#include "driver/i2c.h"

#define HT16K33_ADDRESS 0x70 // آدرس پیش‌فرض HT16K33

uint8_t Sec_tick = 0;

char DispStr1[9];
char DispStr2[5];
uint8_t Disp_Main_Str[12];

uint8_t HT16k33_buffer[16];
uint8_t Disp_Data[13]; // one for leds

const uint8_t SevenSegmentASCII[96] = {
    0x00, /* (space) */
    0x86, /* ! */
    0x22, /* " */
    0x7E, /* # */
    0x6D, /* $ */
    0xD2, /* % */
    0x46, /* & */
    0x20, /* ' */
    0x29, /* ( */
    0x0B, /* ) */
    0x21, /* * */
    0x70, /* + */
    0x10, /* , */
    0x40, /* - */
    0x80, /* . */
    0x52, /* / */
    0x3F, /* 0 */
    0x06, /* 1 */
    0x5B, /* 2 */
    0x4F, /* 3 */
    0x66, /* 4 */
    0x6D, /* 5 */
    0x7D, /* 6 */
    0x07, /* 7 */
    0x7F, /* 8 */
    0x6F, /* 9 */
    0x09, /* : */
    0x0D, /* ; */
    0x61, /* < */
    0x48, /* = */
    0x43, /* > */
    0xD3, /* ? */
    0x5F, /* @ */
    0x77, /* A */
    0x7C, /* B */
    0x39, /* C */
    0x5E, /* D */
    0x79, /* E */
    0x71, /* F */
    0x3D, /* G */
    0x76, /* H */
    0x30, /* I */
    0x1E, /* J */
    0x75, /* K */
    0x38, /* L */
    0x15, /* M */
    0x37, /* N */
    0x3F, /* O */
    0x73, /* P */
    0x6B, /* Q */
    0x33, /* R */
    0x6D, /* S */
    0x78, /* T */
    0x3E, /* U */
    0x3E, /* V */
    0x2A, /* W */
    0x76, /* X */
    0x6E, /* Y */
    0x5B, /* Z */
    0x39, /* [ */
    0x64, /* \ */
    0x0F, /* ] */
    0x23, /* ^ */
    0x08, /* _ */
    0x02, /* ` */
    0x5F, /* a */
    0x7C, /* b */
    0x58, /* c */
    0x5E, /* d */
    0x7B, /* e */
    0x71, /* f */
    0x6F, /* g */
    0x74, /* h */
    0x10, /* i */
    0x0C, /* j */
    0x75, /* k */
    0x30, /* l */
    0x14, /* m */
    0x54, /* n */
    0x5C, /* o */
    0x73, /* p */
    0x67, /* q */
    0x50, /* r */
    0x6D, /* s */
    0x78, /* t */
    0x1C, /* u */
    0x1C, /* v */
    0x14, /* w */
    0x76, /* x */
    0x6E, /* y */
    0x5B, /* z */
    0x46, /* { */
    0x30, /* | */
    0x70, /* } */
    0x01, /* ~ */
    0x00, /* (del) */
};

// Function to map ASCII characters to their 7-segment representation
void convertAsciiTo7Seg(const char *asciiData, uint8_t *sevenSegData, int size)
{
    for (int i = 0; i < size; i++)
    {
        sevenSegData[i] = SevenSegmentASCII[(unsigned char)asciiData[i] - 32];
    }
}

void Seg_Transform(uint8_t *s, uint8_t *d)
{
    uint8_t temp[13];
    // memcpy((uint8_t *)temp, s, 12);

    temp[0] = s[7];
    temp[1] = s[8];
    temp[2] = s[9];
    temp[3] = s[10];
    temp[4] = s[11];
    temp[5] = s[3];
    temp[6] = s[2];
    temp[7] = s[0];
    temp[8] = s[1];
    temp[9] = s[4];
    temp[10] = s[5];
    temp[11] = s[6];
    temp[12] = s[12];

    memcpy(d, (uint8_t *)temp, 13);
}

// Function to map 7-segment data to the Buffer
void map7SegToBuffer(uint8_t *sevenSegData_in, uint8_t *buffer)
{
    uint8_t sevenSegData[13];

    Seg_Transform((uint8_t *)sevenSegData_in, (uint8_t *)sevenSegData);

    memset(buffer, 0, 16); // Clear the buffer

    for (int segment = 0; segment < 8; segment++)
    {                          // Iterate over each segment ('a' to 'dp')
        uint8_t lowerBits = 0; // Lower group (1–5 digits)
        uint8_t upperBits = 0; // Upper group (6–12 digits)

        // Map digits 1–5 to lowerBits
        for (int i = 0; i < 5; i++)
        {
            lowerBits <<= 1;
            if (sevenSegData[i] & (1 << segment)) // Place bits at correct positions
            {
                lowerBits |= 1;
            }
        }

        // Map digits 6–12 to upperBits
        for (int i = 5; i < 12; i++)
        {
            upperBits <<= 1;
            // upperBits |= ((sevenSegData[i] & (1 << segment))); // Place bits at correct positions
            if (sevenSegData[i] & (1 << segment)) // Place bits at correct positions
            {
                upperBits |= 1;
            }
        }

        // Store the result in the buffer
        buffer[2 * segment] = lowerBits << 3; // Lower group (shift left 3 bits for padding)
        buffer[2 * segment + 1] = upperBits;  // Upper group
    }
}

void Display_Init()
{
    Wire.begin(21, 22, 100000); // شروع I2C

    // مقداردهی اولیه HT16K33
    Wire.beginTransmission(HT16K33_ADDRESS);
    Wire.write(0x21); // فعال کردن نوسان‌ساز
    Wire.endTransmission();

    Wire.beginTransmission(HT16K33_ADDRESS);
    Wire.write(0x81); // فعال کردن نمایشگر و خاموش کردن چشمک‌زن
    Wire.endTransmission();

    Wire.beginTransmission(HT16K33_ADDRESS);
    Wire.write(0xE0); // تنظیم روشنایی (0xE0 تا 0xEF)
    Wire.endTransmission();
}

// uint8_t HT16k33_buffer[16];
// uint8_t Disp_Data[12];
void Display_Proc()
{
    static unsigned long previousMillis = 0;
    unsigned long currentMillis = millis();
    // static uint8_t oDisp_Main_Str[12];
    static uint8_t oDisp_Data[13]; // one for leds

    static uint8_t display_state = 0;

    switch (display_state)
    {
    case 1:
        memcpy((uint8_t *)Disp_Main_Str, (uint8_t *)DispStr1, 8);
        memcpy((uint8_t *)&Disp_Main_Str[8], (uint8_t *)DispStr2, 4);
        break;
    case 2:
        convertAsciiTo7Seg((char *)Disp_Main_Str, (uint8_t *)Disp_Data, 12);
        break;
    case 3:
        convertAsciiTo7Seg((char *)Disp_Main_Str, (uint8_t *)Disp_Data, 12);
        if (Sec_tick && ShowClock == true)
        {
            Disp_Data[9] |= (1 << 7);
        }
        else
        {
            Disp_Data[9] &= ~(1 << 7);
        }
        break;

    case 4:
        memcpy((uint8_t *)oDisp_Data, (uint8_t *)Disp_Data, 13);
        map7SegToBuffer((uint8_t *)Disp_Data, (uint8_t *)&HT16k33_buffer);
        break;
    case 5:
        if (WiFi_State)
        {
            HT16k33_buffer[1] |= 0x80;
            HT16k33_buffer[3] &= ~(0x80);
        }
        else
        {
            HT16k33_buffer[1] &= ~0x80;
            if (blink_500ms)
            {
                HT16k33_buffer[3] &= ~0x80;
            }
            else
            {
                HT16k33_buffer[3] |= 0x80;
            }
        }
        break;
    case 6:
        Wire.beginTransmission(HT16K33_ADDRESS);
        Wire.write(0x00); // شروع ارسال داده از آدرس 0x00
        break;
    case 7:
        Wire.write(HT16k33_buffer, 16); // ارسال مقادیر باینری برای هر رقم
        break;
    case 8:
        Wire.endTransmission();
        display_state = 0;
        break;
    }

    if (display_state != 0)
        display_state++;
    /* combine 7 seg buffer*/

    // if (strncmp((char *)oDisp_Main_Str, (char *)Disp_Main_Str, 12) == 0)
    //     return;

    // memcpy((uint8_t *)oDisp_Main_Str, (uint8_t *)Disp_Main_Str, 12);

    /**/

    // if (strncmp((char *)oDisp_Data, (char *)Disp_Data, 13) == 0)
    //    return;

    // for (int i = 0; i < 16; i++)
    //{

    //}
    if (currentMillis - previousMillis >= 300)
    {
        display_state = 1;
        previousMillis = currentMillis; // Save the current time
    }
}