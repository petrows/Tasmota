/*
  xdrv_110_3_galoped_printer.ino - 3D printer integration for Galoped

  https://github.com/petrows/smarthome-galoped-dekad

  Copyright (C) 2026 by Petr Golovachev <petro@petro.ws>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/*
  All-in-one printer module: abstract base class, BambuLab, OctoPrint,
  manager, INI storage, web UI.
  Single file because PlatformIO compiles each .ino in subdirectories
  as a separate translation unit.
*/

#ifdef USE_GALOPED

#include "IniFile.h"
#include <PubSubClient.h>

#define GALOPED_PRINTER_MAX    2
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

// ═══════════════════════════════════════════════════════════════════════════════
//  Common status + abstract base class
// ═══════════════════════════════════════════════════════════════════════════════

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

  virtual void begin() = 0;
  virtual void loop() = 0;
  virtual void everySecond() = 0;
  virtual void disconnect() = 0;

  virtual void loadSettings(File &ini_file) = 0;
  virtual void saveSettings(String &out) = 0;
  virtual uint8_t type() = 0;
  virtual const char* typeName() = 0;

  virtual void webFormFields() = 0;
  virtual void webFormSave() = 0;

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

static GalopedPrinter* galoped_printers[GALOPED_PRINTER_MAX] = { nullptr };

// ═══════════════════════════════════════════════════════════════════════════════
//  Shared lightweight JSON helpers (strstr-based, no malloc)
// ═══════════════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════════════
//  BambuLab implementation
// ═══════════════════════════════════════════════════════════════════════════════

#define BBL_MQTT_PORT        8883
#define BBL_MQTT_KEEPALIVE   30
#define BBL_MQTT_BUF_SIZE    (32*1024)
#define BBL_RECONNECT_SEC    30
#define BBL_PUSHALL_SEC      60
#define BBL_CLOUD_HOST_US    "us.mqtt.bambulab.com"
#define BBL_CLOUD_HOST_CN    "cn.mqtt.bambulab.com"
#define BBL_MODE_LOCAL  0
#define BBL_MODE_CLOUD  1
#define BBL_REGION_US   0
#define BBL_REGION_EU   1
#define BBL_REGION_CN   2

// JWT UID extraction
static int BblBase64UrlDecode(const char *src, int src_len, char *dst, int dst_max) {
  char *tmp = (char*)malloc(src_len + 4);
  if (!tmp) return -1;
  for (int i = 0; i < src_len; i++) {
    if (src[i] == '-') tmp[i] = '+'; else if (src[i] == '_') tmp[i] = '/'; else tmp[i] = src[i];
  }
  int pad = (4 - (src_len % 4)) % 4;
  for (int i = 0; i < pad; i++) tmp[src_len + i] = '=';
  tmp[src_len + pad] = '\0';
  int decoded_len = decode_base64((unsigned char*)tmp, (unsigned char*)dst);
  free(tmp);
  if (decoded_len >= dst_max) decoded_len = dst_max - 1;
  dst[decoded_len] = '\0';
  return decoded_len;
}

static bool BblExtractUidFromToken(const char *token, char *buf, int buf_size) {
  const char *d1 = strchr(token, '.'); if (!d1) return false;
  const char *ps = d1 + 1;
  const char *d2 = strchr(ps, '.'); if (!d2) return false;
  int plen = d2 - ps; if (plen > 1024) return false;
  char *dec = (char*)malloc(plen + 4); if (!dec) return false;
  if (BblBase64UrlDecode(ps, plen, dec, plen + 4) <= 0) { free(dec); return false; }
  JsonParser parser(dec);
  JsonParserObject root = parser.getRootObject();
  bool found = false;
  if (root) {
    static const char *keys[] = { "uid", "sub", "user_id" };
    for (uint32_t i = 0; i < 3 && !found; i++) {
      JsonParserToken t = root[keys[i]];
      if (t) {
        const char *s = t.getStr();
        if (s && strlen(s)) { snprintf(buf, buf_size, "u_%s", s); found = true; }
        else if (t.isInt()) { snprintf(buf, buf_size, "u_%d", t.getInt()); found = true; }
      }
    }
  }
  free(dec);
  return found;
}

