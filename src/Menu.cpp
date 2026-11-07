#include "Menu.h"
#include "main.h"
#include "Network.h"
#include "Display.h"
#include "WiFi.h"

String StartTimeStr;

// #define BuzzMenu() __asm__ __volatile__("NOP")
#define BuzzMenu() beep(4000, 100)

int Hours, Min, Sec;
unsigned long int rep_SR[10] = {0};
unsigned char rep_SR_Th[10] = {0};
unsigned char rep_SR_Tm[10] = {0};
unsigned int rep_SR_Km[10] = {0};
bool rep_SR_night[8] = {false};

bool Blink_tick = false;

unsigned long int Blink_counter = 0;
unsigned long int Zero_counter = 0;

char IsOn = 1;

char Active_Page = main_bm;

char menu_idx = fareCh1_bm;

char nightstr[6]; // night day

extern uint32_t Tpulse;

void main_page();
void menu_page();
void Samar_Page();
void Reserve_Page();
void report_page();
void speed_page();
void pulse_page();
void farech1_page();
void farech4_page();
void Off_Page();

void Menu_Proc()
{
    if (Net_Busy)
    {
        sprintf(DispStr1, "  Busy  ");
        sprintf(DispStr2, "%02u%02u", Hours, Min);
        return;
    }

    switch (Active_Page)
    {
    case main_bm:
        main_page();
        break;
    case menu_bm:
        menu_page();
        break;
    case samar_bm:
        Samar_Page();
        break;
    case reserve_bm:
        Reserve_Page();
        break;
    case speed_bm:
        speed_page();
        break;
    case pulse_bm:
        pulse_page();
        break;
    case rep_bm:
        report_page();
        break;
    case fareCh1_bm:
        farech1_page();
        break;
    case fareCh4_bm:
        farech4_page();
        break;
    case off_bm:
        Off_Page();
        break;
    }
}

void Off_Page()
{
    sprintf(DispStr1, "        ");
    sprintf(DispStr2, "    ");
    ShowClock = false;

    if (KeyB.K5.pressed)
    {
        IsOn = 1;
        BuzzMenu();
        Active_Page = main_bm;
    }
}

void main_page()
{
    static uint8_t Menuflag = 0;
    ShowClock = true;
    sprintf(DispStr1, "--------");
    sprintf(DispStr2, "%02u%02u", Hours, Min);

    if (KeyB.K3.still_pressed && KeyB.Hold_Time > 10000)
    {
        mySetting.setWifi("Rad");
        mySetting.setWifiPass("123456789");
        mySetting.writeToFile();
        ESP.restart();
    }

    if (!Authorized)
        return;

    if (KeyB.K1.pressed && mySetting.getKorsi())
    {
        BuzzMenu();
        // Serial.println("KeyB K1 Released");
        Start_TimeOut(0, 2000);
        night_proc();
        Zero_counter = 0; // blink off
        Start_Trip(1);
        init_ch(0, 1);
        IsCh1_Start = true;
        Active_Page = fareCh4_bm;
    }

    if (KeyB.K2.pressed)
    {
        Tpulse = 0;
        Active_Page = pulse_bm;
    }

    if (KeyB.K3.pressed)
    {
        // Active_Page = rep_bm;
        // Serial.println("KeyB K3 Pressed");
    }

    if (KeyB.K4.pressed && mySetting.getDarbast())
    {
        BuzzMenu();
        // Serial.println("KeyB K4 Pressed");

        Start_TimeOut(1, 2000);
        night_proc();
        StartTimeStr = getFormattedTime();
        Start_Trip(5);
        init_ch(0, 0);
        Zero_counter = 0; // blink off
        Clear_Key();
        Active_Page = fareCh1_bm;
    }

    if (KeyB.K5.still_pressed)
    {
        if (KeyB.Hold_Time > 3000 && Menuflag == 0)
        {
            BuzzMenu();
            Menuflag = 1;
        }
        /* turn off */
        if (KeyB.Hold_Time > 5000 && Menuflag == 1)
        {
            BuzzMenu();
            Menuflag = 0;
            // digitalWrite(Bk_OutputPin, LOW); // Set IO2 low
            Active_Page = off_bm;
        }
    }

    if (KeyB.K5.released and Menuflag == 1)
    {
        Menuflag = 0;
        sprintf(DispStr2, "    ");
        Active_Page = menu_bm;
    }
}

