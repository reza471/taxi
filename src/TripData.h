#ifndef TRIPDATA_H
#define TRIPDATA_H

#include <Arduino.h>      // Include Arduino library for String support
#include <Arduino_JSON.h> // Include the Arduino_JSON library

class TripData
{
private:
    String driver_id;
    String serial;
    long bt_number;
    String start_time;
    String stop_time;
    long trip_distance;
    long trip_duration;
    long traffic_distance;
    long traffic_duration;
    int avg_speed;
    int max_speed;
    long trip_price;
    long traffic_price;
    long night_price;
    long total_price;

public:
    // Default constructor
    TripData()
        : driver_id(""), serial(""), bt_number(0), traffic_duration(0), avg_speed(0), max_speed(0),
          trip_price(0), traffic_price(0), night_price(0), total_price(0) {}

    // Parameterized constructor
    TripData(String driver_id, String serial, long bt_number, String start_time, String stop_time, long trip_distance, long trip_duration,
             long traffic_distance, long traffic_duration, int avg_speed, int max_speed,
             long trip_price, long traffic_price, long night_price, long total_price)
        : driver_id(driver_id), serial(serial), start_time(start_time), stop_time(stop_time), trip_distance(trip_distance),
          trip_duration(trip_duration), traffic_distance(traffic_distance), traffic_duration(traffic_duration),
          avg_speed(avg_speed), max_speed(max_speed), trip_price(trip_price), traffic_price(traffic_price),
          night_price(night_price), total_price(total_price) {}

    // Getters and setters
    String getDriverId() const { return driver_id; }
    void setDriverId(String value) { driver_id = value; }

    String getSerial() const { return serial; }
    void setSerial(String value) { serial = value; }

    long getbt_number() const { return bt_number; }
    void setbt_number(long value) { bt_number = value; }

    String getStartTime() const { return start_time; }
    void setStartTime(const String &value) { start_time = value; }

    String getStopTime() const { return stop_time; }
    void setStopTime(const String &value) { stop_time = value; }

    long getTripDistance() const { return trip_distance; }
    void setTripDistance(long value) { trip_distance = value; }

    long getTripDuration() const { return trip_duration; }
    void setTripDuration(long value) { trip_duration = value; }

    long getTrafficDistance() const { return traffic_distance; }
    void setTrafficDistance(long value) { traffic_distance = value; }

    long getTrafficDuration() const { return traffic_duration; }
    void setTrafficDuration(long value) { traffic_duration = value; }

    int getAvgSpeed() const { return avg_speed; }
    void setAvgSpeed(int value) { avg_speed = value; }

    int getMaxSpeed() const { return max_speed; }
    void setMaxSpeed(int value) { max_speed = value; }

    long getTripPrice() const { return trip_price; }
    void setTripPrice(long value) { trip_price = value; }

    long getTrafficPrice() const { return traffic_price; }
    void setTrafficPrice(long value) { traffic_price = value; }

    long getNightPrice() const { return night_price; }
    void setNightPrice(long value) { night_price = value; }

    long getTotalPrice() const { return total_price; }
    void setTotalPrice(long value) { total_price = value; }

    // Method to convert class data to JSON string using Arduino_JSON
    String toJsonString()
    {
        JSONVar jsonObj;

        jsonObj["driver_id"] = driver_id;
        jsonObj["serial"] = serial;
        jsonObj["bt_number"] = bt_number;
        jsonObj["start_time"] = start_time;
        jsonObj["stop_time"] = stop_time;
        jsonObj["trip_distance"] = trip_distance;
        jsonObj["trip_duration"] = trip_duration;
        jsonObj["traffic_distance"] = traffic_distance;
        jsonObj["traffic_duration"] = traffic_duration;
        jsonObj["avg_speed"] = avg_speed;
        jsonObj["max_speed"] = max_speed;
        jsonObj["trip_price"] = trip_price;
        jsonObj["traffic_price"] = traffic_price;
        jsonObj["night_price"] = night_price;
        jsonObj["total_price"] = total_price;

        // Convert JSON object to string
        return JSON.stringify(jsonObj);
    }

    // Method to generate query string
    String toQueryString() const
    {
        return "?driver_id=" + String(driver_id) +
               "&serial=" + serial +
               "&bt_number=" + bt_number +
               "&start_time=" + start_time +
               "&stop_time=" + stop_time +
               "&trip_distance=" + String(trip_distance) +
               "&trip_duration=" + String(trip_duration) +
               "&traffic_distance=" + String(traffic_distance) +
               "&traffic_duration=" + String(traffic_duration) +
               "&avg_speed=" + String(avg_speed) +
               "&max_speed=" + String(max_speed) +
               "&trip_price=" + String(trip_price) +
               "&traffic_price=" + String(traffic_price) +
               "&night_price=" + String(night_price) +
               "&total_price=" + String(total_price);
    }
};

#endif // TRIPDATA_H