class GalopedPrinterBBL : public GalopedPrinter {
public:
  GalopedPrinterBBL(uint8_t slot) : GalopedPrinter(slot),
    _mode(BBL_MODE_LOCAL), _region(BBL_REGION_EU),
    _tls(nullptr), _mqtt(nullptr), _last_reconnect(0), _last_pushall(0)
  {
    memset(_host, 0, sizeof(_host));
    memset(_serial, 0, sizeof(_serial));
    memset(_access_code, 0, sizeof(_access_code));
    memset(_cloud_token, 0, sizeof(_cloud_token));
  }
  ~GalopedPrinterBBL() override { disconnect(); delete _mqtt; _mqtt=nullptr; delete _tls; _tls=nullptr; }

  uint8_t type() override { return PRINTER_TYPE_BAMBULAB; }
  const char* typeName() override { return "BBL"; }

  void loadSettings(File &ini_file) override {
    IniFile ini(ini_file);
    int32_t v;
    if (ini.getValueInt("printer", "mode", v)) _mode = v;
    if (ini.getValueInt("printer", "cloud_region", v)) _region = v;
    ini.getValueStr("printer", "host", _host, sizeof(_host));
    ini.getValueStr("printer", "serial", _serial, sizeof(_serial));
    ini.getValueStr("printer", "access_code", _access_code, sizeof(_access_code));
    ini.getValueStr("printer", "cloud_token", _cloud_token, sizeof(_cloud_token));
    AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: host=%s serial=%s mode=%d"), _slot, _host, _serial, _mode);
  }
  void saveSettings(String &out) override {
    out += "mode="; out += _mode; out += "\n";
    out += "host="; out += _host; out += "\n";
    out += "serial="; out += _serial; out += "\n";
    out += "access_code="; out += _access_code; out += "\n";
    out += "cloud_region="; out += _region; out += "\n";
    out += "cloud_token="; out += _cloud_token; out += "\n";
  }
  void begin() override {}
  void disconnect() override {
    if (_mqtt) _mqtt->disconnect();
    if (_tls) _tls->stop();
    status.connected = false;
  }
  void loop() override {
    if (_mqtt && status.connected && !_mqtt->loop()) {
      AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: Connection lost"), _slot);
      status.connected = false; status.data_valid = false;
      if (_tls) _tls->stop();
    }
  }
  void everySecond() override {
    if (!isConfigured()) return;
    if (_mqtt && status.connected) {
      if (TasmotaGlobal.uptime - _last_pushall >= BBL_PUSHALL_SEC) sendPushAll();
    } else if (TasmotaGlobal.uptime - _last_reconnect >= BBL_RECONNECT_SEC) {
      doConnect();
    }
  }
  void webFormFields() override {
    WSContentSend_P(PSTR("<p><b>Serial</b><br><input name='bs' maxlength='19' value='%s'></p>"), _serial);
    WSContentSend_P(PSTR("<p><b>Host / IP</b><br><input name='bh' maxlength='39' value='%s'></p>"), _host);
    WSContentSend_P(PSTR("<p><b>LAN Access Code</b><br><input name='ba' type='password' maxlength='19' value='%s'></p>"), _access_code);
  }
  void webFormSave() override {
    char tmp[256];
    WebGetArg(PSTR("bs"), tmp, sizeof(tmp)); strlcpy(_serial, tmp, sizeof(_serial));
    WebGetArg(PSTR("bh"), tmp, sizeof(tmp)); strlcpy(_host, tmp, sizeof(_host));
    WebGetArg(PSTR("ba"), tmp, sizeof(tmp)); strlcpy(_access_code, tmp, sizeof(_access_code));
    _mode = BBL_MODE_LOCAL;
  }

private:
  uint8_t _mode, _region;
  char _host[40], _serial[20], _access_code[20], _cloud_token[256];
  BearSSL::WiFiClientSecure_light *_tls;
  PubSubClient *_mqtt;
  uint32_t _last_reconnect, _last_pushall;

