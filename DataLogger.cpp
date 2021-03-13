#include "Main.h"
#include "DataLogger.hpp"
#include <CircularBuffer.h>

#include <byteswap.h>

namespace
{
    const constexpr char *kLoggingTag = "Logger";

    AsyncEventSource events("/dataevents");

    const constexpr char *dbFilename = "/spiffs/Esp32DataLogger.db";
    const constexpr int dbPageSizeExp = 12; // 4096
    const constexpr char *dbFilenameWithoutFs = &dbFilename[7];

    SemaphoreHandle_t dbMutex = xSemaphoreCreateMutex();
    FILE *dbFile;
    byte dbBuffer[1 << dbPageSizeExp];

    bool dbAccessible = false;
    int flushEveryMillis;
    QueueHandle_t recordQueueHandle;
    TaskHandle_t queueTaskHandle;

    struct dblog_read_context dataReadDbContext;
    time_t dataReadLastTimestamp;
    bool dataReadFinalize;

    SemaphoreHandle_t latestRecordsMutex = xSemaphoreCreateMutex();
    CircularBuffer<Record, latestRecordsBufferSize> latestRecordsBuffer;
}

void setupDataLogger(int flushEverySeconds, int queueLength)
{
    ESP_LOGD(kLoggingTag, "Entering setupDataLogger()");

    recoverDb();

    flushEveryMillis = flushEverySeconds * 1000;
    recordQueueHandle = xQueueCreate(queueLength, sizeof(Record));

    auto createTaskResult = xTaskCreate(queueTask, "recordQueue", 8192 * 2, nullptr, uxTaskPriorityGet(nullptr), &queueTaskHandle);
    if (createTaskResult != pdPASS)
    {
        ESP_LOGE(kLoggingTag, "Error %d creating task", createTaskResult);
        while (true)
            ;
    }

    asyncWebServer.serveStatic("/", SPIFFS, "/").setDefaultFile("index.htm");
    asyncWebServer.on("/data", HTTP_GET, dataResponseHandler);

    events.onConnect([](AsyncEventSourceClient *client) {
        ESP_LOGI(kLoggingTag, "SSE client connected");
        if (xSemaphoreTake(latestRecordsMutex, 100) == pdTRUE)
        {
            // TODO: combine multiple records into one event message as JSON array or do this in a separate (one-off) task
            using index_t = decltype(latestRecordsBuffer)::index_t;
            for (index_t i = 0; i < latestRecordsBuffer.size(); i++)
            {
                ESP_LOGI(kLoggingTag, "Sending latestRecordsBuffer[%d]", i);
                // only send a few samples here to avoid AsyncTCP locking up
                events.send(latestRecordsBuffer[i].toJsonString().c_str());
            }
            xSemaphoreGive(latestRecordsMutex);
        }
    });
    asyncWebServer.addHandler(&events);
}

bool isDatabaseAccessible()
{
    return dbAccessible;
}

bool addRecord(const Record &record, bool addToRingbuffer)
{
    ESP_LOGD(kLoggingTag, "Entering addRecord()");

    events.send(record.toJsonString().c_str());

    if (addToRingbuffer && xSemaphoreTake(latestRecordsMutex, 100) == pdTRUE)
    {
        latestRecordsBuffer.push(record);
        xSemaphoreGive(latestRecordsMutex);
    }

    return xQueueSendToBack(recordQueueHandle, &record, 0) == pdPASS;
}

void flushQueue()
{
    ESP_LOGD(kLoggingTag, "Entering flushQueue()");

    xTaskNotify(queueTaskHandle, 0, eNoAction);
}

uint getQueueSize()
{
    return uxQueueMessagesWaiting(recordQueueHandle);
}

void queueTask(void *taskParameter)
{
    ESP_LOGD(kLoggingTag, "Entering queueTask()");

    while (true)
    {
        xTaskNotifyWait(0, 0, nullptr, pdMS_TO_TICKS(flushEveryMillis));
        queueTaskFlush();
    }

    vTaskDelete(nullptr);
}

