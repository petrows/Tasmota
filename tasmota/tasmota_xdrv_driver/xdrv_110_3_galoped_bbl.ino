/*
  xdrv_110_3_galoped_bbl.ino - BambuLab printer integration for Galoped

  https://github.com/petrows/smarthome-galoped-dekad

  Copyright (C) 2026 by Petr Golovachev <petro@petro.ws>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
  BambuLab 3D printer MQTT integration module.

  Connects to a BambuLab printer via MQTT over TLS (port 8883)
  and reads nozzle temperature and print progress.

  Supports both local (LAN) and BambuCloud connections.
*/

#ifdef USE_GALOPED

#include <PubSubClient.h>

#define WEB_HANDLE_BBL_CFG "bblcfg"

#define BBL_MQTT_PORT        8883
#define BBL_MQTT_KEEPALIVE   30
#define BBL_MQTT_BUF_SIZE    4096   // BambuLab sends large JSON payloads
#define BBL_RECONNECT_INTERVAL 30   // Seconds between reconnect attempts
#define BBL_PUSHALL_INTERVAL 60     // Seconds between pushall requests

// Cloud MQTT endpoints (EU uses US broker)
#define BBL_CLOUD_HOST_US "us.mqtt.bambulab.com"
#define BBL_CLOUD_HOST_CN "cn.mqtt.bambulab.com"

// Cloud regions
#define BBL_REGION_US  0
#define BBL_REGION_EU  1
#define BBL_REGION_CN  2

// Connection modes
#define BBL_MODE_LOCAL  0
#define BBL_MODE_CLOUD  1

#define BBL_SETTINGS_FILE "bbl"

/*********************************************************************************************\
 * Settings
\*********************************************************************************************/

struct BblSettings {
  uint32_t crc32;
  uint8_t  mode;              // 0=local, 1=cloud
  uint8_t  cloud_region;      // BBL_REGION_US/EU/CN
  char     host[40];          // Printer IP (local)
  char     serial[20];        // Printer serial number
  char     access_code[20];   // LAN access code (local mode)
  char     cloud_token[256];  // Cloud access token (JWT)
};

static BblSettings bbl_settings;

struct BblState {
  bool     connected;
  bool     data_valid;
  float    nozzle_temp;
  float    nozzle_target;
  float    bed_temp;
  float    bed_target;
  uint8_t  print_progress;    // 0-100%
  uint16_t remaining_min;     // Remaining time in minutes
  char     gcode_state[16];   // IDLE, RUNNING, PAUSE, FINISH, etc.
  uint32_t last_update;       // Uptime of last data received
  uint32_t last_reconnect;    // Uptime of last reconnect attempt
  uint32_t last_pushall;      // Uptime of last pushall request
};

static BblState bbl_state;

static BearSSL::WiFiClientSecure_light *bbl_tls_client = nullptr;
static PubSubClient *bbl_mqtt_client = nullptr;

/*********************************************************************************************\
 * Settings load/save
\*********************************************************************************************/

static void BblSettingsDefault(void) {
  memset(&bbl_settings, 0x00, sizeof(bbl_settings));
  bbl_settings.mode = BBL_MODE_LOCAL;
  bbl_settings.cloud_region = BBL_REGION_EU;
}

static void BblSettingsLoad(void) {
  BblSettingsDefault();
#ifdef USE_UFILESYS
  char filename[20];
  snprintf_P(filename, sizeof(filename), PSTR("/" BBL_SETTINGS_FILE ".dat"));
  if (TfsLoadFile(filename, (uint8_t*)&bbl_settings, sizeof(bbl_settings))) {
    AddLog(LOG_LEVEL_INFO, PSTR("BBL: Settings loaded, mode=%d, host=%s, serial=%s"),
           bbl_settings.mode, bbl_settings.host, bbl_settings.serial);
  } else {
    AddLog(LOG_LEVEL_DEBUG, PSTR("BBL: Settings file not found, using defaults"));
  }
#endif
}

