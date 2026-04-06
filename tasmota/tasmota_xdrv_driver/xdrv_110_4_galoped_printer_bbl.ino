/*
  xdrv_110_4_galoped_printer_bbl.ino - BambuLab printer implementation

  https://github.com/petrows/smarthome-galoped-dekad

  Copyright (C) 2026 by Petr Golovachev <petro@petro.ws>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#ifdef USE_GALOPED

#include <PubSubClient.h>

#define BBL_MQTT_PORT        8883
#define BBL_MQTT_KEEPALIVE   30
#define BBL_MQTT_BUF_SIZE    (32*1024)
#define BBL_RECONNECT_SEC    30
#define BBL_PUSHALL_SEC      60

#define BBL_CLOUD_HOST_US "us.mqtt.bambulab.com"
#define BBL_CLOUD_HOST_CN "cn.mqtt.bambulab.com"

#define BBL_MODE_LOCAL  0
#define BBL_MODE_CLOUD  1

#define BBL_REGION_US  0
#define BBL_REGION_EU  1
#define BBL_REGION_CN  2

/*********************************************************************************************\
 * Lightweight JSON field extraction (strstr-based, no malloc)
\*********************************************************************************************/

static bool BblJsonGetFloat(const char *buf, const char *key, float *out) {
  const char *p = strstr(buf, key);
  if (!p) return false;
  p += strlen(key);
  while (*p && (*p == '"' || *p == ':' || *p == ' ')) p++;
  if (*p == '\0') return false;
  *out = strtof(p, nullptr);
  return true;
}

static bool BblJsonGetInt(const char *buf, const char *key, int *out) {
  const char *p = strstr(buf, key);
  if (!p) return false;
  p += strlen(key);
  while (*p && (*p == '"' || *p == ':' || *p == ' ')) p++;
  if (*p == '\0') return false;
  *out = strtol(p, nullptr, 10);
  return true;
}

