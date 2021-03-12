#include "Main.h"

// #include <SPI.h>
#include <TFT_eSPI.h>
#include <Button2.h>
#include <EEPROM.h>

TFT_eSPI tft = TFT_eSPI(135, 240);
Button2 button1(35);
Button2 button2(0);
bool loggingEnabled;

#include <INA.h>
INA_Class INA;

namespace
{
    const constexpr char *kLoggingTag = "App";
}

void collectDataPointsTask(void *pvParameters);

void setup()
{
    // default first as it will clear all existing entries
    esp_log_level_set("*", ESP_LOG_INFO);

    esp_log_level_set("App", ESP_LOG_DEBUG);
    esp_log_level_set("Logger", ESP_LOG_VERBOSE);

    ESP_LOGI("Setup", "*** ESP32 DataLogger starting ***");

    Serial.begin(115200);
    Serial.println();
    Serial.setDebugOutput(true);

    tft.init();
    tft.setRotation(1);
    tft.setTextFont(4);
    tft.fillScreen(TFT_BLACK);

    ESP_LOGI("SetupWiFi", "Connecting to %s", ssid);
    if (String(WiFi.SSID()) != String(ssid))
    {
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, password);
    }
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        log_printf(".");
    }
    log_printf("\n");
    ESP_LOGI("SetupWiFi", "Connected! IP address: %s", WiFi.localIP().toString().c_str());

    MDNS.begin(hostName);
    ESP_LOGI("SetupWiFi", "Open http://%s.local/edit to see the file browser", hostName);

    configTzTime("CET-1CEST,M3.5.0/2:00,M10.5.0/3:00:", "pool.ntp.org");

    SPIFFS.begin();
    EEPROM.begin(16);
    setupDataLogger(60, 60 * 5); // account for long delays due to database being queried

    loggingEnabled = EEPROM.read(0) && isDatabaseAccessible();
    button1.setTapHandler([](Button2 &btn) {
        if (loggingEnabled)
            flushQueue();
        loggingEnabled = !loggingEnabled && isDatabaseAccessible();
        EEPROM.write(0, loggingEnabled);
        EEPROM.commit();
    });
    button2.setClickHandler([](Button2 &btn) {
        flushQueue();
    });
    button2.setReleasedHandler([](Button2 &btn) {
        if (btn.wasPressedFor() > 2000)
            resetDb();
    });

    setupWebServer();

    uint8_t devicesFound = INA.begin(1, 100000); // Expected max Amp & shunt resistance
    ESP_LOGW(kLoggingTag, "Detected %d INA devices on the I2C bus", devicesFound);
    if (devicesFound != 1)
        while (true)
            ;
    ESP_LOGI(kLoggingTag, "INA device address: %d, name: %s", INA.getDeviceAddress(), INA.getDeviceName());
    INA.setBusConversion(8500);            // Maximum conversion time 8.244ms
    INA.setShuntConversion(8500);          // Maximum conversion time 8.244ms
    INA.setAveraging(64);                  // Average each reading n-times
    INA.setMode(INA_MODE_CONTINUOUS_BOTH); // Bus/shunt measured continuously

    xTaskCreate(collectDataPointsTask, "collectDataPoints", 8192 * 2, nullptr, uxTaskPriorityGet(nullptr) + 1, nullptr);
}

void loop()
{
    button1.loop();
    button2.loop();
    loopWebServer();
}

void collectDataPointsTask(void *pvParameters)
{
    ESP_LOGD(kLoggingTag, "Entering collectDataPointsTask()");

    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    int tftWidth = tft.width();
    int fontHeight = tft.fontHeight();

    tft.setTextDatum(TL_DATUM); // left aligned
    int alignPosX = tftWidth - max(tft.textWidth(" mA"), tft.textWidth(" V"));
    tft.drawString(" mA", alignPosX, fontHeight * 0);
    int cPadding = tft.textWidth("-32000");
    tft.drawString(" V", alignPosX, fontHeight * 1);
    int vPadding = tft.textWidth("-20.00");

    TickType_t xLastWakeTime = xTaskGetTickCount();
    int latestRecordsCounter = 0;
    for (;;)
    {
        Record record;

        record.currentMilliAmps = INA.getBusMicroAmps() / 1000;
        ESP_LOGD(kLoggingTag, "currentMilliAmps: %f", record.currentMilliAmps);
        record.voltageMilliVolts = INA.getBusMilliVolts();
        ESP_LOGD(kLoggingTag, "voltageMilliVolts: %f", record.voltageMilliVolts);

        if (loggingEnabled)
            addRecord(record, latestRecordsCounter % 10 == 0);
        latestRecordsCounter++;

        tft.setTextSize(2);
        tft.setTextDatum(TR_DATUM); // right aligned
        tft.setTextPadding(cPadding);
        tft.drawFloat(record.currentMilliAmps, 0, alignPosX, fontHeight * 0);
        tft.setTextPadding(vPadding);
        tft.drawFloat(record.voltageMilliVolts / 1000, 2, alignPosX, fontHeight * 1);
        tft.setTextPadding(0);

        tft.setTextSize(1);
        tft.setTextDatum(TL_DATUM); // left aligned
        tft.setCursor(0, fontHeight * 2);
        tft.printf("Lg: %d, Qu: %d, Db: %d   ", loggingEnabled, getQueueSize(), dbFileExists(true));

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));
    }
}