void menu_page()
{
    static uint8_t i = 0;

    ShowClock = false;

    sprintf(DispStr1, "MENU    ");

    /* normal*/
    if (KeyB.K1.released)
    {
        sprintf(DispStr2, "----");
        if (i == 1)
        {
            Active_Page = main_bm;
        }
        i = 1;
    }
    /* Samar */
    if (KeyB.K2.released && mySetting.getSamar())
    {
        sprintf(DispStr2, "SAMR");
        if (i == 2)
        {
            Active_Page = samar_bm;
        }
        i = 2;
    }

    if (KeyB.K3.released && mySetting.getNobat())
    {
        sprintf(DispStr2, "NOBT");
        if (i == 3)
        {
            Active_Page = reserve_bm;
        }
        i = 3;
    }

    if (KeyB.K4.released)
    {
        sprintf(DispStr2, "TARF");
        if (i == 4)
        {
            get_SettingData(1);
            Active_Page = main_bm;
        }
        i = 4;
    }

    if (KeyB.K5.released)
    {
        BuzzMenu();
        Active_Page = main_bm;
    }
}

void Samar_Page()
{
    static int8_t idx = 0;
    static bool selected = false;

    if (!selected)
        sprintf(DispStr1, "SR- %4d", mySetting.getService(idx));
    else
        sprintf(DispStr1, "%4d STA", mySetting.getService(idx));

    if (KeyB.K1.pressed && !selected)
    {
        idx++;
        if (idx >= mySetting.getServiceCount())
        {
            idx = 0;
        }
    }

    if (KeyB.K2.pressed && !selected)
    {
        idx--;
        if (idx < 0)
        {
            idx = mySetting.getServiceCount() - 1;
        }
    }

    if (KeyB.K3.pressed && !selected)
    {
        Send_Service_Data(idx + 1, 1);

        selected = true;
    }

    if (KeyB.K5.pressed && selected)
    {
        Send_Service_Data(idx + 1, 2);

        idx = 0;
        selected = false;
        Active_Page = samar_bm;
    }

    if (KeyB.K5.still_pressed && KeyB.Hold_Time > 3000 && !selected)
    {
        idx = 0;
        selected = false;
        sprintf(DispStr2, "    ");
        Active_Page = menu_bm;
    }
}

void Reserve_Page()
{
    static uint8_t reserve_state = 0;
    if (reserve_state == 0)
    {
        sprintf(DispStr2, "nobt");
        if (blink_500ms)
        {
            sprintf(DispStr1, "--------");
        }
        else
        {
            sprintf(DispStr1, "        ");
        }

        int n = WiFi.scanNetworks();
        Serial.println("scan done");
        if (n != 0)
        {
            for (int i = 0; i < n; ++i)
            {
                String ssid = WiFi.SSID(i);
                if (ssid.equals("Usefi"))
                {
                    reserve_state = 1;
                }
            }
        }
    }

    if (reserve_state == 1)
    {
        sprintf(DispStr1, "211  ---");
    }

    if (KeyB.K5.still_pressed && KeyB.Hold_Time > 3000)
    {
        sprintf(DispStr2, "    ");
        Active_Page = menu_bm;
    }
}

