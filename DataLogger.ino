#include "Main.h"

#include <byteswap.h>

namespace
{
    const constexpr char *kLoggingTag = "Logger";

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
}

void setupDataLogger(int flushEverySeconds, int queueLength)
{
    ESP_LOGD(kLoggingTag, "Entering setupDataLogger()");

    recoverDb();

    flushEveryMillis = flushEverySeconds * 1000;
    recordQueueHandle = xQueueCreate(queueLength, sizeof(Record));

    auto createTaskResult = xTaskCreate(queueTask, "recordQueue", 8192, nullptr, 5, &queueTaskHandle);
    if (createTaskResult != pdPASS)
    {
        ESP_LOGE(kLoggingTag, "Error %d creating task", createTaskResult);
        while (true)
            ;
    }

    asyncWebServer.on("/data", HTTP_GET, respondWithData);
}

bool isDatabaseAccessible()
{
    return dbAccessible;
}

bool addRecord(const Record &record)
{
    ESP_LOGD(kLoggingTag, "Entering addRecord()");

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

    dbAccessible = true;
}

bool dbFileExists()
{
    ESP_LOGD(kLoggingTag, "Entering dbFileExists()");

    bool fileExists = SPIFFS.exists(dbFilenameWithoutFs);
    ESP_LOGI(kLoggingTag, "Database file exists: %d", fileExists);

    return fileExists;
}

void respondWithData(AsyncWebServerRequest *request)
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
        recordsFrom -= 60;
    }
    ESP_LOGI(kLoggingTag, "Responding with data: recordsFrom = %ld, recordsUntil = %ld", recordsFrom, recordsUntil);

    if (!aquireDbMutex(1000 * 10, __func__))
    {
        request->send(500);
        return;
    }

    memset(&dataReadDbContext, 0, sizeof(dataReadDbContext));
    dataReadDbContext.buf = dbBuffer;
    dataReadDbContext.read_fn = read_fn_rctx;

    dbFile = nullptr;
    dbFile = fopen(dbFilename, "rb");
    if (!dbFile)
    {
        ESP_LOGE(kLoggingTag, "Error opening database file '%s'", dbFilename);
        goto exit;
    }
    res = dblog_read_init(&dataReadDbContext);
    if (res)
    {
        ESP_LOGE(kLoggingTag, "dblog_read_init returned error %d", res);
        goto exit;
    }
    ESP_LOGI(kLoggingTag, "Page size: %d, last data page: %d", (int32_t)1 << dataReadDbContext.page_size_exp, dataReadDbContext.last_leaf_page);

    res = dblog_bin_srch_row_by_val(&dataReadDbContext, 0, DBLOG_TYPE_INT, &recordsFrom, sizeof(recordsFrom), 0);
    if (res)
    {
        ESP_LOGE(kLoggingTag, "dblog_bin_srch_row_by_val returned error %d", res);
        goto exit;
    }

    dataReadLastTimestamp = 0;
    dataReadFinalize = false;
    response = request->beginChunkedResponse("text/plain", [recordsUntil](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        ESP_LOGV(kLoggingTag, "ChunkedResponse: recordsUntil = %ld, dataReadFinalize = %d, dataReadLastTimestamp = %ld, buffer = %p, maxLen = %d, index = %d",
                 recordsUntil, dataReadFinalize, dataReadLastTimestamp, buffer, maxLen, index);
        uint8_t *workBuffer = buffer;
        size_t lengthRemaining = maxLen;

        if (dataReadFinalize)
            return 0;

        *workBuffer = index == 0 ? '[' : ',';
        workBuffer += 1;
        lengthRemaining -= 1;

        if (index != 0)
        {
            if ((recordsUntil && dataReadLastTimestamp >= recordsUntil) || dblog_read_next_row(&dataReadDbContext))
            {
                *(workBuffer - 1) = ']';
                dataReadFinalize = true;
            }
        }

        if (!dataReadFinalize)
        {
            auto rowArray = rowToJsonArrayDocument(&dataReadDbContext);
            int bytesWritten = serializeJson(rowArray, workBuffer, lengthRemaining);
            workBuffer += bytesWritten;
            lengthRemaining -= bytesWritten;

            if (recordsUntil)
            {
                uint32_t col_type;
                dataReadLastTimestamp = read_int32((const byte *)dblog_read_col_val(&dataReadDbContext, 0, &col_type));
            }
        }

        int bytesWritten = maxLen - lengthRemaining;
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

exit:
    if (!sentResponse)
    {
        request->send(500);
        if (dbFile)
            fclose(dbFile);
        releaseDbMutex("respondWithData !sentResponse");
    }
}

DynamicJsonDocument rowToJsonArrayDocument(struct dblog_read_context *ctx)
{
    DynamicJsonDocument jsonDoc(JSON_ARRAY_SIZE(3));
    JsonArray jsonArray = jsonDoc.to<JsonArray>();
    for (int i = 0; addColumnToJsonArray(ctx, i, jsonArray); i++)
        ;
    return jsonDoc;
}

bool addColumnToJsonArray(struct dblog_read_context *ctx, int col_idx, JsonArray jsonArray)
{
    uint32_t col_type;
    const byte *col_val = (const byte *)dblog_read_col_val(ctx, col_idx, &col_type);
    if (!col_val)
    {
        if (col_idx == 0)
            ESP_LOGE(kLoggingTag, "Error reading column value");
        return false;
    }

    switch (col_type)
    {
    case 0:
        jsonArray.add(nullptr);
        break;
    case 1:
        jsonArray.add(*((int8_t *)col_val));
        break;
    case 2:
        jsonArray.add(read_int16(col_val));
        break;
    case 4:
        jsonArray.add(read_int32(col_val));
        break;
#if ARDUINOJSON_USE_LONG_LONG
    case 6:
        jsonArray.add(read_int64(col_val));
        break;
#endif
    case 7:
        jsonArray.add(read_double(col_val));
        break;
    default:
    {
        if (col_type < 12)
        {
            ESP_LOGE(kLoggingTag, "Unuspported column type %d", col_type);
            return false;
        }

        uint32_t col_len = dblog_derive_data_len(col_type);
        String stringValue;
        stringValue.reserve(col_type % 2 ? col_len : col_len * 2);
        for (int j = 0; j < col_len; j++)
        {
            if (col_type % 2)
                stringValue += (char)col_val[j];
            else
            {
                stringValue += String(col_val[j], HEX);
            }
        }
        jsonArray.add(stringValue);
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