static bool BblJsonGetStr(const char *buf, const char *key, char *dst, int dst_size) {
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

/*********************************************************************************************\
 * JWT UID extraction
\*********************************************************************************************/

static int BblBase64UrlDecode(const char *src, int src_len, char *dst, int dst_max) {
  char *tmp = (char*)malloc(src_len + 4);
  if (!tmp) return -1;
  for (int i = 0; i < src_len; i++) {
    if (src[i] == '-') tmp[i] = '+';
    else if (src[i] == '_') tmp[i] = '/';
    else tmp[i] = src[i];
  }
  int pad = (4 - (src_len % 4)) % 4;
  for (int i = 0; i < pad; i++) tmp[src_len + i] = '=';
  int total = src_len + pad;
  tmp[total] = '\0';
  int decoded_len = decode_base64((unsigned char*)tmp, (unsigned char*)dst);
  free(tmp);
  if (decoded_len >= dst_max) decoded_len = dst_max - 1;
  dst[decoded_len] = '\0';
  return decoded_len;
}

static bool BblExtractUidFromToken(const char *token, char *buf, int buf_size) {
  const char *dot1 = strchr(token, '.');
  if (!dot1) return false;
  const char *payload_start = dot1 + 1;
  const char *dot2 = strchr(payload_start, '.');
  if (!dot2) return false;
  int payload_b64_len = dot2 - payload_start;
  if (payload_b64_len > 1024) return false;
  char *decoded = (char*)malloc(payload_b64_len + 4);
  if (!decoded) return false;
  int decoded_len = BblBase64UrlDecode(payload_start, payload_b64_len, decoded, payload_b64_len + 4);
  if (decoded_len <= 0) { free(decoded); return false; }
  JsonParser parser(decoded);
  JsonParserObject root = parser.getRootObject();
  bool found = false;
  if (root) {
    static const char *uid_keys[] = { "uid", "sub", "user_id" };
    for (uint32_t i = 0; i < 3; i++) {
      JsonParserToken t = root[uid_keys[i]];
      if (t) {
        const char *uid_str = t.getStr();
        if (uid_str && strlen(uid_str) > 0) {
          snprintf(buf, buf_size, "u_%s", uid_str);
          found = true; break;
        }
        if (t.isInt()) {
          snprintf(buf, buf_size, "u_%d", t.getInt());
          found = true; break;
        }
      }
    }
  }
  free(decoded);
  return found;
}

/*********************************************************************************************\
 * BambuLab printer class
\*********************************************************************************************/

class GalopedPrinterBBL : public GalopedPrinter {
public:
  GalopedPrinterBBL(uint8_t slot) : GalopedPrinter(slot) {
    _mode = BBL_MODE_LOCAL;
    _region = BBL_REGION_EU;
    memset(_host, 0, sizeof(_host));
    memset(_serial, 0, sizeof(_serial));
    memset(_access_code, 0, sizeof(_access_code));
    memset(_cloud_token, 0, sizeof(_cloud_token));
    _tls = nullptr;
    _mqtt = nullptr;
    _last_reconnect = 0;
    _last_pushall = 0;
  }

  ~GalopedPrinterBBL() override {
    disconnect();
    if (_mqtt) { delete _mqtt; _mqtt = nullptr; }
    if (_tls) { delete _tls; _tls = nullptr; }
  }

  uint8_t type() override { return PRINTER_TYPE_BAMBULAB; }
  const char* typeName() override { return "BBL"; }

  void loadSettings(File &ini_file) override {
    IniFile ini(ini_file);
    char tmp[256];

    int32_t mode = 0;
    ini.getValueInt("printer", "mode", mode);
    _mode = mode;

    int32_t region = BBL_REGION_EU;
    ini.getValueInt("printer", "cloud_region", region);
    _region = region;

    ini.getValueStr("printer", "host", _host, sizeof(_host));
    ini.getValueStr("printer", "serial", _serial, sizeof(_serial));
    ini.getValueStr("printer", "access_code", _access_code, sizeof(_access_code));
    ini.getValueStr("printer", "cloud_token", _cloud_token, sizeof(_cloud_token));

    AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: Loaded host=%s serial=%s mode=%d"), _slot, _host, _serial, _mode);
  }

  void saveSettings(String &out) override {
    out += "mode="; out += _mode; out += "\n";
    out += "host="; out += _host; out += "\n";
    out += "serial="; out += _serial; out += "\n";
    out += "access_code="; out += _access_code; out += "\n";
    out += "cloud_region="; out += _region; out += "\n";
    out += "cloud_token="; out += _cloud_token; out += "\n";
  }

  void begin() override {
    // Nothing to pre-allocate; connect happens in everySecond
  }

  void disconnect() override {
    if (_mqtt) _mqtt->disconnect();
    if (_tls) _tls->stop();
    status.connected = false;
  }

  void loop() override {
    if (_mqtt && status.connected) {
      if (!_mqtt->loop()) {
        AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: Connection lost"), _slot);
        status.connected = false;
        status.data_valid = false;
        _tls->stop();
      }
    }
  }

  void everySecond() override {
    if (!isConfigured()) return;

    if (_mqtt && status.connected) {
      if (TasmotaGlobal.uptime - _last_pushall >= BBL_PUSHALL_SEC) {
        sendPushAll();
      }
    } else {
      if (TasmotaGlobal.uptime - _last_reconnect >= BBL_RECONNECT_SEC) {
        doConnect();
      }
    }
  }

  void webFormFields() override {
    WSContentSend_P(PSTR(
      "<p><b>Serial</b><br><input name='bs' maxlength='19' value='%s'></p>"), _serial);
    WSContentSend_P(PSTR(
      "<p><b>Host / IP</b><br><input name='bh' maxlength='39' value='%s'></p>"), _host);
    WSContentSend_P(PSTR(
      "<p><b>LAN Access Code</b><br><input name='ba' type='password' maxlength='19' value='%s'></p>"), _access_code);
  }

  void webFormSave() override {
    char tmp[256];
    WebGetArg(PSTR("bs"), tmp, sizeof(tmp)); strlcpy(_serial, tmp, sizeof(_serial));
    WebGetArg(PSTR("bh"), tmp, sizeof(tmp)); strlcpy(_host, tmp, sizeof(_host));
    WebGetArg(PSTR("ba"), tmp, sizeof(tmp)); strlcpy(_access_code, tmp, sizeof(_access_code));
    _mode = BBL_MODE_LOCAL;
  }

private:
  uint8_t  _mode;
  uint8_t  _region;
  char     _host[40];
  char     _serial[20];
  char     _access_code[20];
  char     _cloud_token[256];
  BearSSL::WiFiClientSecure_light *_tls;
  PubSubClient *_mqtt;
  uint32_t _last_reconnect;
  uint32_t _last_pushall;

  bool isConfigured() {
    if (_mode == BBL_MODE_LOCAL) {
      return strlen(_host) > 0 && strlen(_serial) > 0 && strlen(_access_code) > 0;
    }
    return strlen(_serial) > 0 && strlen(_cloud_token) > 0;
  }

  static void mqttCallbackStatic(char *topic, uint8_t *payload, unsigned int length) {
    // Find which instance this belongs to by matching topic serial
    for (uint8_t i = 0; i < GALOPED_PRINTER_MAX; i++) {
      GalopedPrinter *p = galoped_printers[i];
      if (p && p->type() == PRINTER_TYPE_BAMBULAB) {
        GalopedPrinterBBL *bbl = static_cast<GalopedPrinterBBL*>(p);
        if (strstr(topic, bbl->_serial)) {
          bbl->handleMessage(payload, length);
          return;
        }
      }
    }
  }

  void handleMessage(uint8_t *payload, unsigned int length) {
    if (length < 10) return;

    char saved = ((char*)payload)[length];
    ((char*)payload)[length] = '\0';
    const char *json = (const char*)payload;

    if (!strstr(json, "\"print\"")) {
      ((char*)payload)[length] = saved;
      return;
    }

    float f; int i; bool updated = false;

    if (BblJsonGetFloat(json, "\"nozzle_temper\"", &f))        { status.nozzle_temp = f; updated = true; }
    if (BblJsonGetFloat(json, "\"nozzle_target_temper\"", &f)) { status.nozzle_target = f; updated = true; }
    if (BblJsonGetFloat(json, "\"bed_temper\"", &f))           { status.bed_temp = f; updated = true; }
    if (BblJsonGetFloat(json, "\"bed_target_temper\"", &f))    { status.bed_target = f; updated = true; }
    if (BblJsonGetInt(json, "\"mc_percent\"", &i))             { status.progress = i; updated = true; }
    if (BblJsonGetInt(json, "\"mc_remaining_time\"", &i))      { status.remaining_min = i; updated = true; }

    char state_str[16] = "";
    if (BblJsonGetStr(json, "\"gcode_state\"", state_str, sizeof(state_str))) {
      if      (strcmp(state_str, "IDLE") == 0)    status.state = PRINTER_STATE_IDLE;
      else if (strcmp(state_str, "RUNNING") == 0) status.state = PRINTER_STATE_RUNNING;
      else if (strcmp(state_str, "PAUSE") == 0)   status.state = PRINTER_STATE_PAUSE;
      else if (strcmp(state_str, "FINISH") == 0)  status.state = PRINTER_STATE_FINISH;
      else                                        status.state = PRINTER_STATE_UNKNOWN;
      updated = true;
    }

    ((char*)payload)[length] = saved;

    if (updated) {
      status.data_valid = true;
      status.last_update = TasmotaGlobal.uptime;
      AddLog(LOG_LEVEL_DEBUG, PSTR("BBL[%d]: nozzle=%.0f/%.0f bed=%.0f/%.0f %d%% %s"),
        _slot, status.nozzle_temp, status.nozzle_target,
        status.bed_temp, status.bed_target, status.progress, stateStr());
    }
  }

  void doConnect() {
    if (!WifiHasIP()) return;
    _last_reconnect = TasmotaGlobal.uptime;

    if (_tls) _tls->stop();

    if (!_tls) {
      _tls = new BearSSL::WiFiClientSecure_light(4096, 4096);
      if (!_tls) { AddLog(LOG_LEVEL_ERROR, PSTR("BBL[%d]: OOM TLS"), _slot); return; }
    }
    _tls->setInsecure();

    if (!_mqtt) {
      _mqtt = new PubSubClient();
      if (!_mqtt) { AddLog(LOG_LEVEL_ERROR, PSTR("BBL[%d]: OOM MQTT"), _slot); return; }
      _mqtt->setClient(*_tls);
      _mqtt->setBufferSize(BBL_MQTT_BUF_SIZE);
      _mqtt->setKeepAlive(BBL_MQTT_KEEPALIVE);
      _mqtt->setCallback(mqttCallbackStatic);
    }

    const char *host, *user, *pass;
    char client_id[64];
    static char cloud_uid[48];

    if (_mode == BBL_MODE_LOCAL) {
      host = _host; user = "bblp"; pass = _access_code;
    } else {
      host = (_region == BBL_REGION_CN) ? BBL_CLOUD_HOST_CN : BBL_CLOUD_HOST_US;
      if (!BblExtractUidFromToken(_cloud_token, cloud_uid, sizeof(cloud_uid))) {
        AddLog(LOG_LEVEL_ERROR, PSTR("BBL[%d]: JWT parse failed"), _slot);
        return;
      }
      user = cloud_uid; pass = _cloud_token;
    }
    snprintf(client_id, sizeof(client_id), "bblp_%08X_%d", ESP_getChipId(), _slot);

    AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: Connecting %s:%d user=%s"), _slot, host, BBL_MQTT_PORT, user);

    IPAddress ip;
    if (!WifiHostByName(host, ip)) {
      AddLog(LOG_LEVEL_ERROR, PSTR("BBL[%d]: DNS failed %s"), _slot, host);
      return;
    }

    _mqtt->setServer(ip, BBL_MQTT_PORT);

    if (_mqtt->connect(client_id, user, pass)) {
      status.connected = true;
      char topic[64];
      snprintf(topic, sizeof(topic), "device/%s/report", _serial);
      _mqtt->subscribe(topic);
      AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: Connected, subscribed %s"), _slot, topic);
      sendPushAll();
    } else {
      int rc = _mqtt->state();
      AddLog(LOG_LEVEL_ERROR, PSTR("BBL[%d]: Connect failed rc=%d"), _slot, rc);
      status.connected = false;
      _tls->stop();
    }
  }

  void sendPushAll() {
    if (!_mqtt || !status.connected) return;
    char topic[64];
    snprintf(topic, sizeof(topic), "device/%s/request", _serial);
    _mqtt->publish(topic, "{\"pushing\":{\"command\":\"pushall\"}}");
    _last_pushall = TasmotaGlobal.uptime;
  }
};

#endif  // USE_GALOPED