void queueTaskFlush()
{
    ESP_LOGD(kLoggingTag, "Entering queueTaskFlush()");

    auto messagesWaiting = uxQueueMessagesWaiting(recordQueueHandle);
    if (!messagesWaiting)
    {
        ESP_LOGI(kLoggingTag, "Queue is empty, nothing to flush");
        return;
    }

    ESP_LOGI(kLoggingTag, "Flushing queue");

    if (!aquireDbMutex(flushEveryMillis * 10, __func__))
        return;

    bool fileExists;
    dbFile = nullptr;
    int res;
    struct dblog_write_context ctx;
    ctx.buf = dbBuffer;
    ctx.col_count = Record::ColumnCount;
    ctx.page_size_exp = dbPageSizeExp;
    ctx.read_fn = read_fn_wctx;
    ctx.write_fn = write_fn;
    ctx.flush_fn = flush_fn;
    Record record;

    fileExists = dbFileExists();

    dbFile = fopen(dbFilename, !fileExists ? "w+b" : "r+b");
    if (!dbFile)
    {
        ESP_LOGE(kLoggingTag, "Error opening/creating database file '%s'", dbFilename);
        goto exit;
    }

    res = !fileExists ? dblog_write_init(&ctx) : dblog_init_for_append(&ctx);
    if (res)
    {
        ESP_LOGE(kLoggingTag, "dblog_write_init or dblog_init_for_append returned error %d", res);
        goto exit;
    }

    while (xQueueReceive(recordQueueHandle, &record, 0) == pdTRUE)
    {
        ESP_LOGI(kLoggingTag, "Adding record with timestamp %ld", record.timestamp);
        res = record.AppendToDb(&ctx);
        if (res)
        {
            ESP_LOGE(kLoggingTag, "AppendToDb returned error %d", res);
            goto exit;
        }
    }

    ESP_LOGI(kLoggingTag, "Finalizing database");
    res = dblog_finalize(&ctx);
    if (res)
    {
        ESP_LOGE(kLoggingTag, "dblog_finalize returned error %d", res);
        goto exit;
    }

    ESP_LOGI(kLoggingTag, "    Done flushing queue and adding records");

exit:
    if (dbFile)
        fclose(dbFile);
    ESP_LOGV(kLoggingTag, "Mutex: xSemaphoreGive");
    releaseDbMutex(__func__);
}

bool recoverDb()
{
    ESP_LOGI(kLoggingTag, "Checking / recovering database");

    if (!aquireDbMutex(1000 * 10, __func__))
        return false;

    bool result = false;
    dbFile = nullptr;
    int res;
    struct dblog_write_context ctx;
    ctx.buf = dbBuffer;
    ctx.read_fn = read_fn_wctx;
    ctx.write_fn = write_fn;
    ctx.flush_fn = flush_fn;
    int32_t page_size;

    if (!dbFileExists())
    {
        result = true;
        goto exit;
    }

    dbFile = fopen(dbFilename, "r+b");
    if (!dbFile)
    {
        ESP_LOGE(kLoggingTag, "Error opening database file '%s'", dbFilename);
        goto exit;
    }

    page_size = dblog_read_page_size(&ctx);
    ESP_LOGI(kLoggingTag, "Database page size: %d", page_size);
    if (page_size < 512)
    {
        ESP_LOGE(kLoggingTag, "Page size invalid");
        goto exit;
    }

    res = dblog_recover(&ctx);
    if (res)
    {
        ESP_LOGE(kLoggingTag, "dblog_recover returned error %d", res);
        goto exit;
    }

    result = true;
    ESP_LOGI(kLoggingTag, "    Done recovering database");

exit:
    if (dbFile)
        fclose(dbFile);
    releaseDbMutex(__func__);

    dbAccessible = result;

    return result;
}

void resetDb()
{
    ESP_LOGI(kLoggingTag, "Resetting / removing database");
    if (dbFileExists())
    {
        auto removeResult = SPIFFS.remove(dbFilenameWithoutFs);
        ESP_LOGI(kLoggingTag, "Remove result: %d", removeResult);
    }

    ESP_LOGI(kLoggingTag, "Clearing queue");
    xQueueReset(recordQueueHandle);

    dbAccessible = true;
}

bool dbFileExists(bool noLog)
{
    ESP_LOGD(kLoggingTag, "Entering dbFileExists()");

    bool fileExists = SPIFFS.exists(dbFilenameWithoutFs);
    if (!noLog)
        ESP_LOGI(kLoggingTag, "Database file exists: %d", fileExists);
    else
        ESP_LOGD(kLoggingTag, "Database file exists: %d", fileExists);

    return fileExists;
}