static void BblSettingsSave(void) {
#ifdef USE_UFILESYS
  uint32_t crc32 = GetCfgCrc32((uint8_t*)&bbl_settings + 4, sizeof(bbl_settings) - 4);
  if (crc32 != bbl_settings.crc32) {
    bbl_settings.crc32 = crc32;
    char filename[20];
    snprintf_P(filename, sizeof(filename), PSTR("/" BBL_SETTINGS_FILE ".dat"));
    TfsSaveFile(filename, (const uint8_t*)&bbl_settings, sizeof(bbl_settings));
    AddLog(LOG_LEVEL_DEBUG, PSTR("BBL: Settings saved"));
  }
#endif
}

/*********************************************************************************************\
 * MQTT message callback
\*********************************************************************************************/

static void BblMqttCallback(char *topic, uint8_t *payload, unsigned int length) {
  // Safety: payload can be very large, we only need specific fields
  // JsonParser modifies the buffer in-place, so we need a writable copy
  // For memory efficiency, scan for the "print" object only
  if (length < 10) return;

  AddLog(LOG_LEVEL_DEBUG, PSTR("BBL: MQTT data %d"), length);

  // Null-terminate the payload for string operations
  char *json = (char*)payload;
  char saved = json[length];
  json[length] = '\0';

  JsonParser parser(json);
  JsonParserObject root = parser.getRootObject();
  if (!root) {
    json[length] = saved;
    return;
  }

  JsonParserToken print_token = root[PSTR("print")];
  if (!print_token) {
    json[length] = saved;
    return;
  }

  JsonParserObject print_obj = print_token.getObject();
  if (!print_obj) {
    json[length] = saved;
    return;
  }

  // Extract temperature data
  JsonParserToken t;

  t = print_obj[PSTR("nozzle_temper")];
  if (t) bbl_state.nozzle_temp = t.getFloat();

  t = print_obj[PSTR("nozzle_target_temper")];
  if (t) bbl_state.nozzle_target = t.getFloat();

  t = print_obj[PSTR("bed_temper")];
  if (t) bbl_state.bed_temp = t.getFloat();

  t = print_obj[PSTR("bed_target_temper")];
  if (t) bbl_state.bed_target = t.getFloat();

  t = print_obj[PSTR("mc_percent")];
  if (t) bbl_state.print_progress = t.getInt();

  t = print_obj[PSTR("mc_remaining_time")];
  if (t) bbl_state.remaining_min = t.getInt();

  t = print_obj[PSTR("gcode_state")];
  if (t) {
    const char *state = t.getStr();
    if (state) {
      strlcpy(bbl_state.gcode_state, state, sizeof(bbl_state.gcode_state));
    }
  }

  bbl_state.data_valid = true;
  bbl_state.last_update = TasmotaGlobal.uptime;

  json[length] = saved;
}

/*********************************************************************************************\
 * JWT token UID extraction (cloud mode)
 * JWT format: header.payload.signature (base64url encoded)
 * Payload contains "uid", "sub", or "user_id" field
\*********************************************************************************************/

// Base64url decode (no padding, url-safe alphabet)
static int BblBase64UrlDecode(const char *src, int src_len, char *dst, int dst_max) {
  // Translate base64url to standard base64
  char *tmp = (char*)malloc(src_len + 4);
  if (!tmp) return -1;

  for (int i = 0; i < src_len; i++) {
    if (src[i] == '-') tmp[i] = '+';
    else if (src[i] == '_') tmp[i] = '/';
    else tmp[i] = src[i];
  }
  // Add padding
  int pad = (4 - (src_len % 4)) % 4;
  for (int i = 0; i < pad; i++) tmp[src_len + i] = '=';
  int total = src_len + pad;
  tmp[total] = '\0';

  // Decode using Tasmota's base64 decode
  int decoded_len = decode_base64((unsigned char*)tmp, (unsigned char*)dst);
  free(tmp);

  if (decoded_len >= dst_max) decoded_len = dst_max - 1;
  dst[decoded_len] = '\0';
  return decoded_len;
}

