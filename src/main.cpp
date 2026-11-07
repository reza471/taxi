#include "main.h"
#include "esp32/clk.h"
#include "driver/pcnt.h"

uint8_t Authorized = 1;
uint8_t Trip3_Active = 0;

uint8_t Net_Busy = false;

String SERIAL_NUMBER = "";

int pulse_inputPin = 32;
int Bk_OutputPin = 2;

uint8_t blink_500ms;

uint32_t Tpulse = 0;

extern char IsOn;

float Cost_1PM;
float Cost_1s;
float Sum_Cost1PM[4];
float Sum_Cost1s[4];
long int StopSec[4];
float Sum_Cost[4] = {0};
float Pri_Cost = 19500;
float Prim_Cost = 25884;
float Final_Cost[4];
unsigned long int Trip_KMeter[4];

int speed;

unsigned int pulse = 0;
unsigned int vpulse = 0;
unsigned int kpulse = 0;

bool Is_Blink = false;
bool Is_Off = false;  // start off on  routine
bool IsPause = false; // start stop routine
bool IsNight = false;

bool ShowClock = true;

bool IsCh1_Start = false;
bool IsCh2_Start = false;
bool IsCh3_Start = false;
bool IsCh4_Start = false;

long int freeTripPM = 0;
long int freeSecond = 0;

long int freepulse = 0;
long int freeSecondpulse = 0;
long int freepulse_Cost3 = 0;

signed int kasri[4] = {0};
bool IsFirst[4] = {false};

TSetting mySetting;
TripData myTripData;

// Create a hardware timer object
hw_timer_t *timer = NULL;
void onTimer();
void pcnt_init(); // Initialize

int PM;
volatile bool pulseDetected = false; // Flag for pulse detection
volatile int pulseCount = 0;         // Count pulses safely in ISR

const int buzzer = 4; // buzzer to arduino pin 9

void beep(int freq, int duration)
{
  tone(buzzer, freq); // Send 1KHz sound signal...
  delay(duration);    // ...for 1 sec
  noTone(buzzer);     // Stop sound...
}

void setup()
{
  initArduino();
  Serial.begin(115200);
  Serial.print("Clock Freq :");
  Serial.print(esp_clk_cpu_freq()); // Output in Hz
  Serial.println("Mhz");

  pinMode(buzzer, OUTPUT); // Set buzzer - pin 9 as an output

  pinMode(pulse_inputPin, INPUT_PULLUP);
  pinMode(Bk_OutputPin, OUTPUT);   // Set GPIO2 as output
  digitalWrite(Bk_OutputPin, LOW); // Set IO2 low

  // Configure the timer
  timer = timerBegin(0, 80, true);             // Timer 0, prescaler 80 for 1 microsecond resolution
  timerAttachInterrupt(timer, &onTimer, true); // Attach the ISR to the timer
  timerAlarmWrite(timer, 1000, true);          // 1 second (1,000,000 microseconds)
  timerAlarmEnable(timer);                     // Enable the alarm

mySetting.readFromFile();

  freeTripPM = (mySetting.getFreeTrip()) * mySetting.getPM() / 10;
  freeSecond = mySetting.getFreeTime();

  Key_Init();
  // pcnt_init();
  Odometer_Init();
  Display_Init();
  NetWork_Init();

}

void loop()
{
  static unsigned long previousMillis = 0;
  unsigned long currentMillis = millis();

  static unsigned long tick = 0;

  NetWork_Proc();
  Display_Proc();

  // Check if 1 second has passed
  currentMillis = millis();
  if (currentMillis - previousMillis >= 50)
  {
    Key_Proc();
    Menu_Proc();
    PM = mySetting.getPM();

    previousMillis = currentMillis;
  }
}

//        init_ch(&Final_Cost[0], &Sum_Cost[0], &Sum_Cost1s[0], &Sum_Cost1PM[0], 0);
void init_ch(char ch, uint8_t is4ch)
{
  unsigned long int h = 0;
  unsigned int pri;

  Sum_Cost[ch] = 0;
  Sum_Cost1s[ch] = 0;
  Sum_Cost1PM[ch] = 0;

  if (!is4ch)
  {
    Final_Cost[ch] = mySetting.getPriCost();
    pri = mySetting.getPriCost();
  }
  else
  {
    Final_Cost[ch] = mySetting.getPri2Cost();
    pri = mySetting.getPri2Cost();
  }

  //*sum=(unsigned long int)Pri_Cost%10;
  pri /= 10; // Pri_Cost is change but not important ,it update on each loop
  pri *= 10;

  h = pri / 10;
  h = h % 10;
  //*sum=-(h*10);

  // reza
  kasri[ch] = 50 - (h * 10);
  IsFirst[ch] = true;

  //*final=Pri_Cost;

  //      if(h<=5)
  //      {
  //       Pri_Cost=(unsigned long int)(Pri_Cost/100);
  //       Pri_Cost*=10;
  //       //Pri_Cost+=5;
  //       *final=Pri_Cost*10;
  //      }
  //      else
  //      {
  //       Pri_Cost=(unsigned long int)(Pri_Cost/100);
  //       Pri_Cost+=1;
  //       Pri_Cost*=100;
  //       *final=Pri_Cost;
  //      }

  freepulse = 0;
  freeSecondpulse = 0;
  freepulse_Cost3 = 0;
}

