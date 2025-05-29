#ifndef TSETTING_H
#define TSETTING_H

#include "ArduinoJson.h"
#include "SPIFFS.h"

#define EEPROM_SIZE 100

class TSetting
{
private:
    int PM;
    int pri_cost;
    int Start_night;
    int Stop_night;
    int Cost_200;
    int Cost_stop;
    int Max_stop_km;
    int free_trip;
    int free_time;
    int Percent_night;
    int fraction;
    int pri2_cost;
    int Cost2_200;
    int Cost2_stop;
    int samar;
    int darbast;
    int korsi;
    int nobat;
    int taxi;

    // Using a fixed-size array for service
    int service[10];      // Adjust size as needed
    int serviceCount = 0; // To track how many services are in the array

public:
    // Default constructor
    TSetting()
        : PM(0), pri_cost(0), Start_night(0), Stop_night(0),
          Cost_200(0), Cost_stop(0), Max_stop_km(0), free_trip(0), free_time(0),
          Percent_night(0), fraction(0), pri2_cost(0),
          Cost2_200(0), Cost2_stop(0), taxi(0), samar(0), darbast(0),
          korsi(0), nobat(0)
    {
        memset(service, 0, sizeof(service)); // Initialize service array

        if (!SPIFFS.begin())
        {
            Serial.println("SPIFFS Mount Failed");
            return;
        }
        // Read from file
    }
    // Getters
    int getPM() const { return PM; }
    int getPriCost() const { return pri_cost; }
    int getStartNight() const { return Start_night; }
    int getStopNight() const { return Stop_night; }
    int getCost200() const { return Cost_200; }
    int getCostStop() const { return Cost_stop; }
    int getMaxStopKm() const { return Max_stop_km; }
    int getFreeTrip() const { return free_trip; }
    int getFreeTime() const { return free_time; }
    int getPercentNight() const { return Percent_night; }
    int getFraction() const { return fraction; }
    int getTaxi() const { return taxi; }
    int getPri2Cost() const { return pri2_cost; }
    int getCost2200() const { return Cost2_200; }
    int getCost2Stop() const { return Cost2_stop; }
    int getSamar() const { return samar; }
    int getDarbast() const { return darbast; }
    int getKorsi() const { return korsi; }
    int getNobat() const { return nobat; }

    // Getter for the service array
    int getService(int i) { return service[i]; }
    int getServiceCount() { return serviceCount; }

    // Setters
    void setPM(int value) { PM = value; }
    void setPriCost(int value) { pri_cost = value; }
    void setStartNight(int value) { Start_night = value; }
    void setStopNight(int value) { Stop_night = value; }
    void setCost200(int value) { Cost_200 = value; }
    void setCostStop(int value) { Cost_stop = value; }
    void setMaxStopKm(int value) { Max_stop_km = value; }
    void setFreeTrip(int value) { free_trip = value; }
    void setFreeTime(int value) { free_time = value; }
    void setPercentNight(int value) { Percent_night = value; }
    void setFraction(int value) { fraction = value; }
    void setTaxi(int value) { taxi = value; }
    void setPri2Cost(int value) { pri2_cost = value; }
    void setCost2200(int value) { Cost2_200 = value; }
    void setCost2Stop(int value) { Cost2_stop = value; }
    void setSamar(int value) { samar = value; }
    void setDarbast(int value) { darbast = value; }
    void setKorsi(int value) { korsi = value; }
    void setNobat(int value) { nobat = value; }

    // Write object data to LittleFS
    void writeToFile()
    {
        File file = SPIFFS.open("/mysetting.conf", FILE_WRITE); // Open file for writing

        if (!file)
        {
            Serial.println("Failed to open file for writing");
        }

        // Create a JSON document to store the data
        JsonDocument doc = StaticJsonDocument<1024>();

        // StaticJsonDocument<1024> doc;

        doc["PM"] = PM;
        doc["pri_cost"] = pri_cost;
        doc["Start_night"] = Start_night;
        doc["Stop_night"] = Stop_night;
        doc["Cost_200"] = Cost_200;
        doc["Cost_stop"] = Cost_stop;
        doc["Max_stop_km"] = Max_stop_km;
        doc["free_trip"] = free_trip;
        doc["free_time"] = free_time;
        doc["Percent_night"] = Percent_night;
        doc["fraction"] = fraction;
        doc["taxi"] = taxi;
        doc["pri2_cost"] = pri2_cost;
        doc["Cost2_200"] = Cost2_200;
        doc["Cost2_stop"] = Cost2_stop;
        doc["samar"] = samar;
        doc["darbast"] = darbast;
        doc["korsi"] = korsi;
        doc["nobat"] = nobat;

        // Write service array
        JsonArray serviceArray = doc.createNestedArray("service");
        for (int i = 0; i < 10; i++)
        {
            serviceArray.add(service[i]);
        }

        // Serialize the document to a string
        String jsonStr;
        serializeJson(doc, jsonStr);

        // Write the JSON string to the file
        file.print(jsonStr);

        file.close(); // Close the file after writing
    }