// Extract UID from JWT token, writes "u_<uid>" into buf
// Returns true on success
static bool BblExtractUidFromToken(const char *token, char *buf, int buf_size) {
  // Find second segment (payload) between first and second '.'
  const char *dot1 = strchr(token, '.');
  if (!dot1) return false;
  const char *payload_start = dot1 + 1;
  const char *dot2 = strchr(payload_start, '.');
  if (!dot2) return false;

  int payload_b64_len = dot2 - payload_start;
  if (payload_b64_len > 1024) return false;  // Sanity check

  // Decode payload
  char *decoded = (char*)malloc(payload_b64_len + 4);
  if (!decoded) return false;

  int decoded_len = BblBase64UrlDecode(payload_start, payload_b64_len, decoded, payload_b64_len + 4);
  if (decoded_len <= 0) {
    free(decoded);
    return false;
  }

  // Parse JSON to find UID
  JsonParser parser(decoded);
  JsonParserObject root = parser.getRootObject();
  bool found = false;

  if (root) {
    // Try common UID field names
    static const char *uid_keys[] = { "uid", "sub", "user_id" };
    for (uint32_t i = 0; i < 3; i++) {
      JsonParserToken t = root[uid_keys[i]];
      if (t) {
        const char *uid_str = t.getStr();
        if (uid_str && strlen(uid_str) > 0) {
          snprintf(buf, buf_size, "u_%s", uid_str);
          found = true;
          break;
        }
        // Try as integer
        if (t.isInt()) {
          snprintf(buf, buf_size, "u_%d", t.getInt());
          found = true;
          break;
        }
      }
    }
  }

  free(decoded);
  return found;
}

/*********************************************************************************************\
 * MQTT connection management
\*********************************************************************************************/

static bool BblIsConfigured(void) {
  if (bbl_settings.mode == BBL_MODE_LOCAL) {
    return (strlen(bbl_settings.host) > 0 &&
            strlen(bbl_settings.serial) > 0 &&
            strlen(bbl_settings.access_code) > 0);
  } else {
    return (strlen(bbl_settings.serial) > 0 &&
            strlen(bbl_settings.cloud_token) > 0);
  }
}

static const char* BblCloudHost(void) {
  if (bbl_settings.cloud_region == BBL_REGION_CN) {
    return BBL_CLOUD_HOST_CN;
  }
  return BBL_CLOUD_HOST_US;  // US and EU both use US broker
}

static void BblDisconnect(void) {
  if (bbl_mqtt_client) {
    bbl_mqtt_client->disconnect();
  }
  if (bbl_tls_client) {
    bbl_tls_client->stop();
  }
  bbl_state.connected = false;
}