  bool isConfigured() {
    return (_mode == BBL_MODE_LOCAL)
      ? (strlen(_host) && strlen(_serial) && strlen(_access_code))
      : (strlen(_serial) && strlen(_cloud_token));
  }

  static void mqttCbStatic(char *topic, uint8_t *payload, unsigned int length) {
    for (uint8_t i = 0; i < GALOPED_PRINTER_MAX; i++) {
      GalopedPrinter *p = galoped_printers[i];
      if (p && p->type() == PRINTER_TYPE_BAMBULAB) {
        GalopedPrinterBBL *b = static_cast<GalopedPrinterBBL*>(p);
        if (strstr(topic, b->_serial)) { b->onMessage(payload, length); return; }
      }
    }
  }

  void onMessage(uint8_t *payload, unsigned int length) {
    if (length < 10) return;
    char saved = ((char*)payload)[length];
    ((char*)payload)[length] = '\0';
    const char *j = (const char*)payload;
    if (!strstr(j, "\"print\"")) { ((char*)payload)[length] = saved; return; }

    float f; int i; bool upd = false;
    if (PrtJsonGetFloat(j, "\"nozzle_temper\"", &f))        { status.nozzle_temp = f; upd = true; }
    if (PrtJsonGetFloat(j, "\"nozzle_target_temper\"", &f)) { status.nozzle_target = f; upd = true; }
    if (PrtJsonGetFloat(j, "\"bed_temper\"", &f))           { status.bed_temp = f; upd = true; }
    if (PrtJsonGetFloat(j, "\"bed_target_temper\"", &f))    { status.bed_target = f; upd = true; }
    if (PrtJsonGetInt(j, "\"mc_percent\"", &i))             { status.progress = i; upd = true; }
    if (PrtJsonGetInt(j, "\"mc_remaining_time\"", &i))      { status.remaining_min = i; upd = true; }

    char ss[16] = "";
    if (PrtJsonGetStr(j, "\"gcode_state\"", ss, sizeof(ss))) {
      if      (!strcmp(ss, "IDLE"))    status.state = PRINTER_STATE_IDLE;
      else if (!strcmp(ss, "RUNNING")) status.state = PRINTER_STATE_RUNNING;
      else if (!strcmp(ss, "PAUSE"))   status.state = PRINTER_STATE_PAUSE;
      else if (!strcmp(ss, "FINISH"))  status.state = PRINTER_STATE_FINISH;
      else                             status.state = PRINTER_STATE_UNKNOWN;
      upd = true;
    }
    ((char*)payload)[length] = saved;
    if (upd) {
      status.data_valid = true; status.last_update = TasmotaGlobal.uptime;
      AddLog(LOG_LEVEL_DEBUG, PSTR("BBL[%d]: n=%.0f/%.0f b=%.0f/%.0f %d%% %s"),
        _slot, status.nozzle_temp, status.nozzle_target,
        status.bed_temp, status.bed_target, status.progress, stateStr());
    }
  }

