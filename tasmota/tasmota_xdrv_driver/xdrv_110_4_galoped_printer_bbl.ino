/*
  xdrv_110_4_galoped_printer_bbl.ino - BambuLab printer implementation

  https://github.com/petrows/smarthome-galoped-dekad
  Copyright (C) 2026 by Petr Golovachev <petro@petro.ws>
  License: GPLv3+

  Connects to BambuLab printer via MQTT over TLS (port 8883).
  Supports local (LAN) and BambuCloud connections.
  BambuCloud: EU and US use us.mqtt.bambulab.com, CN uses cn.mqtt.bambulab.com.
  Cloud auth: UID extracted automatically from JWT access token.
*/

#ifdef USE_GALOPED

#include <PubSubClient.h>

// MQTT connection parameters
#define BBL_MQTT_PORT      8883
#define BBL_MQTT_KEEPALIVE 30
#define BBL_MQTT_BUF_SIZE  (32 * 1024)  // BambuLab pushall response can be 10-20KB

// Timing (seconds)
#define BBL_RECONNECT_SEC  30   // Delay between reconnect attempts
#define BBL_PUSHALL_SEC    60   // Interval for pushall status requests

// Cloud MQTT broker endpoints (EU uses US broker)
#define BBL_CLOUD_HOST_US  "us.mqtt.bambulab.com"
#define BBL_CLOUD_HOST_CN  "cn.mqtt.bambulab.com"

// Connection modes
#define BBL_MODE_LOCAL  0
#define BBL_MODE_CLOUD  1

// Cloud regions
#define BBL_REGION_US   0
#define BBL_REGION_EU   1
#define BBL_REGION_CN   2

/*********************************************************************************************\
 * JWT token helpers (for cloud authentication)
 *
 * BambuCloud MQTT requires username "u_<UID>" where UID is extracted
 * from the JWT access token payload.
\*********************************************************************************************/

// Decode base64url (JWT uses URL-safe base64 without padding)
static int BblBase64UrlDecode(const char *src, int src_len, char *dst, int dst_max) {
  char *tmp = (char *)malloc(src_len + 4);
  if (!tmp) return -1;

  // Convert base64url alphabet to standard base64
  for (int i = 0; i < src_len; i++) {
    if (src[i] == '-')      tmp[i] = '+';
    else if (src[i] == '_') tmp[i] = '/';
    else                    tmp[i] = src[i];
  }

  // Add padding
  int pad = (4 - (src_len % 4)) % 4;
  for (int i = 0; i < pad; i++) {
    tmp[src_len + i] = '=';
  }
  tmp[src_len + pad] = '\0';

  int decoded_len = decode_base64((unsigned char *)tmp, (unsigned char *)dst);
  free(tmp);

  if (decoded_len >= dst_max) {
    decoded_len = dst_max - 1;
  }
  dst[decoded_len] = '\0';
  return decoded_len;
}

