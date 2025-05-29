#include "main.h"
#include "WiFi.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

struct tm timeinfo;

const char *GetSettingURL = "https://node.raxitaxi.ir/api/taxi/TaxiFare?driver_type=1&serial=";
const char *PostTripURL = "https://node.raxitaxi.ir/api/taxi/SetTaxiEnd";
const char *PostTripStartURL = "https://node.raxitaxi.ir/api/taxi/SetTaxiStart?serial=08A6F7A6A30C&bt_number=";
const char *PostTripStopURL = "https://node.raxitaxi.ir/api/taxi/SetTaxiEnd?serial=08A6F7A6A30C&bt_number=";
const char *PostServiceURL = "https://node.raxitaxi.ir/api/taxi/StartService?"; // serviceNum=222&serial=123123123&start=1
// const char *GetSettingURL = "http://192.168.0.122/api/taxi/TaxiFare?driver_type=1&serial=08A6F7A6A30C";
// const char *PostTripURL = "http://192.168.0.122/api/taxi/SetTaxiEnd";
// const char *PostTripStartURL = "http://192.168.0.122/api/taxi/SetTaxiStart?serial=08A6F7A6A30C&bt_number=";
// const char *PostTripStopURL = "http://192.168.0.122/api/taxi/SetTaxiEnd?serial=08A6F7A6A30C&bt_number=";
// const char *PostServiceURL = "http://192.168.0.122/api/taxi/StartService?"; // serviceNum=222&serial=123123123&start=1

// const char *ssid = "AP-RAD";
// const char *password = "1398@Radrayaneh.com";
// const char *ssid = "Gal";
// const char *password = "1234qwer";

// const char *ssid = "TP-LINK_5EE9B8";
// const char *password = "92519251**";

const char *ssid = "dd-wrt_vap";
const char *password = "73147314**iI";

// NTP server configuration
const char *ntpServer = "ntp.day.ir";
const long gmtOffset_sec = 12600; // Adjust for your timezone offset in seconds (e.g., -3600 for GMT-1)
const int daylightOffset_sec = 0; // Adjust for daylight savings in seconds

int SamerInQueue[10];
int SamerOutQueue[10];
int KursStartQueue[10];
TripData TripDataQueue[10];

String PostDataQueue[10] = {
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    ""};

uint8_t WiFi_State = 0;

void SetToQueue(String s);

void Get_Time();

void get_SettingData();

void NetWork_Init()
{
    Serial.println();
    Serial.print("[WiFi] Connecting to ");
    Serial.println(ssid);
    Serial.println(WiFi.macAddress());
    WiFi.begin(ssid, password);
}

void NetWork_Proc()
{
    static unsigned long previousMillis = 0;
    unsigned long currentMillis = millis();

    Network_Send_Proc();

    // Check if 10 second has passed
    if (currentMillis - previousMillis < 30000)
        return;
    previousMillis = currentMillis;

    switch (WiFi.status())
    {
    case WL_NO_SSID_AVAIL:
        Serial.println("[WiFi] SSID not found");
        break;
    case WL_CONNECT_FAILED:
        WiFi_State = 0;
        Serial.print("[WiFi] Failed - WiFi not connected! Reason: ");
        return;
        break;
    case WL_CONNECTION_LOST:
        WiFi_State = 0;
        Serial.println("[WiFi] Connection was lost");
        break;
    case WL_SCAN_COMPLETED:
        Serial.println("[WiFi] Scan is completed");
        break;
    case WL_DISCONNECTED:
        WiFi_State = 0;
        // Serial.println("[WiFi] WiFi is disconnected");
        break;
    case WL_CONNECTED:
        WiFi_State = 1;
        Serial.println("[WiFi] WiFi is connected!");
        Serial.print("[WiFi] IP address: ");
        Serial.println(WiFi.localIP());
        Get_Time();
        // get_SettingData();
        return;
        break;
    default:
        Serial.print("[WiFi] WiFi Status: ");
        Serial.println(WiFi.status());
        break;
    }
}

// Function to get time in "XX:XX" format
String getFormattedTime()
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
    {
        Serial.println("Failed to obtain time");
        return "00:00";
    }

    char timeStr[6];                                        // Buffer to hold "XX:XX" format
    strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo); // Format hours and minutes
    return String(timeStr);
}

void Send_Trip_Data1()
{
    // return;
    if (WiFi.status() == WL_CONNECTED)
    {
        WiFiClient client;
        HTTPClient http;

        // If you need an HTTP request with a content type: application/json, use the following:

        String url = String(PostTripURL) + myTripData.toQueryString();
        printf(url.c_str());
        http.begin(url);                      // Specify the URL
        int httpResponseCode = http.POST(""); // Send the POST request

        if (httpResponseCode == 301)
        {
            Serial.println(http.getString());
            http.end();
        }
        Serial.print("HTTP Response code: ");
        Serial.println(httpResponseCode);

        // Free resources
        http.end();
    }
    else
    {
        // Serial.println("Wifi is not connected");
    }
}

