#include "Main.h"

#include <ArduinoOTA.h>
#include <FS.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <SPIFFSEditor.h>

AsyncWebServer asyncWebServer(80);

void setupWebServer()
{
    // //Send OTA events to the browser
    // ArduinoOTA.onStart([]() { events.send("Update Start", "ota"); });
    // ArduinoOTA.onEnd([]() { events.send("Update End", "ota"); });
    // ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    //     char p[32];
    //     sprintf(p, "Progress: %u%%\n", (progress / (total / 100)));
    //     events.send(p, "ota");
    // });
    // ArduinoOTA.onError([](ota_error_t error) {
    //     if (error == OTA_AUTH_ERROR)
    //         events.send("Auth Failed", "ota");
    //     else if (error == OTA_BEGIN_ERROR)
    //         events.send("Begin Failed", "ota");
    //     else if (error == OTA_CONNECT_ERROR)
    //         events.send("Connect Failed", "ota");
    //     else if (error == OTA_RECEIVE_ERROR)
    //         events.send("Recieve Failed", "ota");
    //     else if (error == OTA_END_ERROR)
    //         events.send("End Failed", "ota");
    // });
    ArduinoOTA.setHostname(hostName);
    ArduinoOTA.begin();

    MDNS.addService("http", "tcp", 80);

    asyncWebServer.addHandler(new SPIFFSEditor(SPIFFS, spiffsEditorUsername, spiffsEditorPassword));

    asyncWebServer.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", String(ESP.getFreeHeap()));
    });

    asyncWebServer.onNotFound([](AsyncWebServerRequest *request) {
        Serial.printf("NOT_FOUND: ");
        if (request->method() == HTTP_GET)
            Serial.printf("GET");
        else if (request->method() == HTTP_POST)
            Serial.printf("POST");
        else if (request->method() == HTTP_DELETE)
            Serial.printf("DELETE");
        else if (request->method() == HTTP_PUT)
            Serial.printf("PUT");
        else if (request->method() == HTTP_PATCH)
            Serial.printf("PATCH");
        else if (request->method() == HTTP_HEAD)
            Serial.printf("HEAD");
        else if (request->method() == HTTP_OPTIONS)
            Serial.printf("OPTIONS");
        else
            Serial.printf("UNKNOWN");
        Serial.printf(" http://%s%s\n", request->host().c_str(), request->url().c_str());

        if (request->contentLength())
        {
            Serial.printf("_CONTENT_TYPE: %s\n", request->contentType().c_str());
            Serial.printf("_CONTENT_LENGTH: %u\n", request->contentLength());
        }

        int headers = request->headers();
        int i;
        for (i = 0; i < headers; i++)
        {
            AsyncWebHeader *h = request->getHeader(i);
            Serial.printf("_HEADER[%s]: %s\n", h->name().c_str(), h->value().c_str());
        }

        int params = request->params();
        for (i = 0; i < params; i++)
        {
            AsyncWebParameter *p = request->getParam(i);
            if (p->isFile())
            {
                Serial.printf("_FILE[%s]: %s, size: %u\n", p->name().c_str(), p->value().c_str(), p->size());
            }
            else if (p->isPost())
            {
                Serial.printf("_POST[%s]: %s\n", p->name().c_str(), p->value().c_str());
            }
            else
            {
                Serial.printf("_GET[%s]: %s\n", p->name().c_str(), p->value().c_str());
            }
        }

        request->send(404);
    });

    asyncWebServer.begin();
}

void loopWebServer()
{
    ArduinoOTA.handle();
}