void Samar1_Page()
{
    static uint8_t idx = 0;
    static uint8_t i = 0;

    ShowClock = true;

    if (idx != 0)
    {
        sprintf(DispStr1, "SR- %4d", mySetting.getService(idx - 1));
    }
    else
    {
        sprintf(DispStr1, "SERVICE ");
    }
    sprintf(DispStr2, "%02u%02u", Hours, Min);
    // sprintf(DispStr2, "%4d", idx);
    if (KeyB.K1.pressed)
    {
        if ((idx == 0) || (idx == 1))
        {
            if (idx == 0)
            {
                idx = 1;
                Send_Service_Data(1, 1);
            }
            else
            {
                idx = 0;
                Send_Service_Data(1, 2);
            }
        }
    }

    if (KeyB.K2.pressed)
    {
        if ((idx == 0) || (idx == 2))
        {
            if (idx == 0)
            {
                idx = 2;
                Send_Service_Data(2, 1);
            }
            else
            {
                idx = 0;
                Send_Service_Data(2, 2);
            }
        }
    }

    if (KeyB.K3.pressed)
    {
        if ((idx == 0) || (idx == 3))
        {
            if (idx == 0)
            {
                idx = 3;
                Send_Service_Data(3, 1);
            }
            else
            {
                idx = 0;
                Send_Service_Data(3, 2);
            }
        }
    }

    if (KeyB.K4.pressed)
    {
        if ((idx == 0) || (idx == 4))
        {
            if (idx == 0)
            {
                idx = 4;
                Send_Service_Data(4, 1);
            }
            else
            {
                idx = 0;
                Send_Service_Data(4, 2);
            }
        }
    }

    if (KeyB.K5.pressed && idx == 0)
    {
        idx = 0;
        Active_Page = main_bm;
    }
}

void speed_page()
{
    int Hours, Min;
    sprintf(DispStr1, "SPD: %3u", speed);
    sprintf(DispStr2, "%02u%02u", Hours, Min);
    if (KeyB.K5.pressed)
    {
        // BuzzMenu();
        menu_idx = fareCh1_bm; // fare ----
        Active_Page = main_bm;
    }
}

void pulse_page()
{
    ShowClock = false;
    sprintf(DispStr1, "%8lu", Tpulse);
    sprintf(DispStr2, "PL  ");

    if (KeyB.K2.pressed)
    {
        Tpulse = 0;
    }

    if (KeyB.K5.pressed)
    {
        // BuzzMenu();
        menu_idx = fareCh1_bm; // fare ----
        Active_Page = main_bm;
    }
}

void test_page()
{
    static int test_idx = 0;
    int Hours, Min;
    sprintf(DispStr2, "%02u%02u", Hours, Min);

    switch (test_idx)
    {
    case 0:
        sprintf(DispStr1, "PM: %4u", mySetting.getPM());
    case 1:
        sprintf(DispStr1, "PL:%5u", pulse);
        break;
    case 2:
        // sprintf(DispStr1, "S%5u", mySetting.getDriverType());
        break;
    case 3:
        // sprintf(DispStr1, "t%5u", mySetting.getDate());
        break;
    case 4:
        // sprintf(DispStr1, "C%5u", mySetting.getDarbast1mstop());
        break;
    case 5:
        // sprintf(DispStr1, "c%5u", mySetting.getDarbast1mstop2());
        break;
    case 6:
        // sprintf(DispStr1, "D%5u", mySetting.getDarbast200m());
        break;
    case 7:
        // sprintf(DispStr1, "d%5u", mySetting.getDarbast200m2());
        break;
    }

    if (KeyB.K4.pressed)
    {
        test_idx++;
        if (test_idx > 7)
            test_idx = 0;

        if (test_idx == 1)
            pulse = 0;
    }

    if (KeyB.K5.pressed)
    {
        BuzzMenu();
        test_idx = 0;
        menu_idx = fareCh1_bm; // fare ----
        Active_Page = main_bm;
    }
}

// Max_stop_km

void round()
{
    if (Sum_Cost[0] > mySetting.getFraction())
    {
        Sum_Cost[0] -= mySetting.getFraction();
        Final_Cost[0] += mySetting.getFraction();
    }

    if (Sum_Cost[1] > mySetting.getFraction())
    {
        Sum_Cost[1] -= mySetting.getFraction();
        Final_Cost[1] += mySetting.getFraction();
    }

    if (Sum_Cost[2] > mySetting.getFraction())
    {
        Sum_Cost[2] -= mySetting.getFraction();
        Final_Cost[2] += mySetting.getFraction();
    }

    if (Sum_Cost[3] > mySetting.getFraction())
    {
        Sum_Cost[3] -= mySetting.getFraction();
        Final_Cost[3] += mySetting.getFraction();
    }

    if (Final_Cost[0] > 99999999)
        Final_Cost[0] = 0;
    if (Final_Cost[1] > 999999)
        Final_Cost[1] = 0;
    if (Final_Cost[2] > 999999)
        Final_Cost[2] = 0;
    if (Final_Cost[3] > 999999)
        Final_Cost[3] = 0;
}