static void BblConnect(void) {
  if (!BblIsConfigured()) return;
  if (!WifiHasIP()) return;  // Wait for WiFi to be up

  bbl_state.last_reconnect = TasmotaGlobal.uptime;

  // Clean up previous TLS connection before reconnecting
  if (bbl_tls_client) {
    bbl_tls_client->stop();
  }

  // Allocate clients on first use
  if (!bbl_tls_client) {
    bbl_tls_client = new BearSSL::WiFiClientSecure_light(4096, 4096);
    if (!bbl_tls_client) {
      AddLog(LOG_LEVEL_ERROR, PSTR("BBL: Failed to allocate TLS client"));
      return;
    }
  }
  // BambuLab uses self-signed certs, skip verification
  bbl_tls_client->setInsecure();

  if (!bbl_mqtt_client) {
    bbl_mqtt_client = new PubSubClient();
    if (!bbl_mqtt_client) {
      AddLog(LOG_LEVEL_ERROR, PSTR("BBL: Failed to allocate MQTT client"));
      return;
    }
    bbl_mqtt_client->setClient(*bbl_tls_client);
    bbl_mqtt_client->setBufferSize(BBL_MQTT_BUF_SIZE);
    bbl_mqtt_client->setKeepAlive(BBL_MQTT_KEEPALIVE);
    bbl_mqtt_client->setCallback(BblMqttCallback);
  }

  // Determine host and credentials
  const char *host;
  const char *user;
  const char *pass;
  char client_id[64];
  static char cloud_uid[48];

  if (bbl_settings.mode == BBL_MODE_LOCAL) {
    host = bbl_settings.host;
    user = "bblp";
    pass = bbl_settings.access_code;
  } else {
    host = BblCloudHost();
    // Extract UID from JWT token
    if (!BblExtractUidFromToken(bbl_settings.cloud_token, cloud_uid, sizeof(cloud_uid))) {
      AddLog(LOG_LEVEL_ERROR, PSTR("BBL: Failed to extract UID from cloud token"));
      return;
    }
    user = cloud_uid;
    pass = bbl_settings.cloud_token;
  }
  snprintf(client_id, sizeof(client_id), "bblp_%08X", ESP_getChipId());

  AddLog(LOG_LEVEL_INFO, PSTR("BBL: Connecting to %s:%d (%s), user=%s, pass_len=%d, client=%s"),
         host, BBL_MQTT_PORT,
         bbl_settings.mode == BBL_MODE_LOCAL ? "local" : "cloud",
         user, strlen(pass), client_id);

  // Resolve and connect
  IPAddress ip;
  if (!WifiHostByName(host, ip)) {
    AddLog(LOG_LEVEL_ERROR, PSTR("BBL: DNS resolve failed for %s"), host);
    return;
  }

  bbl_mqtt_client->setServer(ip, BBL_MQTT_PORT);

  if (bbl_mqtt_client->connect(client_id, user, pass)) {
    AddLog(LOG_LEVEL_INFO, PSTR("BBL: Connected to %s"), host);
    bbl_state.connected = true;

    // Subscribe to printer reports
    char topic[64];
    snprintf(topic, sizeof(topic), "device/%s/report", bbl_settings.serial);
    bbl_mqtt_client->subscribe(topic);
    AddLog(LOG_LEVEL_INFO, PSTR("BBL: Subscribed to %s"), topic);

    // Request full status
    BblSendPushAll();
  } else {
    int rc = bbl_mqtt_client->state();
    const char *err;
    switch (rc) {
      case -4: err = "TIMEOUT"; break;
      case -3: err = "CONNECTION_LOST"; break;
      case -2: err = "CONNECT_FAILED"; break;
      case  1: err = "BAD_PROTOCOL"; break;
      case  2: err = "BAD_CLIENT_ID"; break;
      case  3: err = "UNAVAILABLE"; break;
      case  4: err = "BAD_CREDENTIALS"; break;
      case  5: err = "NOT_AUTHORIZED"; break;
      default: err = "UNKNOWN"; break;
    }
    AddLog(LOG_LEVEL_ERROR, PSTR("BBL: Connection failed, rc=%d (%s)"), rc, err);
    bbl_state.connected = false;
    // Clean up TLS state for fresh retry
    bbl_tls_client->stop();
  }
}

static void BblSendPushAll(void) {
  if (!bbl_mqtt_client || !bbl_state.connected) return;

  char topic[64];
  snprintf(topic, sizeof(topic), "device/%s/request", bbl_settings.serial);

  const char *payload = "{\"pushing\":{\"command\":\"pushall\"}}";
  bbl_mqtt_client->publish(topic, payload);
  bbl_state.last_pushall = TasmotaGlobal.uptime;

  AddLog(LOG_LEVEL_DEBUG, PSTR("BBL: Sent pushall request"));
}

/*********************************************************************************************\
 * Loop - called every second from Xdrv110
\*********************************************************************************************/

void BblInit(void) {
  BblSettingsLoad();
  memset(&bbl_state, 0x00, sizeof(bbl_state));
  strcpy(bbl_state.gcode_state, "UNKNOWN");
}

void BblEverySecond(void) {
  if (!BblIsConfigured()) return;

  if (bbl_mqtt_client && bbl_state.connected) {
    // Process incoming messages
    if (!bbl_mqtt_client->loop()) {
      // Connection lost
      AddLog(LOG_LEVEL_INFO, PSTR("BBL: Connection lost"));
      bbl_state.connected = false;
      bbl_state.data_valid = false;
    }

    // Periodic pushall to keep data fresh
    if (bbl_state.connected &&
        (TasmotaGlobal.uptime - bbl_state.last_pushall >= BBL_PUSHALL_INTERVAL)) {
      BblSendPushAll();
    }
  } else {
    // Try to reconnect
    if (TasmotaGlobal.uptime - bbl_state.last_reconnect >= BBL_RECONNECT_INTERVAL) {
      BblConnect();
    }
  }
}