    void fromJson(const String &jsonStr)
    {
        JsonDocument taxiFare = StaticJsonDocument<1024>();

        // StaticJsonDocument<1024> taxiFare; // Adjust size if needed
        DeserializationError error = deserializeJson(taxiFare, jsonStr);

        if (error)
        {
            Serial.print("JSON deserialization failed: ");
            Serial.println(error.f_str());
            return;
        }

        // Serial.println("Here");

        // Map the 'taxiFare' object in the JSON to the class variables
        // JsonObject taxiFare = doc["taxiFare"];

        PM = taxiFare["PM"] | 0;
        pri_cost = taxiFare["pri_cost"] | 0;
        Start_night = taxiFare["Start_night"] | 0;
        Stop_night = taxiFare["Stop_night"] | 0;
        Cost_200 = taxiFare["Cost_200"] | 0;
        Cost_stop = taxiFare["Cost_stop"] | 0;
        Max_stop_km = taxiFare["Max_stop_km"] | 0;
        free_trip = taxiFare["free_trip"] | 0;
        free_time = taxiFare["free_time"] | 0;
        Percent_night = taxiFare["Percent_night"] | 0;
        fraction = taxiFare["fraction"] | 0;
        taxi = taxiFare["taxi"] | 0;
        pri2_cost = taxiFare["pri2_cost"] | 0;
        Cost2_200 = taxiFare["Cost2_200"] | 0;
        Cost2_stop = taxiFare["Cost2_stop"] | 0;
        samar = taxiFare["samar"] | 0;
        darbast = taxiFare["darbast"] | 0;
        korsi = taxiFare["korsi"] | 0;
        nobat = taxiFare["nobat"] | 0;

        // Parse the 'service' array
        JsonArray serviceArray = taxiFare["service"];
        serviceCount = 0;
        for (JsonVariant v : serviceArray)
        {
            if (serviceCount < 10) // Avoid exceeding array bounds
            {
                service[serviceCount++] = v.as<int>();
            }
        }
        writeToFile();
    }

    // Read object data from LittleFS
    void readFromFile()
    {
        File file = SPIFFS.open("/mysetting.conf"); // Open file for reading

        if (!file)
        {
            Serial.println("Failed to open file for reading");
            return;
        }

        // Read the entire file into a String
        String jsonStr = file.readString();

        file.close(); // Close the file after reading

        // Parse the JSON string to populate the class attributes
        JsonDocument doc = StaticJsonDocument<1024>();

        // StaticJsonDocument<1024> doc;
        DeserializationError error = deserializeJson(doc, jsonStr);

        if (error)
        {
            Serial.print("JSON deserialization failed: ");
            Serial.println(error.f_str());
            return;
        }

        // Map the JSON object to class variables
        PM = doc["PM"];
        pri_cost = doc["pri_cost"];
        Start_night = doc["Start_night"];
        Stop_night = doc["Stop_night"];
        Cost_200 = doc["Cost_200"];
        Cost_stop = doc["Cost_stop"];
        Max_stop_km = doc["Max_stop_km"];
        free_trip = doc["free_trip"];
        free_time = doc["free_time"];
        Percent_night = doc["Percent_night"];
        fraction = doc["fraction"];
        taxi = doc["taxi"];
        pri2_cost = doc["pri2_cost"];
        Cost2_200 = doc["Cost2_200"];
        Cost2_stop = doc["Cost2_stop"];
        samar = doc["samar"];
        darbast = doc["darbast"];
        korsi = doc["korsi"];
        nobat = doc["nobat"];

        // Read the 'service' array
        JsonArray serviceArray = doc["service"].as<JsonArray>();
        serviceCount = 0;
        for (JsonVariant v : serviceArray)
        {
            if (serviceCount < 10) // Avoid exceeding array bounds
            {
                service[serviceCount++] = v.as<int>();
            }
        }
    }

    // Debug method to print all attributes to Serial
    void printAttributes()
    {
        Serial.println("Attributes:");
        Serial.println("PM: " + String(PM));
        Serial.println("Primary Cost: " + String(pri_cost));
        Serial.println("Start Night: " + String(Start_night));
        Serial.println("Stop Night: " + String(Stop_night));
        Serial.println("Cost 200m: " + String(Cost_200));
        Serial.println("Cost Stop: " + String(Cost_stop));
        Serial.println("Max Stop KM: " + String(Max_stop_km));
        Serial.println("Free Trip: " + String(free_trip));
        Serial.println("Free Time: " + String(free_time));
        Serial.println("Percent Night: " + String(Percent_night));
        Serial.println("Fraction: " + String(fraction));
        Serial.println("Taxi: " + String(taxi));
        Serial.println("Pri2 Cost: " + String(pri2_cost));
        Serial.println("Cost2 200m: " + String(Cost2_200));
        Serial.println("Cost2 Stop: " + String(Cost2_stop));
        Serial.println("Samar: " + String(samar));
        Serial.println("Darbast: " + String(darbast));
        Serial.println("Korsi: " + String(korsi));
        Serial.println("Nobat: " + String(nobat));

        // Print the 'service' array
        Serial.println("Service Array:");
        for (int i = 0; i < serviceCount; i++)
        {
            Serial.println(service[i]);
        }
    }
};

#endif