  void doConnect() {
    if (!WifiHasIP()) return;
    _last_reconnect = TasmotaGlobal.uptime;
    if (_tls) _tls->stop();
    if (!_tls) { _tls = new BearSSL::WiFiClientSecure_light(4096, 4096); if (!_tls) return; }
    _tls->setInsecure();
    if (!_mqtt) {
      _mqtt = new PubSubClient();
      _mqtt->setClient(*_tls); _mqtt->setBufferSize(BBL_MQTT_BUF_SIZE);
      _mqtt->setKeepAlive(BBL_MQTT_KEEPALIVE); _mqtt->setCallback(mqttCbStatic);
    }
    const char *host, *user, *pass; char cid[64]; static char uid[48];
    if (_mode == BBL_MODE_LOCAL) { host=_host; user="bblp"; pass=_access_code; }
    else {
      host = (_region==BBL_REGION_CN) ? BBL_CLOUD_HOST_CN : BBL_CLOUD_HOST_US;
      if (!BblExtractUidFromToken(_cloud_token, uid, sizeof(uid))) {
        AddLog(LOG_LEVEL_ERROR, PSTR("BBL[%d]: JWT fail"), _slot); return;
      }
      user=uid; pass=_cloud_token;
    }
    snprintf(cid, sizeof(cid), "bblp_%08X_%d", ESP_getChipId(), _slot);
    AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: -> %s:%d user=%s"), _slot, host, BBL_MQTT_PORT, user);
    IPAddress ip;
    if (!WifiHostByName(host, ip)) { AddLog(LOG_LEVEL_ERROR, PSTR("BBL[%d]: DNS fail"), _slot); return; }
    _mqtt->setServer(ip, BBL_MQTT_PORT);
    if (_mqtt->connect(cid, user, pass)) {
      status.connected = true;
      char t[64]; snprintf(t, sizeof(t), "device/%s/report", _serial);
      _mqtt->subscribe(t);
      AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: OK, sub %s"), _slot, t);
      sendPushAll();
    } else {
      AddLog(LOG_LEVEL_ERROR, PSTR("BBL[%d]: fail rc=%d"), _slot, _mqtt->state());
      status.connected = false; _tls->stop();
    }
  }

  void sendPushAll() {
    if (!_mqtt || !status.connected) return;
    char t[64]; snprintf(t, sizeof(t), "device/%s/request", _serial);
    _mqtt->publish(t, "{\"pushing\":{\"command\":\"pushall\"}}");
    _last_pushall = TasmotaGlobal.uptime;
  }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  OctoPrint implementation (raw WiFiClient, no HTTPClientLight)
// ═══════════════════════════════════════════════════════════════════════════════

#define OCTO_POLL_SEC       5
#define OCTO_RECONNECT_SEC  30
#define OCTO_TIMEOUT_MS     5000
#define OCTO_DEFAULT_PORT   80
#define OCTO_BUF_SIZE       1024

class GalopedPrinterOctoprint : public GalopedPrinter {
public:
  GalopedPrinterOctoprint(uint8_t slot) : GalopedPrinter(slot),
    _port(OCTO_DEFAULT_PORT), _last_poll(0), _fail_count(0)
  { memset(_host,0,sizeof(_host)); memset(_api_key,0,sizeof(_api_key)); }
  ~GalopedPrinterOctoprint() override { disconnect(); }

  uint8_t type() override { return PRINTER_TYPE_OCTOPRINT; }
  const char* typeName() override { return "Octo"; }

  void loadSettings(File &ini_file) override {
    IniFile ini(ini_file);
    ini.getValueStr("printer", "host", _host, sizeof(_host));
    ini.getValueStr("printer", "api_key", _api_key, sizeof(_api_key));
    int32_t p = OCTO_DEFAULT_PORT; ini.getValueInt("printer", "port", p); _port = p;
    AddLog(LOG_LEVEL_INFO, PSTR("OCTO[%d]: host=%s:%d"), _slot, _host, _port);
  }
  void saveSettings(String &out) override {
    out += "host="; out += _host; out += "\n";
    out += "port="; out += _port; out += "\n";
    out += "api_key="; out += _api_key; out += "\n";
  }
  void begin() override {}
  void disconnect() override { status.connected = false; status.data_valid = false; }
  void loop() override {}
  void everySecond() override {
    if (!isConfigured() || !WifiHasIP()) return;
    uint32_t iv = (_fail_count > 3) ? OCTO_RECONNECT_SEC : OCTO_POLL_SEC;
    if (TasmotaGlobal.uptime - _last_poll < iv) return;
    _last_poll = TasmotaGlobal.uptime;
    pollPrinter(); pollJob();
  }
  void webFormFields() override {
    WSContentSend_P(PSTR("<p><b>Host / IP</b><br><input name='oh' maxlength='39' value='%s'></p>"), _host);
    WSContentSend_P(PSTR("<p><b>Port</b><br><input name='op' type='number' value='%d'></p>"), _port);
    WSContentSend_P(PSTR("<p><b>API Key</b><br><input name='ok' type='password' maxlength='63' value='%s'></p>"), _api_key);
  }
  void webFormSave() override {
    char tmp[128];
    WebGetArg(PSTR("oh"), tmp, sizeof(tmp)); strlcpy(_host, tmp, sizeof(_host));
    WebGetArg(PSTR("op"), tmp, sizeof(tmp)); _port = atoi(tmp); if (!_port) _port = OCTO_DEFAULT_PORT;
    WebGetArg(PSTR("ok"), tmp, sizeof(tmp)); strlcpy(_api_key, tmp, sizeof(_api_key));
  }

private:
  char _host[40], _api_key[64];
  uint16_t _port;
  uint32_t _last_poll;
  uint8_t _fail_count;