void Send_Trip_Data()
{
    String TripPostData = String(PostTripURL) + myTripData.toQueryString();
    SetToQueue(TripPostData);
}

void Start_Trip(uint32_t id)
{
    String TripPostData = String(PostTripStartURL) + id;
    SetToQueue(TripPostData);
}

void Stop_Trip(uint32_t id)
{
    String TripPostData = String(PostTripStopURL) + id;
    SetToQueue(TripPostData);
}

void Send_Service_Data(uint8_t service_num, uint8_t start)
{
    String TripPostData = String(PostServiceURL) + "&serial=" + String(SERIAL_NUMBER) + "&serviceNum=" + mySetting.getService(service_num - 1) + "&start=" + start;
    SetToQueue(TripPostData);
}

String settingReading;

void get_SettingData()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        HTTPClient http;
        String macStr = WiFi.macAddress();
        http.begin(GetSettingURL+macStr); // Replace `GetSettingURL` with your URL string
        int httpResponseCode = http.GET();

        if (httpResponseCode == 200) // HTTP OK
        {
            String response = http.getString();
            Serial.println("Raw Response:");
            Serial.println(response); // Debug: print raw response

            // Parse JSON response using ArduinoJson
            JsonDocument doc = StaticJsonDocument<1024>();

            // StaticJsonDocument<1024> doc; // Adjust size if needed
            DeserializationError error = deserializeJson(doc, response);

            if (error)
            {
                Serial.print("JSON deserialization failed: ");
                Serial.println(error.f_str());
                return;
            }

            // Populate TSetting object from parsed JSON
            mySetting.fromJson(response);

            // Print attributes to Serial Monitor
            mySetting.printAttributes();

            // Optionally, save the response to a global variable for later use
            settingReading = response;

            beep(4000, 200);
            delay(500);
            beep(4000, 200);
        }
        else
        {
            Serial.print("HTTP GET Request failed with code: ");
            Serial.println(httpResponseCode);
        }

        http.end();
    }
    else
    {
        // Serial.println("WiFi is not connected");
    }
}

void printLocalTime()
{

    if (!getLocalTime(&timeinfo))
    {
        Serial.println("Failed to obtain time");
        return;
    }
    Serial.println(&timeinfo, "Current time: %A, %B %d %Y %H:%M:%S");
    Hours = timeinfo.tm_hour;
    Min = timeinfo.tm_min;
}

void Get_Time()
{
    // ntpServer
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(1000);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected");

    // Initialize and configure NTP
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    // Fetch and display the time
    printLocalTime();
}

void SetToQueue(String s)
{
    // Serial.print(GREEN);
    //  Serial.println(s);
    //  Serial.print(RESET);

    for (int i = 0; i < 10; i++)
    {

        if (PostDataQueue[i] == "")
        {
            // Serial.print("-----");
            // Serial.println(PostDataQueue[i]);
            PostDataQueue[i] = s;
            // Serial.print("PostData");
            // Serial.println(PostDataQueue[i]);

            Serial.print(YELLOW);
            Serial.print("Save to :");
            Serial.println(i);
            Serial.print(RESET);

            break;
        }
        else
        {
            // Serial.print(RED);
            // Serial.println(".....FULL......");
            // Serial.print(RESET);
        }
    }
}

void Network_Send_Proc()
{
    static int idx = 0;

    if (KeyB.RestTime < 3000)
        return;

    // sprintf(DispStr1, "123456  ");
    //  currentMillis = millis();
    //  if (currentMillis - previousMillis >= 50)
    //      ;

    if (WiFi.status() == WL_CONNECTED)
    {
        WiFiClient client;
        HTTPClient http;

        // for (int i = 0; i < 10; i++)
        // {
        //     if (PostDataQueue[i] != "")
        //     {
        //         Serial.print("Data on loc:");
        //         Serial.println(i);
        //         Serial.println("");
        //     }
        // }

        if (PostDataQueue[idx] != "")
        {
            Serial.println("Try to send....");
            Net_Busy = true;
            // If you need an HTTP request with a content type: application/json, use the following:
            String url = PostDataQueue[idx];
            // Serial.print(GREEN);
            // Serial.println(url);
            // Serial.print(RESET);
            // printf(url.c_str());
            //printf("Http Request Sent.");
            http.begin(url);                      // Specify the URL
            int httpResponseCode = http.POST(""); // Send the POST request

            if (httpResponseCode == 301 || httpResponseCode == 200)
            {
                PostDataQueue[idx] = "";
                Serial.print(YELLOW);
                Serial.println("SEND!");
                Serial.print(RESET);
                http.end();
            }
            else
            {
                Serial.print(RED);
                Serial.println("...............Error");
                Serial.print(RESET);
                // Serial.print("HTTP Response code: ");
                // Serial.println(httpResponseCode);

                /* PostDataQueue[idx] = "";
                 idx++;
                 if (idx > 9)
                     idx = 0;
                */
                // Free resources
                http.end();
            }
        }
        else
        {
            // Serial.println("WiFi is not connected");
        }

        idx++;
        if (idx > 9)
            idx = 0;

        Net_Busy = false;
    }
}