/*********************************************************************************************\
 * Web UI - Configuration page
\*********************************************************************************************/

#ifdef USE_WEBSERVER

void BblConfigPage(void) {
  if (!HttpCheckPriviledgedAccess()) { return; }

  AddLog(LOG_LEVEL_DEBUG, PSTR(D_LOG_HTTP "BBL config"));

  if (Webserver->hasArg(F("save"))) {
    char tmp[256];

    WebGetArg(PSTR("bm"), tmp, sizeof(tmp));
    bbl_settings.mode = atoi(tmp);

    WebGetArg(PSTR("bh"), tmp, sizeof(tmp));
    strlcpy(bbl_settings.host, tmp, sizeof(bbl_settings.host));

    WebGetArg(PSTR("bs"), tmp, sizeof(tmp));
    strlcpy(bbl_settings.serial, tmp, sizeof(bbl_settings.serial));

    WebGetArg(PSTR("ba"), tmp, sizeof(tmp));
    strlcpy(bbl_settings.access_code, tmp, sizeof(bbl_settings.access_code));

    WebGetArg(PSTR("br"), tmp, sizeof(tmp));
    bbl_settings.cloud_region = atoi(tmp);

    WebGetArg(PSTR("bt"), tmp, sizeof(tmp));
    strlcpy(bbl_settings.cloud_token, tmp, sizeof(bbl_settings.cloud_token));

    BblSettingsSave();

    // Force reconnect with new settings
    BblDisconnect();
    bbl_state.last_reconnect = 0;

    HandleConfiguration();
    return;
  }

  WSContentStart_P(PSTR("BambuLab Configuration"));
  WSContentSendStyle();

  // JavaScript to toggle local/cloud fields
  WSContentSend_P(PSTR(
    "<script>"
    "function bblMode(){"
      "var m=document.getElementById('bm').value;"
      "document.getElementById('local_cfg').style.display=m=='0'?'':'none';"
      "document.getElementById('cloud_cfg').style.display=m=='1'?'':'none';"
    "}"
    "</script>"
  ));

  WSContentSend_P(PSTR("<fieldset><legend><b>&nbsp;BambuLab Printer&nbsp;</b></legend>"));
  WSContentSend_P(PSTR("<form method='get' action='" WEB_HANDLE_BBL_CFG "'>"));

  // Connection mode
  WSContentSend_P(PSTR(
    "<p><b>Connection Mode</b><br>"
    "<select id='bm' name='bm' onchange='bblMode()'>"
    "<option value='%d'%s>Local (LAN)</option>"
    // BambuCloud is broken
    // "<option value='%d'%s>BambuCloud</option>"
    "</select></p>"),
    BBL_MODE_LOCAL, bbl_settings.mode == BBL_MODE_LOCAL ? " selected" : "",
    BBL_MODE_CLOUD, bbl_settings.mode == BBL_MODE_CLOUD ? " selected" : "");

  // Printer serial (common)
  WSContentSend_P(PSTR(
    "<p><b>Printer Serial</b><br>"
    "<input id='bs' name='bs' maxlength='19' value='%s'></p>"),
    bbl_settings.serial);

  // Local settings
  WSContentSend_P(PSTR("<div id='local_cfg'%s>"),
    bbl_settings.mode == BBL_MODE_LOCAL ? "" : " style='display:none'");

  WSContentSend_P(PSTR(
    "<p><b>Printer IP / Hostname</b><br>"
    "<input id='bh' name='bh' maxlength='39' value='%s'></p>"),
    bbl_settings.host);

  WSContentSend_P(PSTR(
    "<p><b>LAN Access Code</b><br>"
    "<input id='ba' name='ba' type='password' maxlength='19' value='%s'></p>"),
    bbl_settings.access_code);

  WSContentSend_P(PSTR("</div>"));

  // Cloud settings
  WSContentSend_P(PSTR("<div id='cloud_cfg'%s>"),
    bbl_settings.mode == BBL_MODE_CLOUD ? "" : " style='display:none'");

  WSContentSend_P(PSTR(
    "<p><b>Cloud Region</b><br>"
    "<select id='br' name='br'>"
    "<option value='%d'%s>US</option>"
    "<option value='%d'%s>EU</option>"
    "<option value='%d'%s>China</option>"
    "</select></p>"),
    BBL_REGION_US, bbl_settings.cloud_region == BBL_REGION_US ? " selected" : "",
    BBL_REGION_EU, bbl_settings.cloud_region == BBL_REGION_EU ? " selected" : "",
    BBL_REGION_CN, bbl_settings.cloud_region == BBL_REGION_CN ? " selected" : "");

  WSContentSend_P(PSTR(
    "<p><b>Cloud Access Token</b><br>"
    "<input id='bt' name='bt' type='password' maxlength='255' value='%s'></p>"),
    bbl_settings.cloud_token);

  WSContentSend_P(PSTR("</div>"));

  // Status indicator
  WSContentSend_P(PSTR("<hr><p>Status: <b style='color:%s'>%s</b></p>"),
    bbl_state.connected ? "green" : "red",
    !BblIsConfigured() ? "Not configured" :
    bbl_state.connected ? "Connected" : "Disconnected");

  WSContentSend_P(PSTR("<br><button name='save' type='submit' class='button bgrn'>" D_SAVE "</button>"));
  WSContentSend_P(PSTR("</form></fieldset>"));

  WSContentSpaceButton(BUTTON_CONFIGURATION);
  WSContentStop();
}

