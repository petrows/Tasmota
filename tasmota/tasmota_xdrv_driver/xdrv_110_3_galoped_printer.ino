/*
  xdrv_110_3_galoped_printer.ino - Abstract 3D printer interface

  https://github.com/petrows/smarthome-galoped-dekad
  Copyright (C) 2026 by Petr Golovachev <petro@petro.ws>
  License: GPLv3+
*/

#ifdef USE_GALOPED

#include "IniFile.h"

#define GALOPED_PRINTER_MAX    2
#define WEB_HANDLE_PRINTER_CFG "printcfg"

#define PRINTER_TYPE_NONE      0
#define PRINTER_TYPE_BAMBULAB  1
#define PRINTER_TYPE_OCTOPRINT 2

#define PRINTER_STATE_UNKNOWN  0
#define PRINTER_STATE_IDLE     1
#define PRINTER_STATE_RUNNING  2
#define PRINTER_STATE_PAUSE    3
#define PRINTER_STATE_FINISH   4
#define PRINTER_STATE_ERROR    5

struct PrinterStatus {
  bool     connected;
  bool     data_valid;
  float    nozzle_temp;
  float    nozzle_target;
  float    bed_temp;
  float    bed_target;
  uint8_t  progress;
  uint16_t remaining_min;
  uint8_t  state;
  uint32_t last_update;
};

class GalopedPrinter {
public:
  GalopedPrinter(uint8_t slot) : _slot(slot) {
    memset(&status, 0, sizeof(status));
    status.state = PRINTER_STATE_UNKNOWN;
  }
  virtual ~GalopedPrinter() {}

  virtual void prtBegin() = 0;
  virtual void prtLoop() = 0;
  virtual void prtEverySecond() = 0;
  virtual void prtDisconnect() = 0;

  virtual void prtLoadSettings(File &ini_file) = 0;
  virtual void prtSaveSettings(String &out) = 0;
  virtual uint8_t prtType() = 0;
  virtual const char* prtTypeName() = 0;

  virtual void prtWebFormFields() = 0;
  virtual void prtWebFormSave() = 0;

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
  uint8_t prtSlot() { return _slot; }
protected:
  uint8_t _slot;
};

static GalopedPrinter* galoped_printers[GALOPED_PRINTER_MAX] = { nullptr };

// Shared JSON helpers (strstr-based, no malloc)
static bool PrtJsonGetFloat(const char *buf, const char *key, float *out) {
  const char *p = strstr(buf, key);
  if (!p) return false;
  p += strlen(key);
  while (*p && (*p == '"' || *p == ':' || *p == ' ')) p++;
  if (!*p) return false;
  *out = strtof(p, nullptr);
  return true;
}
static bool PrtJsonGetInt(const char *buf, const char *key, int *out) {
  const char *p = strstr(buf, key);
  if (!p) return false;
  p += strlen(key);
  while (*p && (*p == '"' || *p == ':' || *p == ' ')) p++;
  if (!*p) return false;
  *out = strtol(p, nullptr, 10);
  return true;
}
static bool PrtJsonGetStr(const char *buf, const char *key, char *dst, int dst_size) {
  const char *p = strstr(buf, key);
  if (!p) return false;
  p += strlen(key);
  while (*p && (*p == ':' || *p == ' ')) p++;
  if (*p != '"') return false;
  p++;
  const char *end = strchr(p, '"');
  if (!end) return false;
  int len = end - p;
  if (len >= dst_size) len = dst_size - 1;
  memcpy(dst, p, len);
  dst[len] = '\0';
  return true;
}

#endif  // USE_GALOPED
