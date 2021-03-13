# ESP32 Data Logger with Fast Web Chart GUI

## About
- Primarly based on [Sqlite µLogger for Arduino](https://github.com/siara-cc/sqlite_micro_logger_arduino) and [μPlot](https://github.com/leeoniya/uPlot) (both great projects, many thanks to the respective authors!)
- Currently uses the [TTGO T-Display ESP32](https://github.com/Xinyuan-LilyGO/TTGO-T-Display) with a built-in TFT display and an INA226 breakout board (connected to the standard I2C pins) to measure current and voltage, but should be pretty easy to adapt to different ESP32 boards, sensors and/or measurement types.
- Saves measurements to SPIFFS to an SQlite-like database and displays them in realtime in a lean and fast web GUI using a REST endpoint and server side events. Adapting the code to save measurements to SD card should also be possible.
- The web GUI shows the measurements of the last hour per default, but supports using the mouse wheel for zooming in and out of the chart and the middle mouse button for panning. Reloads data automatically as needed for zooming and panning.
- The TFT display shows measurements and some status and the buttons on the board can be used to start and stop logging, flush values to file (usually only done every 60 seconds) and to reset/clear the database.

## Status
- Far from perfect, but good enough to sample voltage and current of a lipo discharge and charge cycle - once per second for a 1-2 hours.
- It did work for me without any major issues. The only potentially bigger problem I noticed was that once after a reset during writing to a rather large DB, the recovery process did not seem to finish within a minute or so, but I was too impatient to wait any longer or to further debug the issue and just started off with a new database.
- Areas with potential for enhancements (from my personal perspective):
  - Further test, cleanup and fix the code that delivers the yet uncommitted samples to the client. Or include them in the REST response.
  - Cleanup web page and JavaScript code.
  - Better separate measurement code from logging and web GUI code to allow easier adoption to different measurement types (e.g. temperature, humidity and pressure).
  - Track and debug the potential database corruption issue mentioned above.
  - Protect downloading of the database with the mutex also.
  - Pack it as a library.

### I have no plans to document or develop this any further soon or to support it in any way, but if you're looking to build something similar then this might be a helpful starting point.