/*********************************************************************************************\
 * Web UI - Sensor display on main page
\*********************************************************************************************/

bool BblStatusWeb(void) {
  if (!bbl_state.data_valid) return false;

  // Nozzle temperature with color
  const char *temp_color = bbl_state.nozzle_temp > 200 ? "#FF4444" :
                           bbl_state.nozzle_temp > 100 ? "#FFAA00" : "#44FF44";

  WSContentSend_P(PSTR("{s}BBL Nozzle{m}<span style='color:%s'>%.0f</span> / %.0f °C{e}"),
    temp_color, bbl_state.nozzle_temp, bbl_state.nozzle_target);

  WSContentSend_P(PSTR("{s}BBL Bed{m}%.0f / %.0f °C{e}"),
    bbl_state.bed_temp, bbl_state.bed_target);

  // Print progress
  if (strcmp(bbl_state.gcode_state, "IDLE") != 0 &&
      strcmp(bbl_state.gcode_state, "UNKNOWN") != 0) {
    WSContentSend_P(PSTR("{s}BBL Progress{m}%d %%{e}"), bbl_state.print_progress);

    if (bbl_state.remaining_min > 0) {
      uint16_t hours = bbl_state.remaining_min / 60;
      uint16_t mins = bbl_state.remaining_min % 60;
      if (hours > 0) {
        WSContentSend_P(PSTR("{s}BBL Remaining{m}%dh %dm{e}"), hours, mins);
      } else {
        WSContentSend_P(PSTR("{s}BBL Remaining{m}%dm{e}"), mins);
      }
    }
  }

  WSContentSend_P(PSTR("{s}BBL Status{m}%s{e}"), bbl_state.gcode_state);

  return true;
}

#endif  // USE_WEBSERVER

/*********************************************************************************************\
 * JSON status for MQTT/teleperiod
\*********************************************************************************************/

void BblShowJson(bool append) {
  if (!bbl_state.data_valid) return;

  if (append) {
    ResponseAppend_P(PSTR(","));
  }
  ResponseAppend_P(PSTR("\"BambuLab\":{\"Nozzle\":%.1f,\"NozzleTarget\":%.1f,"
    "\"Bed\":%.1f,\"BedTarget\":%.1f,"
    "\"Progress\":%d,\"Remaining\":%d,\"State\":\"%s\"}"),
    bbl_state.nozzle_temp, bbl_state.nozzle_target,
    bbl_state.bed_temp, bbl_state.bed_target,
    bbl_state.print_progress, bbl_state.remaining_min,
    bbl_state.gcode_state);
}

#endif  // USE_GALOPED
