#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>
#include <StreamString.h>

#include "ulog_sqlite.h"

#include <Esp32Logging.hpp>

#include "Consts.h"

//
// Record

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
    int AppendToDb(struct dblog_write_context *wctx) const
    {
        static uint8_t types[] = {DBLOG_TYPE_INT, DBLOG_TYPE_REAL, DBLOG_TYPE_REAL};
        const void *values[] = {&timestamp, &currentMilliAmps, &voltageMilliVolts};
        static uint16_t lengths[] = {sizeof(time_t), sizeof(float), sizeof(float)};
        return dblog_append_row_with_values(wctx, types, values, lengths);
    }
    String toJsonString() const
    {
        StreamString stringBuffer;
        stringBuffer.printf("[%ld,%f,%f]", timestamp, currentMilliAmps, voltageMilliVolts);
        return stringBuffer;
    }

    static constexpr int JsonMaxChars = 30;
};

//
// DataLogger.cpp

void setupDataLogger(int flushEverySeconds, int queueLength);
bool isDatabaseAccessible();
bool addRecord(const Record &record, bool addToRingbuffer);
void flushQueue();
uint getQueueSize();
void resetDb();
bool dbFileExists(bool noLog = false);

static constexpr size_t latestRecordsBufferSize = 6;

//
// WebserverAsync.cpp

extern AsyncWebServer asyncWebServer;

void setupWebServer();
void loopWebServer();