void Collect_Trip_Data(long bt)
{
    myTripData.setSerial("08A6F7A6A30C");
    myTripData.setDriverId("08A6F7A6A30C");
    myTripData.setbt_number(bt);
    myTripData.setStartTime(StartTimeStr);
    myTripData.setStopTime(getFormattedTime());
    myTripData.setTripDistance(0);
    myTripData.setTripDuration(1);
    myTripData.setTrafficDistance(0);
    myTripData.setTrafficDuration(1);
    myTripData.setAvgSpeed(0);
    myTripData.setMaxSpeed(0);
    myTripData.setNightPrice(0);

    if (bt >= 1 && bt <= 4)
    {
        myTripData.setTripPrice(Sum_Cost1PM[bt - 1]);
        myTripData.setTrafficPrice(Sum_Cost1s[bt - 1]);
        myTripData.setTotalPrice(Final_Cost[bt - 1]);
    }
    else if (bt == 5)
    {
        myTripData.setTripPrice(Sum_Cost1PM[0]);
        myTripData.setTrafficPrice(Sum_Cost1s[0]);
        myTripData.setTotalPrice(Final_Cost[0]);
    }
}

void farech1_page()
{
    static int fareIdx = 0;
    char clkstr[5];

    if (!IsTimeOut(1))
    { // Pri_Cost
        sprintf(DispStr1, "%8lu", (unsigned long int)(mySetting.getPriCost() / 10));
        Zero_counter = 0; // blink off
        StopSec[0] = 0;
        IsPause = 0;
        return;
    }

    switch (fareIdx)
    {
    case 0:
        ShowClock = true;
        sprintf(clkstr, "%02u%02u", Hours, Min);
        break;
    case 1:
        ShowClock = false;
        sprintf(clkstr, "%4lu", speed);
        break;
    case 2:
        ShowClock = false;
        if (Trip_KMeter[0] < 10)
            sprintf(clkstr, "  0%1lu", Trip_KMeter[0]);
        else
            sprintf(clkstr, "%4lu", Trip_KMeter[0]);
        break;
    case 3:
        ShowClock = false;
        sprintf(clkstr, "%4lu", StopSec[0] / 60);
        break;
    }

    round();

    sprintf(DispStr2, "%4s", clkstr);

    if (Is_Blink)
    {

        if (Blink_tick)
        {
            sprintf(DispStr1, "%8lu", (unsigned long int)(Final_Cost[0] / 10));
        }
        else
        {
            sprintf(DispStr1, "        ");
        }
    }
    if (!Is_Blink && !IsPause)
    {
        sprintf(DispStr1, "%8lu", (unsigned long int)(Final_Cost[0] / 10));
    }

    if (KeyB.K3.pressed) // pause
    {
        BuzzMenu();
        IsPause = !IsPause;
    }
    else if (IsPause)
    {
        sprintf(DispStr1, "  STOP  ");
    }

    if (KeyB.K4.released && !IsPause)
    {
        BuzzMenu();
        Zero_counter = Blink_C;
    }

    if (Is_Blink && KeyB.K4.released)
    {
        BuzzMenu();
        // insert_new_rep((unsigned long int)(Final_Cost[0] / 10));
        Active_Page = main_bm;
        fareIdx = 0;
        IsPause = 0;
        Collect_Trip_Data(5);
        Send_Trip_Data();
    }

    if (KeyB.K2.pressed)
    {
        sprintf(DispStr1, "123456  ");
        fareIdx++;
        if (fareIdx > 3)
            fareIdx = 0;
    }

    if (Final_Cost[0] > 99999999)
        Final_Cost[0] = 0;
}