  bool isConfigured() { return strlen(_host) && strlen(_api_key); }

  String httpGet(const char *path) {
    WiFiClient client; String body;
    if (!client.connect(_host, _port)) {
      if (++_fail_count > 3) { status.connected = false; status.data_valid = false; }
      return body;
    }
    client.printf("GET %s HTTP/1.1\r\nHost: %s:%d\r\nX-Api-Key: %s\r\nConnection: close\r\n\r\n",
      path, _host, _port, _api_key);
    uint32_t t0 = millis();
    while (!client.available() && millis() - t0 < OCTO_TIMEOUT_MS) delay(10);
    if (!client.available()) { client.stop(); _fail_count++; return body; }
    bool hdr = true;
    while (client.available()) {
      String line = client.readStringUntil('\n');
      if (hdr) {
        if (line.length() <= 2) { hdr = false; continue; }
        if (line.startsWith("HTTP/") && !line.substring(9, 12).equals("200")) {
          client.stop(); _fail_count++; return body;
        }
        continue;
      }
      body += line;
      if (body.length() > OCTO_BUF_SIZE) break;
    }
    client.stop(); status.connected = true; _fail_count = 0;
    return body;
  }

  void pollPrinter() {
    String body = httpGet("/api/printer"); if (!body.length()) return;
    char *b = (char*)body.c_str(); float f;
    const char *t0 = strstr(b, "\"tool0\"");
    if (t0) { PrtJsonGetFloat(t0,"\"actual\"",&f); status.nozzle_temp=f; PrtJsonGetFloat(t0,"\"target\"",&f); status.nozzle_target=f; }
    const char *bd = strstr(b, "\"bed\"");
    if (bd) { PrtJsonGetFloat(bd,"\"actual\"",&f); status.bed_temp=f; PrtJsonGetFloat(bd,"\"target\"",&f); status.bed_target=f; }
    status.data_valid = true; status.last_update = TasmotaGlobal.uptime;
  }

