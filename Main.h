#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "ulog_sqlite.h"

#include <Esp32Logging.hpp>

#include "Consts.h"

struct Record
{
    time_t timestamp;
    float currentMilliAmps;
    float voltageMilliVolts;

    Record()
    {
        time(&timestamp);
    }

    static constexpr int ColumnCount = 3;
    int AppendToDb(struct dblog_write_context *wctx)
    {
        static uint8_t types[] = {DBLOG_TYPE_INT, DBLOG_TYPE_REAL, DBLOG_TYPE_REAL};
        void *values[] = {&timestamp, &currentMilliAmps, &voltageMilliVolts};
        static uint16_t lengths[] = {sizeof(time_t), sizeof(float), sizeof(float)};
        return dblog_append_row_with_values(wctx, types, (const void **)values, lengths);
    }
};

extern AsyncWebServer asyncWebServer;

void setupWebServer();
void loopWebServer();
