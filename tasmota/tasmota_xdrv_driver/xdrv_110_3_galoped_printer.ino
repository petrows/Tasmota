/*
  xdrv_110_3_galoped_printer.ino - Abstract 3D printer interface for Galoped

  https://github.com/petrows/smarthome-galoped-dekad

  Copyright (C) 2026 by Petr Golovachev <petro@petro.ws>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/*
  Abstract printer interface with INI-file based settings.
  Supports multiple printer instances (up to GALOPED_PRINTER_MAX).
  Each printer stored as /printer_N.ini on UFS.

  Compilation order: _3 (base) -> _4 (BBL) -> _5 (Octo) -> _6 (manager+UI)
*/

#ifdef USE_GALOPED

#include "IniFile.h"

#define GALOPED_PRINTER_MAX      2

#define WEB_HANDLE_PRINTER_CFG "printcfg"

// Printer types
#define PRINTER_TYPE_NONE      0
#define PRINTER_TYPE_BAMBULAB  1
#define PRINTER_TYPE_OCTOPRINT 2

// Printer states
#define PRINTER_STATE_UNKNOWN  0
#define PRINTER_STATE_IDLE     1
#define PRINTER_STATE_RUNNING  2
#define PRINTER_STATE_PAUSE    3
#define PRINTER_STATE_FINISH   4
#define PRINTER_STATE_ERROR    5

/*********************************************************************************************\
 * Common printer status (same for all types)
\*********************************************************************************************/

struct PrinterStatus {
  bool     connected;
  bool     data_valid;
  float    nozzle_temp;
  float    nozzle_target;
  float    bed_temp;
  float    bed_target;
  uint8_t  progress;        // 0-100%
  uint16_t remaining_min;
  uint8_t  state;           // PRINTER_STATE_*
  uint32_t last_update;     // TasmotaGlobal.uptime
};

/*********************************************************************************************\
 * Abstract printer base class
\*********************************************************************************************/

class GalopedPrinter {
public:
  GalopedPrinter(uint8_t slot) : _slot(slot) {
    memset(&status, 0, sizeof(status));
    status.state = PRINTER_STATE_UNKNOWN;
  }
  virtual ~GalopedPrinter() {}

  // Lifecycle
  virtual void begin() = 0;
  virtual void loop() = 0;
  virtual void everySecond() = 0;
  virtual void disconnect() = 0;

  // Settings (INI)
  virtual void loadSettings(File &ini_file) = 0;
  virtual void saveSettings(String &out) = 0;
  virtual uint8_t type() = 0;
  virtual const char* typeName() = 0;

  // Web UI
  virtual void webFormFields() = 0;
  virtual void webFormSave() = 0;

  // Status
  PrinterStatus status;

  bool isRunning()  { return status.data_valid && status.state == PRINTER_STATE_RUNNING; }
  bool isFinished() { return status.data_valid && status.state == PRINTER_STATE_FINISH; }
  bool isError()    { return !status.data_valid || status.state == PRINTER_STATE_PAUSE || status.state == PRINTER_STATE_ERROR; }
  bool isIdle()     { return status.data_valid && status.state == PRINTER_STATE_IDLE; }

  const char* stateStr() {
    switch (status.state) {
      case PRINTER_STATE_IDLE:    return "IDLE";
      case PRINTER_STATE_RUNNING: return "RUNNING";
      case PRINTER_STATE_PAUSE:   return "PAUSE";
      case PRINTER_STATE_FINISH:  return "FINISH";
      case PRINTER_STATE_ERROR:   return "ERROR";
      default:                    return "UNKNOWN";
    }
  }

  uint8_t slot() { return _slot; }

protected:
  uint8_t _slot;
};

// Global printer array — used by subclass callbacks (e.g. BBL MQTT static callback)
static GalopedPrinter* galoped_printers[GALOPED_PRINTER_MAX] = { nullptr };

#endif  // USE_GALOPED