  void pollJob() {
    String body = httpGet("/api/job"); if (!body.length()) return;
    char *b = (char*)body.c_str(); float f; int i;
    if (PrtJsonGetFloat(b, "\"completion\"", &f)) status.progress = (uint8_t)f;
    if (PrtJsonGetInt(b, "\"printTimeLeft\"", &i)) status.remaining_min = (i>0) ? (i/60) : 0;
    char ss[20]="";
    if (PrtJsonGetStr(b, "\"state\"", ss, sizeof(ss))) {
      if (!strcmp(ss,"Printing")) status.state = PRINTER_STATE_RUNNING;
      else if (!strcmp(ss,"Operational")) status.state = PRINTER_STATE_IDLE;
      else if (!strcmp(ss,"Paused")||!strcmp(ss,"Pausing")) status.state = PRINTER_STATE_PAUSE;
      else if (!strcmp(ss,"Finishing")) status.state = PRINTER_STATE_FINISH;
      else if (strstr(ss,"Error")) status.state = PRINTER_STATE_ERROR;
      else status.state = PRINTER_STATE_UNKNOWN;
    }
    AddLog(LOG_LEVEL_DEBUG, PSTR("OCTO[%d]: n=%.0f/%.0f b=%.0f/%.0f %d%% %s"),
      _slot, status.nozzle_temp, status.nozzle_target,
      status.bed_temp, status.bed_target, status.progress, stateStr());
  }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  Factory
// ═══════════════════════════════════════════════════════════════════════════════

static GalopedPrinter* PrinterCreate(uint8_t ptype, uint8_t slot) {
  switch (ptype) {
    case PRINTER_TYPE_BAMBULAB:  return new GalopedPrinterBBL(slot);
    case PRINTER_TYPE_OCTOPRINT: return new GalopedPrinterOctoprint(slot);
    default: return nullptr;
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  INI load/save
// ═══════════════════════════════════════════════════════════════════════════════

static void PrinterLoadSlot(uint8_t slot) {
  if (galoped_printers[slot]) { galoped_printers[slot]->disconnect(); delete galoped_printers[slot]; galoped_printers[slot]=nullptr; }
#ifdef USE_UFILESYS
  char fn[20]; snprintf(fn, sizeof(fn), "/printer_%d.ini", slot);
  File f = ffsp->open(fn, "r");
  if (!f) { AddLog(LOG_LEVEL_DEBUG, PSTR("PRT: Slot %d: no cfg"), slot); return; }
  IniFile ini(f);
  char ts[16]=""; ini.getValueStr("printer", "type", ts, sizeof(ts));
  uint8_t pt = PRINTER_TYPE_NONE;
  if (!strcmp(ts,"bambulab")) pt = PRINTER_TYPE_BAMBULAB;
  else if (!strcmp(ts,"octoprint")) pt = PRINTER_TYPE_OCTOPRINT;
  if (pt == PRINTER_TYPE_NONE) { f.close(); return; }
  GalopedPrinter *p = PrinterCreate(pt, slot);
  if (!p) { f.close(); return; }
  f.seek(0);
  p->loadSettings(f);
  f.close();
  galoped_printers[slot] = p;
  p->begin();
  AddLog(LOG_LEVEL_INFO, PSTR("PRT: Slot %d: %s"), slot, p->typeName());
#endif
}

static void PrinterSaveSlot(uint8_t slot) {
#ifdef USE_UFILESYS
  char fn[20]; snprintf(fn, sizeof(fn), "/printer_%d.ini", slot);
  GalopedPrinter *p = galoped_printers[slot];
  if (!p) { ffsp->remove(fn); return; }
  String ini; ini += "[printer]\n";
  if (p->type()==PRINTER_TYPE_BAMBULAB) ini+="type=bambulab\n";
  else if (p->type()==PRINTER_TYPE_OCTOPRINT) ini+="type=octoprint\n";
  p->saveSettings(ini);
  File f = ffsp->open(fn, "w");
  if (f) { f.print(ini); f.close(); AddLog(LOG_LEVEL_INFO, PSTR("PRT: Slot %d saved"), slot); }
#endif
}

// ═══════════════════════════════════════════════════════════════════════════════
//  Lifecycle (called from Xdrv110)
// ═══════════════════════════════════════════════════════════════════════════════

void PrinterInit(void)        { for (uint8_t i=0; i<GALOPED_PRINTER_MAX; i++) PrinterLoadSlot(i); }
void PrinterLoop(void)        { for (uint8_t i=0; i<GALOPED_PRINTER_MAX; i++) if (galoped_printers[i]) galoped_printers[i]->loop(); }
void PrinterEverySecond(void) { for (uint8_t i=0; i<GALOPED_PRINTER_MAX; i++) if (galoped_printers[i]) galoped_printers[i]->everySecond(); }

GalopedPrinter* PrinterGet(uint8_t slot) {
  return (slot < GALOPED_PRINTER_MAX) ? galoped_printers[slot] : nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  Web UI
// ═══════════════════════════════════════════════════════════════════════════════

#ifdef USE_WEBSERVER

void PrinterConfigPage(void) {
  if (!HttpCheckPriviledgedAccess()) return;
  char tmp[256]; uint8_t slot=0;
  WebGetArg(PSTR("slot"), tmp, sizeof(tmp));
  if (strlen(tmp)) slot = atoi(tmp);
  if (slot >= GALOPED_PRINTER_MAX) slot = 0;

  if (Webserver->hasArg(F("save"))) {
    WebGetArg(PSTR("pt"), tmp, sizeof(tmp));
    uint8_t nt = atoi(tmp);
    if (galoped_printers[slot]) { galoped_printers[slot]->disconnect(); delete galoped_printers[slot]; galoped_printers[slot]=nullptr; }
    if (nt != PRINTER_TYPE_NONE) {
      GalopedPrinter *p = PrinterCreate(nt, slot);
      if (p) { p->webFormSave(); galoped_printers[slot]=p; PrinterSaveSlot(slot); p->begin(); }
    } else { PrinterSaveSlot(slot); }
    HandleConfiguration(); return;
  }

  WSContentStart_P(PSTR("3D Printer Configuration"));
  WSContentSendStyle();

  // Slot tabs
  WSContentSend_P(PSTR("<div style='text-align:center;margin-bottom:10px'>"));
  for (uint8_t i=0; i<GALOPED_PRINTER_MAX; i++) {
    const char *st = (i==slot) ? "background:#1fa3ec;color:white;font-weight:bold" : "background:#eee;color:#333;border:1px solid #ccc";
    const char *lb = galoped_printers[i] ? galoped_printers[i]->typeName() : "Empty";
    WSContentSend_P(PSTR("<a href='" WEB_HANDLE_PRINTER_CFG "?slot=%d' style='%s;padding:5px 15px;margin:2px;text-decoration:none;display:inline-block'>P%d: %s</a>"), i, st, i+1, lb);
  }
  WSContentSend_P(PSTR("</div>"));

  GalopedPrinter *p = galoped_printers[slot];
  uint8_t ct = p ? p->type() : PRINTER_TYPE_NONE;

  WSContentSend_P(PSTR("<script>function ptC(){var t=document.getElementById('pt').value;"
    "var d=document.querySelectorAll('.ptype');for(var i=0;i<d.length;i++) d[i].style.display='none';"
    "var e=document.getElementById('pt_'+t);if(e) e.style.display='';}</script>"));

  WSContentSend_P(PSTR("<fieldset><legend><b>&nbsp;Printer %d&nbsp;</b></legend>"), slot+1);
  WSContentSend_P(PSTR("<form method='get' action='" WEB_HANDLE_PRINTER_CFG "'><input type='hidden' name='slot' value='%d'>"), slot);

  WSContentSend_P(PSTR("<p><b>Type</b><br><select id='pt' name='pt' onchange='ptC()'>"
    "<option value='%d'%s>-- None --</option>"
    "<option value='%d'%s>BambuLab</option>"
    "<option value='%d'%s>OctoPrint</option></select></p>"),
    PRINTER_TYPE_NONE, ct==PRINTER_TYPE_NONE?" selected":"",
    PRINTER_TYPE_BAMBULAB, ct==PRINTER_TYPE_BAMBULAB?" selected":"",
    PRINTER_TYPE_OCTOPRINT, ct==PRINTER_TYPE_OCTOPRINT?" selected":"");

  // BBL fields
  WSContentSend_P(PSTR("<div id='pt_%d' class='ptype'%s>"), PRINTER_TYPE_BAMBULAB, ct==PRINTER_TYPE_BAMBULAB?"":" style='display:none'");
  if (p && ct==PRINTER_TYPE_BAMBULAB) p->webFormFields();
  else { GalopedPrinterBBL d(slot); d.webFormFields(); }
  WSContentSend_P(PSTR("</div>"));

  // Octo fields
  WSContentSend_P(PSTR("<div id='pt_%d' class='ptype'%s>"), PRINTER_TYPE_OCTOPRINT, ct==PRINTER_TYPE_OCTOPRINT?"":" style='display:none'");
  if (p && ct==PRINTER_TYPE_OCTOPRINT) p->webFormFields();
  else { GalopedPrinterOctoprint d(slot); d.webFormFields(); }
  WSContentSend_P(PSTR("</div>"));

  if (p) WSContentSend_P(PSTR("<hr><p>Status: <b style='color:%s'>%s</b></p>"),
    p->status.connected?"green":"red", p->status.connected?"Connected":"Disconnected");

  WSContentSend_P(PSTR("<br><button name='save' type='submit' class='button bgrn'>" D_SAVE "</button></form></fieldset>"));
  WSContentSpaceButton(BUTTON_CONFIGURATION);
  WSContentStop();
}

bool PrinterStatusWeb(void) {
  bool r = false;
  for (uint8_t i=0; i<GALOPED_PRINTER_MAX; i++) {
    GalopedPrinter *p = galoped_printers[i]; if (!p || !p->status.data_valid) continue; r=true;
    const char *n = p->typeName();
    const char *tc = p->status.nozzle_temp>200?"#F44":p->status.nozzle_temp>100?"#FA0":"#4F4";
    WSContentSend_P(PSTR("{s}%s Nozzle{m}<span style='color:%s'>%.0f</span>/%.0f°C{e}"), n, tc, p->status.nozzle_temp, p->status.nozzle_target);
    WSContentSend_P(PSTR("{s}%s Bed{m}%.0f/%.0f°C{e}"), n, p->status.bed_temp, p->status.bed_target);
    if (p->status.state!=PRINTER_STATE_IDLE && p->status.state!=PRINTER_STATE_UNKNOWN) {
      WSContentSend_P(PSTR("{s}%s Progress{m}%d%%{e}"), n, p->status.progress);
      if (p->status.remaining_min > 0) {
        uint16_t h=p->status.remaining_min/60, m=p->status.remaining_min%60;
        if (h) WSContentSend_P(PSTR("{s}%s ETA{m}%dh%dm{e}"), n, h, m);
        else   WSContentSend_P(PSTR("{s}%s ETA{m}%dm{e}"), n, m);
      }
    }
    WSContentSend_P(PSTR("{s}%s Status{m}%s{e}"), n, p->stateStr());
  }
  return r;
}

void PrinterShowJson(void) {
  for (uint8_t i=0; i<GALOPED_PRINTER_MAX; i++) {
    GalopedPrinter *p = galoped_printers[i]; if (!p || !p->status.data_valid) continue;
    ResponseAppend_P(PSTR(",\"%s%d\":{\"Nozzle\":%.1f,\"NozzleTarget\":%.1f,"
      "\"Bed\":%.1f,\"BedTarget\":%.1f,\"Progress\":%d,\"Remaining\":%d,\"State\":\"%s\"}"),
      p->typeName(), i, p->status.nozzle_temp, p->status.nozzle_target,
      p->status.bed_temp, p->status.bed_target, p->status.progress, p->status.remaining_min, p->stateStr());
  }
}

#endif  // USE_WEBSERVER

// ═══════════════════════════════════════════════════════════════════════════════
//  Compatibility wrappers for galoped_conf.ino (uses printer slot 0)
// ═══════════════════════════════════════════════════════════════════════════════

bool BblStatusIsValid()    { GalopedPrinter *p=PrinterGet(0); return p && p->status.data_valid; }
bool BblStatusIsRunning()  { GalopedPrinter *p=PrinterGet(0); return p && p->isRunning(); }
bool BblStatusIsFinished() { GalopedPrinter *p=PrinterGet(0); return p && p->isFinished(); }
bool BblStatusIsError()    { GalopedPrinter *p=PrinterGet(0); return !p || p->isError(); }
float BblGetNozzleTemp()   { GalopedPrinter *p=PrinterGet(0); return p ? p->status.nozzle_temp : 0; }
uint8_t BblGetProgress()   { GalopedPrinter *p=PrinterGet(0); return p ? p->status.progress : 0; }
uint8_t BblGetGCodeStatus(){ GalopedPrinter *p=PrinterGet(0); return p ? p->status.state : PRINTER_STATE_UNKNOWN; }
bool BblStatusWeb(void)    { return PrinterStatusWeb(); }

#endif  // USE_GALOPED
