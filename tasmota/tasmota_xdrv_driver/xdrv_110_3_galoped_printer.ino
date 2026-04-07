/*
  xdrv_110_3_galoped_printer.ino - Abstract 3D printer interface

  https://github.com/petrows/smarthome-galoped-dekad
  Copyright (C) 2026 by Petr Golovachev <petro@petro.ws>
  License: GPLv3+
*/

#ifdef USE_GALOPED

#include "IniFile.h"

// Maximum number of printer slots
#define GALOPED_PRINTER_MAX    2

// Web UI endpoint
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
 * Common printer status structure (shared by all printer types)
\*********************************************************************************************/

struct PrinterStatus {
  bool     connected;       // Connection to printer is active
  bool     data_valid;      // At least one successful data read
  float    nozzle_temp;     // Current nozzle temperature (C)
  float    nozzle_target;   // Target nozzle temperature (C)
  float    bed_temp;        // Current bed temperature (C)
  float    bed_target;      // Target bed temperature (C)
  uint8_t  progress;        // Print progress 0-100%
  uint16_t remaining_min;   // Estimated remaining time (minutes)
  uint8_t  state;           // PRINTER_STATE_*
  uint32_t last_update;     // TasmotaGlobal.uptime of last data
};

/*********************************************************************************************\
 * Abstract printer base class
 *
 * All methods prefixed with "prt" to avoid collision with Arduino
 * reserved names (begin, loop, etc.) which cause prototype generation
 * issues when .ino files are compiled.
\*********************************************************************************************/

class GalopedPrinter {
public:
  GalopedPrinter(uint8_t slot) : _slot(slot) {
    memset(&status, 0, sizeof(status));
    status.state = PRINTER_STATE_UNKNOWN;
  }

  virtual ~GalopedPrinter() {}

  // Lifecycle methods
  virtual void prtBegin() = 0;           // Called after settings loaded
  virtual void prtLoop() = 0;            // Called from FUNC_LOOP (fast)
  virtual void prtEverySecond() = 0;     // Called from FUNC_EVERY_SECOND
  virtual void prtDisconnect() = 0;      // Disconnect from printer

  // Settings (INI file persistence)
  virtual void prtLoadSettings(File &ini_file) = 0;  // Read type-specific keys
  virtual void prtSaveSettings(String &out) = 0;     // Append type-specific INI lines
  virtual uint8_t prtType() = 0;                     // PRINTER_TYPE_*
  virtual const char* prtTypeName() = 0;              // Display name

  // Web UI form
  virtual void prtWebFormFields() = 0;   // Render type-specific form inputs
  virtual void prtWebFormSave() = 0;     // Read form inputs on save

  // Current status
  PrinterStatus status;

  // State query helpers
  bool isRunning() {
    return status.data_valid && status.state == PRINTER_STATE_RUNNING;
  }

  bool isFinished() {
    return status.data_valid && status.state == PRINTER_STATE_FINISH;
  }

  bool isError() {
    return !status.data_valid ||
           status.state == PRINTER_STATE_PAUSE ||
           status.state == PRINTER_STATE_ERROR;
  }

  bool isIdle() {
    return status.data_valid && status.state == PRINTER_STATE_IDLE;
  }

  // State as human-readable string
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

  uint8_t prtSlot() { return _slot; }

protected:
  uint8_t _slot;  // Slot index (0..GALOPED_PRINTER_MAX-1)
};

// Global printer instances array
static GalopedPrinter* galoped_printers[GALOPED_PRINTER_MAX] = { nullptr };

/*********************************************************************************************\
 * Shared lightweight JSON helpers
 *
 * strstr-based field extraction for large JSON payloads.
 * No malloc, works directly on the buffer.
 * Used by both BambuLab (MQTT) and OctoPrint (HTTP) parsers.
\*********************************************************************************************/

// Find "key": <number> and return the float value
static bool PrtJsonGetFloat(const char *buf, const char *key, float *out) {
  const char *p = strstr(buf, key);
  if (!p) return false;

  p += strlen(key);

  // Skip '": ' separators after the key name
  while (*p && (*p == '"' || *p == ':' || *p == ' ')) {
    p++;
  }
  if (!*p) return false;

  *out = strtof(p, nullptr);
  return true;
}

// Find "key": <integer> and return the int value
static bool PrtJsonGetInt(const char *buf, const char *key, int *out) {
  const char *p = strstr(buf, key);
  if (!p) return false;

  p += strlen(key);

  while (*p && (*p == '"' || *p == ':' || *p == ' ')) {
    p++;
  }
  if (!*p) return false;

  *out = strtol(p, nullptr, 10);
  return true;
}

// Find "key": "string" and copy the string value to dst
static bool PrtJsonGetStr(const char *buf, const char *key, char *dst, int dst_size) {
  const char *p = strstr(buf, key);
  if (!p) return false;

  p += strlen(key);

  // Skip colon and spaces, but NOT the opening quote
  while (*p && (*p == ':' || *p == ' ')) {
    p++;
  }
  if (*p != '"') return false;
  p++;  // Skip opening quote

  const char *end = strchr(p, '"');
  if (!end) return false;

  int len = end - p;
  if (len >= dst_size) {
    len = dst_size - 1;
  }

  memcpy(dst, p, len);
  dst[len] = '\0';
  return true;
}

#endif  // USE_GALOPED