void dataResponseHandler(AsyncWebServerRequest *request)
{
    ESP_LOGD(kLoggingTag, "Entering respondWithData()");

    bool sentResponse = false;
    time_t recordsFrom = 0, recordsUntil = 0;
    int res;
    AsyncWebServerResponse *response;

    if (auto param = request->getParam("from"))
        recordsFrom = param->value().toInt();
    if (auto param = request->getParam("until"))
        recordsUntil = param->value().toInt();
    if (!recordsFrom)
    {
        time(&recordsFrom);
        recordsFrom -= 60 * 60;
    }
    ESP_LOGI(kLoggingTag, "Responding with data: recordsFrom = %ld, recordsUntil = %ld", recordsFrom, recordsUntil);

    if (!aquireDbMutex(1000 * 10, __func__))
    {
        request->send(500);
        return;
    }

    if (!dbFileExists())
    {
        request->send(200, "application/json", "[]");
        releaseDbMutex("respondWithData empty");
        sentResponse = true;
        goto exitHandler;
    }

    memset(&dataReadDbContext, 0, sizeof(dataReadDbContext));
    dataReadDbContext.buf = dbBuffer;
    dataReadDbContext.read_fn = read_fn_rctx;

    dbFile = nullptr;
    dbFile = fopen(dbFilename, "rb");
    if (!dbFile)
    {
        ESP_LOGE(kLoggingTag, "Error opening database file '%s'", dbFilename);
        goto exitHandler;
    }
    res = dblog_read_init(&dataReadDbContext);
    if (res)
    {
        ESP_LOGE(kLoggingTag, "dblog_read_init returned error %d", res);
        goto exitHandler;
    }
    ESP_LOGI(kLoggingTag, "Page size: %d, last data page: %d", (int32_t)1 << dataReadDbContext.page_size_exp, dataReadDbContext.last_leaf_page);

    res = dblog_bin_srch_row_by_val(&dataReadDbContext, 0, DBLOG_TYPE_INT, &recordsFrom, sizeof(recordsFrom), 0);
    if (res)
    {
        ESP_LOGE(kLoggingTag, "dblog_bin_srch_row_by_val returned error %d", res);
        goto exitHandler;
    }

    dataReadLastTimestamp = 0;
    dataReadFinalize = false;
    response = request->beginChunkedResponse("application/json", [recordsUntil](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        ESP_LOGV(kLoggingTag, "ChunkedResponse: recordsUntil = %ld, dataReadFinalize = %d, dataReadLastTimestamp = %ld, buffer = %p, maxLen = %d, index = %d",
                 recordsUntil, dataReadFinalize, dataReadLastTimestamp, buffer, maxLen, index);
        uint8_t *workBuffer = buffer;
        size_t lengthRemaining = maxLen;
        size_t bytesWritten;

        if (dataReadFinalize)
            return 0;

        bool isFirstRecord = index == 0;
        while (lengthRemaining > Record::JsonMaxChars)
        {
            *workBuffer = isFirstRecord ? '[' : ',';
            workBuffer += 1;
            lengthRemaining -= 1;

            if (!isFirstRecord)
            {
                if ((recordsUntil && dataReadLastTimestamp >= recordsUntil) || dblog_read_next_row(&dataReadDbContext))
                {
                    *(workBuffer - 1) = ']';
                    dataReadFinalize = true;
                    goto exitResponse;
                }
            }

            auto rowBuffer = rowToBuffer(&dataReadDbContext, recordsUntil ? &dataReadLastTimestamp : nullptr);
            rowBuffer.getBytes(workBuffer, lengthRemaining);
            bytesWritten = rowBuffer.length();
            workBuffer += bytesWritten;
            lengthRemaining -= bytesWritten;

            isFirstRecord = false;
        }

        // completely fill remaining buffer as otherwise we might get called again with a maxLen of 3 or so instead of with a new large buffer...
        for (bytesWritten = 0; bytesWritten < lengthRemaining; bytesWritten++)
            workBuffer[bytesWritten] = ' ';
        workBuffer += bytesWritten;
        lengthRemaining -= bytesWritten;

    exitResponse:
        bytesWritten = maxLen - lengthRemaining;
        ESP_LOGV(kLoggingTag, "ChunkedResponse: bytesWritten = %d, buffer = '%.*s', lengthRemaining = %d, dataReadFinalize = %d, dataReadLastTimestamp = %ld",
                 bytesWritten, bytesWritten, buffer, lengthRemaining, dataReadFinalize, dataReadLastTimestamp);

        return bytesWritten;
    });
    request->onDisconnect([]() {
        fclose(dbFile);
        releaseDbMutex("respondWithData onDisconnect");
    });
    request->send(response);
    sentResponse = true;

exitHandler:
    if (!sentResponse)
    {
        request->send(500);
        if (dbFile)
            fclose(dbFile);
        releaseDbMutex("respondWithData !sentResponse");
    }
}