// Extract UID from JWT token, writes "u_<uid>" into buf
// JWT format: header.payload.signature (each part is base64url)
static bool BblExtractUidFromToken(const char *token, char *buf, int buf_size) {
  // Find payload segment (between first and second '.')
  const char *dot1 = strchr(token, '.');
  if (!dot1) return false;

  const char *payload_start = dot1 + 1;
  const char *dot2 = strchr(payload_start, '.');
  if (!dot2) return false;

  int payload_len = dot2 - payload_start;
  if (payload_len > 1024) return false;  // Sanity check

  // Decode payload JSON
  char *decoded = (char *)malloc(payload_len + 4);
  if (!decoded) return false;

  if (BblBase64UrlDecode(payload_start, payload_len, decoded, payload_len + 4) <= 0) {
    free(decoded);
    return false;
  }

  // Parse JSON to find UID field
  JsonParser parser(decoded);
  JsonParserObject root = parser.getRootObject();
  bool found = false;

  if (root) {
    // Try common UID field names used by Bambu
    static const char *uid_keys[] = { "uid", "sub", "user_id" };
    for (uint32_t i = 0; i < 3 && !found; i++) {
      JsonParserToken t = root[uid_keys[i]];
      if (t) {
        const char *uid_str = t.getStr();
        if (uid_str && strlen(uid_str)) {
          snprintf(buf, buf_size, "u_%s", uid_str);
          found = true;
        } else if (t.isInt()) {
          snprintf(buf, buf_size, "u_%d", t.getInt());
          found = true;
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
  GalopedPrinterBBL(uint8_t slot) : GalopedPrinter(slot),
    _mode(BBL_MODE_LOCAL),
    _region(BBL_REGION_EU),
    _tls(nullptr),
    _mqtt(nullptr),
    _last_reconnect(0),
    _last_pushall(0)
  {
    memset(_host, 0, sizeof(_host));
    memset(_serial, 0, sizeof(_serial));
    memset(_access_code, 0, sizeof(_access_code));
    memset(_cloud_token, 0, sizeof(_cloud_token));
  }

  ~GalopedPrinterBBL() override {
    prtDisconnect();
    delete _mqtt;   _mqtt = nullptr;
    delete _tls;    _tls = nullptr;
  }

  uint8_t prtType() override { return PRINTER_TYPE_BAMBULAB; }
  const char* prtTypeName() override { return "BBL"; }

  /*********************************************************************************************\
   * Settings
  \*********************************************************************************************/

  void prtLoadSettings(File &ini_file) override {
    IniFile ini(ini_file);
    int32_t val;

    if (ini.getValueInt("printer", "mode", val))         _mode = val;
    if (ini.getValueInt("printer", "cloud_region", val))  _region = val;

    ini.getValueStr("printer", "host",         _host,         sizeof(_host));
    ini.getValueStr("printer", "serial",       _serial,       sizeof(_serial));
    ini.getValueStr("printer", "access_code",  _access_code,  sizeof(_access_code));
    ini.getValueStr("printer", "cloud_token",  _cloud_token,  sizeof(_cloud_token));

    AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: host=%s serial=%s mode=%d"),
           _slot, _host, _serial, _mode);
  }

  void prtSaveSettings(String &out) override {
    out += "mode=";         out += _mode;         out += "\n";
    out += "host=";         out += _host;         out += "\n";
    out += "serial=";       out += _serial;       out += "\n";
    out += "access_code=";  out += _access_code;  out += "\n";
    out += "cloud_region="; out += _region;        out += "\n";
    out += "cloud_token=";  out += _cloud_token;  out += "\n";
  }

  /*********************************************************************************************\
   * Lifecycle
  \*********************************************************************************************/

  void prtBegin() override {
    // Connection will be initiated from prtEverySecond()
  }

  void prtDisconnect() override {
    if (_mqtt) _mqtt->disconnect();
    if (_tls)  _tls->stop();
    status.connected = false;
  }

  // Must be called frequently to process incoming MQTT packets
  void prtLoop() override {
    if (!_mqtt || !status.connected) return;

    if (!_mqtt->loop()) {
      AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: Connection lost"), _slot);
      status.connected = false;
      status.data_valid = false;
      if (_tls) _tls->stop();
    }
  }

  // Reconnect and periodic pushall timing
  void prtEverySecond() override {
    if (!bblIsConfigured()) return;

    if (_mqtt && status.connected) {
      // Send periodic pushall to keep data fresh
      if (TasmotaGlobal.uptime - _last_pushall >= BBL_PUSHALL_SEC) {
        bblSendPushAll();
      }
    } else {
      // Try to reconnect
      if (TasmotaGlobal.uptime - _last_reconnect >= BBL_RECONNECT_SEC) {
        bblDoConnect();
      }
    }
  }

  /*********************************************************************************************\
   * Web UI form fields
  \*********************************************************************************************/

  void prtWebFormFields() override {
    WSContentSend_P(PSTR(
      "<p><b>Serial</b><br>"
      "<input name='bs' maxlength='19' value='%s'></p>"), _serial);

    WSContentSend_P(PSTR(
      "<p><b>Host / IP</b><br>"
      "<input name='bh' maxlength='39' value='%s'></p>"), _host);

    WSContentSend_P(PSTR(
      "<p><b>LAN Access Code</b><br>"
      "<input name='ba' type='password' maxlength='19' value='%s'></p>"), _access_code);
  }

  void prtWebFormSave() override {
    char tmp[256];

    WebGetArg(PSTR("bs"), tmp, sizeof(tmp));
    strlcpy(_serial, tmp, sizeof(_serial));

    WebGetArg(PSTR("bh"), tmp, sizeof(tmp));
    strlcpy(_host, tmp, sizeof(_host));

    WebGetArg(PSTR("ba"), tmp, sizeof(tmp));
    strlcpy(_access_code, tmp, sizeof(_access_code));

    _mode = BBL_MODE_LOCAL;
  }

private:
  // Settings
  uint8_t  _mode;
  uint8_t  _region;
  char     _host[40];
  char     _serial[20];
  char     _access_code[20];
  char     _cloud_token[256];

  // MQTT client
  BearSSL::WiFiClientSecure_light *_tls;
  PubSubClient *_mqtt;

  // Timing
  uint32_t _last_reconnect;
  uint32_t _last_pushall;

  /*********************************************************************************************\
   * Internal helpers
  \*********************************************************************************************/

  bool bblIsConfigured() {
    if (_mode == BBL_MODE_LOCAL) {
      return strlen(_host) > 0 && strlen(_serial) > 0 && strlen(_access_code) > 0;
    }
    return strlen(_serial) > 0 && strlen(_cloud_token) > 0;
  }

  // Static MQTT callback - routes message to the correct BBL instance
  static void bblMqttCbStatic(char *topic, uint8_t *payload, unsigned int length) {
    for (uint8_t i = 0; i < GALOPED_PRINTER_MAX; i++) {
      GalopedPrinter *p = galoped_printers[i];
      if (p && p->prtType() == PRINTER_TYPE_BAMBULAB) {
        GalopedPrinterBBL *bbl = static_cast<GalopedPrinterBBL *>(p);
        if (strstr(topic, bbl->_serial)) {
          bbl->bblOnMessage(payload, length);
          return;
        }
      }
    }
  }

  // Process incoming MQTT message with printer status
  void bblOnMessage(uint8_t *payload, unsigned int length) {
    if (length < 10) return;

    // Null-terminate payload in-place for strstr
    char saved = ((char *)payload)[length];
    ((char *)payload)[length] = '\0';
    const char *json = (const char *)payload;

    // Only process messages containing the "print" object
    if (!strstr(json, "\"print\"")) {
      ((char *)payload)[length] = saved;
      return;
    }

    float f;
    int i;
    bool updated = false;

    // Extract temperature data
    if (PrtJsonGetFloat(json, "\"nozzle_temper\"", &f)) {
      status.nozzle_temp = f;
      updated = true;
    }
    if (PrtJsonGetFloat(json, "\"nozzle_target_temper\"", &f)) {
      status.nozzle_target = f;
      updated = true;
    }
    if (PrtJsonGetFloat(json, "\"bed_temper\"", &f)) {
      status.bed_temp = f;
      updated = true;
    }
    if (PrtJsonGetFloat(json, "\"bed_target_temper\"", &f)) {
      status.bed_target = f;
      updated = true;
    }

    // Extract progress data
    if (PrtJsonGetInt(json, "\"mc_percent\"", &i)) {
      status.progress = i;
      updated = true;
    }
    if (PrtJsonGetInt(json, "\"mc_remaining_time\"", &i)) {
      status.remaining_min = i;
      updated = true;
    }

    // Extract printer state
    char state_str[16] = "";
    if (PrtJsonGetStr(json, "\"gcode_state\"", state_str, sizeof(state_str))) {
      if      (!strcmp(state_str, "IDLE"))    status.state = PRINTER_STATE_IDLE;
      else if (!strcmp(state_str, "RUNNING")) status.state = PRINTER_STATE_RUNNING;
      else if (!strcmp(state_str, "PAUSE"))   status.state = PRINTER_STATE_PAUSE;
      else if (!strcmp(state_str, "FINISH"))  status.state = PRINTER_STATE_FINISH;
      else                                   status.state = PRINTER_STATE_UNKNOWN;
      updated = true;
    }

    // Restore original byte
    ((char *)payload)[length] = saved;

    if (updated) {
      status.data_valid = true;
      status.last_update = TasmotaGlobal.uptime;

      AddLog(LOG_LEVEL_DEBUG,
             PSTR("BBL[%d]: nozzle=%.0f/%.0f bed=%.0f/%.0f %d%% %s"),
             _slot, status.nozzle_temp, status.nozzle_target,
             status.bed_temp, status.bed_target,
             status.progress, stateStr());
    }
  }

  // Establish MQTT connection to the printer
  void bblDoConnect() {
    if (!WifiHasIP()) return;

    _last_reconnect = TasmotaGlobal.uptime;

    // Clean up previous TLS session
    if (_tls) _tls->stop();

    // Allocate TLS client on first use
    if (!_tls) {
      _tls = new BearSSL::WiFiClientSecure_light(4096, 4096);
      if (!_tls) return;
    }
    _tls->setInsecure();  // BambuLab uses self-signed certs locally

    // Allocate MQTT client on first use
    if (!_mqtt) {
      _mqtt = new PubSubClient();
      _mqtt->setClient(*_tls);
      _mqtt->setBufferSize(BBL_MQTT_BUF_SIZE);
      _mqtt->setKeepAlive(BBL_MQTT_KEEPALIVE);
      _mqtt->setCallback(bblMqttCbStatic);
    }

    // Determine connection parameters based on mode
    const char *host;
    const char *user;
    const char *pass;
    char client_id[64];
    static char cloud_uid[48];

    if (_mode == BBL_MODE_LOCAL) {
      host = _host;
      user = "bblp";
      pass = _access_code;
    } else {
      // Cloud mode: EU and US both use US broker
      host = (_region == BBL_REGION_CN) ? BBL_CLOUD_HOST_CN : BBL_CLOUD_HOST_US;

      // Extract UID from JWT access token
      if (!BblExtractUidFromToken(_cloud_token, cloud_uid, sizeof(cloud_uid))) {
        AddLog(LOG_LEVEL_ERROR, PSTR("BBL[%d]: JWT parse failed"), _slot);
        return;
      }
      user = cloud_uid;
      pass = _cloud_token;
    }

    snprintf(client_id, sizeof(client_id), "bblp_%08X_%d", ESP_getChipId(), _slot);

    AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: Connecting to %s:%d user=%s"),
           _slot, host, BBL_MQTT_PORT, user);

    // DNS resolve
    IPAddress ip;
    if (!WifiHostByName(host, ip)) {
      AddLog(LOG_LEVEL_ERROR, PSTR("BBL[%d]: DNS failed for %s"), _slot, host);
      return;
    }

    // Connect
    _mqtt->setServer(ip, BBL_MQTT_PORT);

    if (_mqtt->connect(client_id, user, pass)) {
      status.connected = true;

      // Subscribe to printer report topic
      char topic[64];
      snprintf(topic, sizeof(topic), "device/%s/report", _serial);
      _mqtt->subscribe(topic);

      AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: Connected, subscribed to %s"), _slot, topic);
      bblSendPushAll();
    } else {
      AddLog(LOG_LEVEL_ERROR, PSTR("BBL[%d]: Connection failed rc=%d"), _slot, _mqtt->state());
      status.connected = false;
      _tls->stop();
    }
  }

  // Request full printer status
  void bblSendPushAll() {
    if (!_mqtt || !status.connected) return;

    char topic[64];
    snprintf(topic, sizeof(topic), "device/%s/request", _serial);
    _mqtt->publish(topic, "{\"pushing\":{\"command\":\"pushall\"}}");
    _last_pushall = TasmotaGlobal.uptime;
  }
};

#endif  // USE_GALOPED