void night_proc()
{
  float x, y;
  if (Active_Page != fareCh4_bm)
  {
    x = (float)mySetting.getCost200() / (20 * mySetting.getPM()); // 20 X ppm10m  = 200m
    y = (float)mySetting.getCostStop() / 60;
    Pri_Cost = mySetting.getPriCost();

    if (Trip3_Active)
    {
      x = (float)mySetting.getCost3_200() / (20 * mySetting.getPM()); // 20 X ppm10m  = 200m
      y = (float)mySetting.getCost3_stop() / 60;
    }
  }
  else
  {
    x = (float)mySetting.getCost2200() / (20 * mySetting.getPM());
    y = (float)mySetting.getCost2Stop() / 60;
    Pri_Cost = mySetting.getPri2Cost();
  }

  if (IsNight)
  {
    Cost_1PM = (x * (float)mySetting.getPercentNight() / 100) + x;
    Cost_1s = (y * (float)mySetting.getPercentNight() / 100) + y;
    Pri_Cost = (Pri_Cost * 0.1) + Pri_Cost;
  }
  else
  {
    Cost_1PM = x;
    Cost_1s = y;
  }
}

void processPulse()
{
  if (IsPause)
    return;

  pulse++;
  vpulse++;
  kpulse++;
  freepulse++;
  freepulse_Cost3++;

  // Update km count
  if (kpulse >= (10 * PM))
  { // Replace Cost_1PM with mySetting.getPM() if needed
    kpulse = 0;
    Trip_KMeter[0]++;
    Trip_KMeter[1]++;
    Trip_KMeter[2]++;
    Trip_KMeter[3]++;
  }

  // Update cost

  Sum_Cost[0] += Cost_1PM;
  Sum_Cost[1] += Cost_1PM;
  Sum_Cost[2] += Cost_1PM;
  Sum_Cost[3] += Cost_1PM;

  // Handle free pulses
  if (freepulse < freeTripPM)
  {
    Sum_Cost[0] -= Cost_1PM;
    Sum_Cost[1] -= Cost_1PM;
    Sum_Cost[2] -= Cost_1PM;
    Sum_Cost[3] -= Cost_1PM;
  }
  else
  {
    freepulse--;
  }

  if (freepulse_Cost3 > mySetting.gettrip3() && mySetting.gettrip3() > 0)
  {
    Trip3_Active = 1;
  }
  else
  {
    Trip3_Active = 0;
  }
}

void IRAM_ATTR onTimer()
{
  static unsigned long previousMillis1000 = 0;
  static unsigned long previousMillis10000 = 0;

  static unsigned long counter500 = 0;
  unsigned long currentMillis = millis();

  Sync_hdlr();

  counter500++;
  if (counter500 >= 500)
  {
    blink_500ms = ~blink_500ms;
    counter500 = 0;
  }

  // Check if 1 second has passed
  if (currentMillis - previousMillis1000 >= 1000)
  {
    Sec_tick = ~Sec_tick;

    /* calc pulse */
    static uint32_t Calc_CostPm;
    uint32_t pulse = pcnt_get_counter();
    Calc_CostPm = pulse * Cost_1PM;
    Tpulse += pulse;
    speed = (unsigned char)((float)((10.0 / pulse) * 3.6 * vpulse));

    if (!IsPause && (speed < mySetting.getMaxStopKm()))
    {
      /* --------------- Trip -------------------------*/
      freepulse += pulse;

      for (int i = 0; i < 4; i++)
      {
        Sum_Cost[i] += Calc_CostPm;
      }

      // Handle free pulses
      if (freepulse < freeTripPM)
      {
        for (int i = 0; i < 4; i++)
        {
          Sum_Cost[i] -= Calc_CostPm;
        }
      }
      else
      {
        freepulse -= pulse;
      } // lock
    }

    if (!IsPause)
    {
      /* --------------- Time -------------------------*/
      freeSecondpulse++;

      for (int i = 0; i < 4; i++)
      {
        Sum_Cost[i] += Cost_1s;
      }

      for (int i = 0; i < 4; i++)
      {
        StopSec[i]++;
      }

      if (freeSecondpulse < freeSecond)
      {
        for (int i = 0; i < 4; i++)
        {
          Sum_Cost[i] -= Cost_1s;
        }
      }
      else
      {
        freeSecondpulse--;
      } // lock
    }

    previousMillis1000 = currentMillis; // Save the current time
  }

  // Check if 10 second has passed
  if (currentMillis - previousMillis1000 >= 10000)
  {
    if (((Hours * 100 + Min) >= (mySetting.getStopNight() * 100 + 0)) && ((Hours * 100 + Min) < (mySetting.getStartNight() * 100 + 0)))
      IsNight = false;
    else
      IsNight = true;

    Sec = 10;
    if (Sec > 59)
    {
      Sec = 0;
      Min++;
    }

    if (Min > 59)
    {
      Min = 0;
      Hours++;
    }
    if (Hours > 23)
      Hours = 0;

    if (IsOn && Hours > 17)
      digitalWrite(Bk_OutputPin, HIGH);
    else
      digitalWrite(Bk_OutputPin, LOW);

    previousMillis10000 = currentMillis; // Save the current time
  }

  Menu_Sync_1ms();
  Key_Sync();
}