String rowToBuffer(struct dblog_read_context *ctx, time_t *timestamp)
{
    String buffer((char *)nullptr);
    for (int i = 0; addColumnToBuffer(ctx, i, buffer); i++)
        ;

    if (timestamp)
    {
        uint32_t col_type;
        *timestamp = (time_t)read_int32((const byte *)dblog_read_col_val(ctx, 0, &col_type));
    }

    return buffer;
}

bool addColumnToBuffer(struct dblog_read_context *ctx, int col_idx, String &buffer)
{
    uint32_t col_type;
    const byte *col_val = (const byte *)dblog_read_col_val(ctx, col_idx, &col_type);
    if (!col_val)
    {
        if (col_idx == 0)
            ESP_LOGE(kLoggingTag, "Error reading column value");
        else
            buffer.concat(']');
        return false;
    }

    buffer.concat(col_idx == 0 ? '[' : ',');

    switch (col_type)
    {
    case 0:
        buffer.concat("null");
        break;
    case 1:
        buffer.concat(*((int8_t *)col_val));
        break;
    case 2:
        buffer.concat(read_int16(col_val));
        break;
    case 4:
        buffer.concat(read_int32(col_val));
        break;
    case 6:
    {
        char buf[2 + 3 * sizeof(int64_t)];
        sprintf(buf, "%lld", read_int64(col_val));
        buffer.concat(buf);
    }
    break;
    case 7:
        buffer.concat(read_double(col_val));
        break;
    default:
    {
        if (col_type < 12)
        {
            ESP_LOGE(kLoggingTag, "Unuspported column type %d", col_type);
            return false;
        }

        uint32_t col_len = dblog_derive_data_len(col_type);
        buffer.reserve(buffer.length() + (col_type % 2 ? col_len : col_len * 2));
        for (int j = 0; j < col_len; j++)
        {
            if (col_type % 2)
                buffer.concat((char)col_val[j]);
            else
            {
                buffer.concat(String(col_val[j], HEX));
            }
        }
    }
    }

    return true;
}

inline int16_t read_int16(const byte *ptr)
{
    return __bswap_16(*(int16_t *)ptr);
}

inline int32_t read_int32(const byte *ptr)
{
    return __bswap_32(*(int32_t *)ptr);
}

inline int64_t read_int64(const byte *ptr)
{
    return __bswap_64(*(int64_t *)ptr);
}

inline double read_double(const byte *ptr)
{
    int64_t doubleAsInt = read_int64(ptr);
    // avoid strict-aliasing rules warning
    double *doublePtr = (double *)&doubleAsInt;
    return *doublePtr;
}

inline bool aquireDbMutex(uint blockMillis, const char *owner)
{
    ESP_LOGD(kLoggingTag, "Mutex: xSemaphoreTake for owner '%s'", owner);
    if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(blockMillis)) == pdFALSE)
    {
        ESP_LOGE(kLoggingTag, "Timeout aquiring database mutex for owner '%s'", owner);
        return false;
    }

    ESP_LOGD(kLoggingTag, "  Mutex: got it for owner '%s'", owner);
    return true;
}

inline void releaseDbMutex(const char *owner)
{
    ESP_LOGD(kLoggingTag, "Mutex: xSemaphoreGive from owner '%s'", owner);
    xSemaphoreGive(dbMutex);
}

int32_t read_fn_rctx(struct dblog_read_context *ctx, void *buf, uint32_t pos, size_t len)
{
    if (fseek(dbFile, pos, SEEK_SET))
        return DBLOG_RES_SEEK_ERR;
    size_t ret = fread(buf, 1, len, dbFile);
    if (ret != len)
        return DBLOG_RES_READ_ERR;
    return ret;
}

int32_t read_fn_wctx(struct dblog_write_context *ctx, void *buf, uint32_t pos, size_t len)
{
    if (fseek(dbFile, pos, SEEK_SET))
        return DBLOG_RES_SEEK_ERR;
    size_t ret = fread(buf, 1, len, dbFile);
    if (ret != len)
        return DBLOG_RES_READ_ERR;
    return ret;
}

int32_t write_fn(struct dblog_write_context *ctx, void *buf, uint32_t pos, size_t len)
{
    if (fseek(dbFile, pos, SEEK_SET))
        return DBLOG_RES_SEEK_ERR;
    size_t ret = fwrite(buf, 1, len, dbFile);
    if (ret != len)
        return DBLOG_RES_ERR;
    if (fflush(dbFile))
        return DBLOG_RES_FLUSH_ERR;
    fsync(fileno(dbFile));
    return ret;
}

int flush_fn(struct dblog_write_context *ctx)
{
    return DBLOG_RES_OK;
}
