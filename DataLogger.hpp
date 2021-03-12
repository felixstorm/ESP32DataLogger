# pragma once

void queueTask(void *taskParameter);
void queueTaskFlush();
bool recoverDb();
void dataResponseHandler(AsyncWebServerRequest *request);
String rowToBuffer(struct dblog_read_context *ctx, time_t *timestamp);
bool addColumnToBuffer(struct dblog_read_context *ctx, int col_idx, String &buffer);
inline int16_t read_int16(const byte *ptr);
inline int32_t read_int32(const byte *ptr);
inline int64_t read_int64(const byte *ptr);
inline double read_double(const byte *ptr);
inline bool aquireDbMutex(uint blockMillis, const char *owner);
inline void releaseDbMutex(const char *owner);
int32_t read_fn_rctx(struct dblog_read_context *ctx, void *buf, uint32_t pos, size_t len);
int32_t read_fn_wctx(struct dblog_write_context *ctx, void *buf, uint32_t pos, size_t len);
int32_t write_fn(struct dblog_write_context *ctx, void *buf, uint32_t pos, size_t len);
int flush_fn(struct dblog_write_context *ctx);