bool RouteToNextActive_channel(int *i)
{

    switch (*i)
    {
    case 0:

        if (IsCh2_Start)
        {
            *i = 1;
            break;
        }
        else if (IsCh3_Start)
        {
            *i = 2;
            break;
        }
        else if (IsCh4_Start)
        {
            *i = 3;
            break;
        }
        break;
    case 1:

        if (IsCh3_Start)
        {
            *i = 2;
            break;
        }
        else if (IsCh4_Start)
        {
            *i = 3;
            break;
        }
        else if (IsCh1_Start)
        {
            *i = 0;
            break;
        }
        break;
    case 2:

        if (IsCh4_Start)
        {
            *i = 3;
            break;
        }
        else if (IsCh1_Start)
        {
            *i = 0;
            break;
        }
        else if (IsCh2_Start)
        {
            *i = 1;
            break;
        }
        break;
    case 3:

        if (IsCh1_Start)
        {
            *i = 0;
            break;
        }
        else if (IsCh2_Start)
        {
            *i = 1;
            break;
        }
        else if (IsCh3_Start)
        {
            *i = 2;
            break;
        }
        break;
    }

    return 0;
}

void farech4_page()
{
    static int farech4_idx = 0;

    if (!IsTimeOut(0))
    {
        sprintf(DispStr1, "1-%6lu", (unsigned long int)(mySetting.getPri2Cost() / 10));
        Zero_counter = 0; // blink off
        return;
    }

    round();

    //    night_proc();
    //
    //    sprintf(DispStr,"%6lu%02d%02d",(unsigned long int)Pri_Cost,Hours,Min);
    //    delay_ms(1000);
    //
    //    sprintf(DispStr,"%6lu%02d%02d",(unsigned long int)Cost_1PM,Hours,Min);
    //    delay_ms(1000);
    //
    //    sprintf(DispStr,"%6lu%02d%02d",(unsigned long int)Cost_1s,Hours,Min);
    //    delay_ms(1000);

    switch (farech4_idx)
    {

    case 0:
        if (IsCh1_Start)
            sprintf(DispStr1, "1-%6lu", (unsigned long int)(Final_Cost[0] / 10));
        else
            sprintf(DispStr1, "1 ----- ");
        break;
    case 1:
        if (IsCh2_Start)
            sprintf(DispStr1, "2-%6lu", (unsigned long int)(Final_Cost[1] / 10));
        else
            sprintf(DispStr1, "2 ----- ");
        break;
    case 2:
        if (IsCh3_Start)
            sprintf(DispStr1, "3-%6lu", (unsigned long int)(Final_Cost[2] / 10));
        else
            sprintf(DispStr1, "3 ----- ");
        break;
    case 3:
        if (IsCh4_Start)
            sprintf(DispStr1, "4-%6lu", (unsigned long int)(Final_Cost[3] / 10));
        else
            sprintf(DispStr1, "4 ----- ");
        break;
    }

    if ((IsCh1_Start || IsCh2_Start || IsCh3_Start || IsCh4_Start) && Is_Blink && Blink_tick)
    {
        sprintf(DispStr1, "%1d-      ", farech4_idx + 1);
    }

    // CH1************************************************
    if (KeyB.K1.released)
    {
        BuzzMenu();
        // Serial.print("Start is");
        // Serial.println(IsCh1_Start);

        if (!IsCh1_Start)
        {
            // Serial.println("Its start");
            Clear_Key();
            Start_Trip(1);
            sprintf(DispStr2, "%02d%02d", Hours, Min);
            sprintf(DispStr1, "1-%6lu", (unsigned long int)(mySetting.getPri2Cost() / 10));
            Display_Proc();
            delay(500);
            init_ch(0, 1);
            IsCh1_Start = true;
        }
        else
        {
            // Serial.println("Its End");
            if (farech4_idx == 0 && Is_Blink)
            {
                Clear_Key();
                Zero_counter = 0; // blink off
                BuzzMenu();
                IsCh1_Start = false;
                Collect_Trip_Data(1);
                Send_Trip_Data();
                // insert_new_rep((unsigned long int)(Final_Cost[0] / 10));
            }
        }
        farech4_idx = 0;
        // if(!IsCh1_Start) RouteToNextActive_channel();
    }

    // CH2************************************************
    if (KeyB.K2.released)
    {
        BuzzMenu();
        if (!IsCh2_Start)
        {
            Clear_Key();
            sprintf(DispStr1, "2-%6lu", (unsigned long int)(mySetting.getPri2Cost() / 10));
            Display_Proc();
            delay(500);
            init_ch(1, 1);
            IsCh2_Start = true;
            Start_Trip(2);
        }
        else
        {
            if (farech4_idx == 1 && Is_Blink)
            {
                Clear_Key();
                Zero_counter = 0; // blink off
                BuzzMenu();
                IsCh2_Start = false;
                Collect_Trip_Data(2);
                Send_Trip_Data();
                // Stop_Trip(3);
                //  insert_new_rep((unsigned long int)(Final_Cost[2] / 10));
            }
        }
        farech4_idx = 1;
    }

    // CH3************************************************
    if (KeyB.K3.released)
    {
        BuzzMenu();
        if (!IsCh3_Start)
        {
            Clear_Key();
            sprintf(DispStr1, "3-%6lu", (unsigned long int)(mySetting.getPri2Cost() / 10));
            Display_Proc();
            delay(500);
            init_ch(2, 1);
            IsCh3_Start = true;
            Start_Trip(3);
        }
        else
        {
            if (farech4_idx == 2 && Is_Blink)
            {
                Clear_Key();
                Zero_counter = 0; // blink off
                Is_Blink = false;
                BuzzMenu();
                IsCh3_Start = false;
                Collect_Trip_Data(3);
                Send_Trip_Data();
                // Stop_Trip(3);
                //  insert_new_rep((unsigned long int)(Final_Cost[2] / 10));
            }
        }
        farech4_idx = 2;
        // if(!IsCh3_Start)RouteToNextActive_channel();
    }

    // CH4************************************************
    if (KeyB.K4.released)
    {
        BuzzMenu();
        if (!IsCh4_Start)
        {
            Clear_Key();
            sprintf(DispStr2, "%02d%02d", Hours, Min);
            sprintf(DispStr1, "4-%6lu", (unsigned long int)(mySetting.getPri2Cost() / 10));
            Display_Proc();
            delay(500);
            init_ch(3, 1);
            IsCh4_Start = true;
            Start_Trip(4);
        }
        else
        {
            if (farech4_idx == 3 && Is_Blink)
            {
                Clear_Key();
                Zero_counter = 0; // blink off
                BuzzMenu();
                IsCh4_Start = false;
                Collect_Trip_Data(4);
                Send_Trip_Data();
                // insert_new_rep((unsigned long int)(Final_Cost[3] / 10));
            }
        }
        farech4_idx = 3;
        // if(!IsCh4_Start)RouteToNextActive_channel();
    }

    if (KeyB.K1.released || KeyB.K2.released || KeyB.K3.released || KeyB.K4.released)
    {
        Zero_counter = Blink_C;
    }

    //****************************************************
    if (!IsCh1_Start && !IsCh2_Start && !IsCh3_Start && !IsCh4_Start)
    {
        menu_idx = fareCh1_bm; // fare ----
        Active_Page = main_bm;
    }

    if (Final_Cost[0] > 99999999)
        Final_Cost[0] = 0;
    if (Final_Cost[1] > 999999)
        Final_Cost[1] = 0;
    if (Final_Cost[2] > 999999)
        Final_Cost[2] = 0;
    if (Final_Cost[3] > 999999)
        Final_Cost[3] = 0;
}

void report_page()
{
    static int rep_idx = 0;
    sprintf(DispStr1, "  %6lu", rep_SR[rep_idx]);
    sprintf(DispStr2, "%02u%02u", rep_SR_Th[rep_idx], rep_SR_Tm[rep_idx]);
    if (KeyB.K3.pressed)
    {
        rep_idx++;
        if (rep_idx >= 10)
            rep_idx = 0;
    }

    if (KeyB.K5.pressed)
    {
        // BuzzK4();
        rep_idx = 0;
        menu_idx = fareCh1_bm; // fare ----
        Active_Page = main_bm;
    }
}

void Disp_serial_Proc()
{
    // Actions to perform every second
    Serial.print("Taxi:");
    Serial.print(DispStr1);

    Serial.print("  clock:");
    Serial.println(DispStr2);
}

void Menu_Sync_1ms()
{
    // 1 ms
    Blink_counter++;
    if (Blink_counter > Blink_PR)
        Blink_counter = 0;

    if (Blink_counter > 240)
        Blink_tick = true;
    else
        Blink_tick = false;

    if (Zero_counter >= 1)
        Zero_counter--;

    if (Zero_counter != 0)
        Is_Blink = true;
    else
        Is_Blink = false;
}