#ifndef _NETWORK_H_
#define _NETWORK_H_

#include "main.h"

#ifdef __cplusplus
extern "C"
{
#endif

    extern struct tm timeinfo;
    extern uint8_t WiFi_State;

    void NetWork_Init();
    void NetWork_Proc();
    void Send_Trip_Data();
    void Send_Service_Data(uint8_t service_num, uint8_t start);
    void get_SettingData(uint8_t);
    void Start_Trip(uint32_t id);
    void Stop_Trip(uint32_t id);
    String getFormattedTime();
    void Network_Send_Proc();

#ifdef __cplusplus
}
#endif

#endif // _NETWORK_H_
