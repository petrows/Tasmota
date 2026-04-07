/*
  xdrv_110_5_galoped_printer_octo.ino - OctoPrint printer implementation

  https://github.com/petrows/smarthome-galoped-dekad
  Copyright (C) 2026 by Petr Golovachev <petro@petro.ws>
  License: GPLv3+

  Polls OctoPrint REST API via raw WiFiClient / WiFiClientSecure.
  Supports both HTTP and HTTPS connections.

  Endpoints used:
    GET /api/printer  -> tool0 + bed temperatures
    GET /api/job      -> progress, remaining time, state
  Authentication via X-Api-Key header.
*/

#ifdef USE_GALOPED

// Polling intervals (seconds)
#define OCTO_POLL_SEC       5    // Normal polling interval
#define OCTO_RECONNECT_SEC  30   // Backoff interval after repeated failures

// HTTP parameters
#define OCTO_TIMEOUT_MS     5000   // HTTP request timeout
#define OCTO_DEFAULT_PORT   80
#define OCTO_DEFAULT_TLS_PORT 443
#define OCTO_BUF_SIZE       1024   // Max response body size to read

/*********************************************************************************************\
 * OctoPrint printer class
\*********************************************************************************************/

class GalopedPrinterOctoprint : public GalopedPrinter {
public:
  GalopedPrinterOctoprint(uint8_t slot) : GalopedPrinter(slot),
    _port(OCTO_DEFAULT_PORT),
    _use_tls(false),
    _last_poll(0),
    _fail_count(0),
    _tls(nullptr)
  {
    memset(_host, 0, sizeof(_host));
    memset(_api_key, 0, sizeof(_api_key));
  }

  ~GalopedPrinterOctoprint() override {
    prtDisconnect();
    if (_tls) {
      delete _tls;
      _tls = nullptr;
    }
  }

  uint8_t prtType() override { return PRINTER_TYPE_OCTOPRINT; }
  const char* prtTypeName() override { return "Octo"; }

  /*********************************************************************************************\
   * Settings
  \*********************************************************************************************/

  void prtLoadSettings(File &ini_file) override {
    IniFile ini(ini_file);

    ini.getValueStr("printer", "host", _host, sizeof(_host));
    ini.getValueStr("printer", "api_key", _api_key, sizeof(_api_key));

    int32_t port = OCTO_DEFAULT_PORT;
    ini.getValueInt("printer", "port", port);
    _port = port;

    bool tls = false;
    ini.getValueBool("printer", "tls", tls);
    _use_tls = tls;

    AddLog(LOG_LEVEL_INFO, PSTR("OCTO[%d]: host=%s:%d tls=%d"),
           _slot, _host, _port, _use_tls);
  }

  void prtSaveSettings(String &out) override {
    out += "host=";    out += _host;                   out += "\n";
    out += "port=";    out += _port;                   out += "\n";
    out += "api_key="; out += _api_key;                out += "\n";
    out += "tls=";     out += (_use_tls ? "true" : "false"); out += "\n";
  }

  /*********************************************************************************************\
   * Lifecycle
  \*********************************************************************************************/

  void prtBegin() override {
    // Polling starts in prtEverySecond()
  }

  void prtDisconnect() override {
    status.connected = false;
    status.data_valid = false;
  }

  void prtLoop() override {
    // HTTP polling is handled in prtEverySecond() only
  }

  void prtEverySecond() override {
    if (!octoIsConfigured() || !WifiHasIP()) return;

    // Increase interval after repeated failures
    uint32_t interval = (_fail_count > 3) ? OCTO_RECONNECT_SEC : OCTO_POLL_SEC;
    if (TasmotaGlobal.uptime - _last_poll < interval) return;

    _last_poll = TasmotaGlobal.uptime;

    // Poll both endpoints
    octoPollPrinter();
    octoPollJob();
  }

  /*********************************************************************************************\
   * Web UI form fields
  \*********************************************************************************************/

  void prtWebFormFields() override {
    WSContentSend_P(PSTR(
      "<p><b>Host / IP</b><br>"
      "<input name='oh' maxlength='39' value='%s'></p>"), _host);

    WSContentSend_P(PSTR(
      "<p><b>Port</b><br>"
      "<input name='op' type='number' value='%d'></p>"), _port);

    WSContentSend_P(PSTR(
      "<p><b>API Key</b><br>"
      "<input name='ok' type='password' maxlength='63' value='%s'></p>"), _api_key);

    // HTTPS checkbox
    WSContentSend_P(PSTR(
      "<p><input type='checkbox' name='ot' value='1'%s>"
      " <b>Use HTTPS</b></p>"),
      _use_tls ? " checked" : "");
  }

  void prtWebFormSave() override {
    char tmp[128];

    WebGetArg(PSTR("oh"), tmp, sizeof(tmp));
    strlcpy(_host, tmp, sizeof(_host));

    WebGetArg(PSTR("op"), tmp, sizeof(tmp));
    _port = atoi(tmp);

    WebGetArg(PSTR("ok"), tmp, sizeof(tmp));
    strlcpy(_api_key, tmp, sizeof(_api_key));

    // Checkbox: present in form data only when checked
    WebGetArg(PSTR("ot"), tmp, sizeof(tmp));
    _use_tls = (strlen(tmp) > 0);

    // Set default port based on TLS setting
    if (!_port) {
      _port = _use_tls ? OCTO_DEFAULT_TLS_PORT : OCTO_DEFAULT_PORT;
    }
  }

private:
  // Settings
  char     _host[40];
  char     _api_key[64];
  uint16_t _port;
  bool     _use_tls;

  // State
  uint32_t _last_poll;
  uint8_t  _fail_count;

  // TLS client (allocated on first use, reused across requests)
  BearSSL::WiFiClientSecure_light *_tls;

  /*********************************************************************************************\
   * Internal helpers
  \*********************************************************************************************/

  bool octoIsConfigured() {
    return strlen(_host) > 0 && strlen(_api_key) > 0;
  }

  // Connect a WiFiClient (plain or TLS) and return it
  // Caller must call client->stop() when done
  Client* octoConnect() {
    if (_use_tls) {
      // Allocate TLS client on first use
      if (!_tls) {
        _tls = new BearSSL::WiFiClientSecure_light(2048, 2048);
        if (!_tls) return nullptr;
      }
      _tls->setInsecure();  // Skip cert verification (self-signed OK)

      if (!_tls->connect(_host, _port)) {
        return nullptr;
      }
      return _tls;
    } else {
      // Plain HTTP - use stack-allocated WiFiClient
      // We need a persistent client, so use a static one
      static WiFiClient plain_client;
      if (!plain_client.connect(_host, _port)) {
        return nullptr;
      }
      return &plain_client;
    }
  }

  // HTTP(S) GET - returns response body or empty string on error
  String octoHttpGet(const char *path) {
    String body;

    Client *client = octoConnect();
    if (!client) {
      _fail_count++;
      if (_fail_count > 3) {
        status.connected = false;
        status.data_valid = false;
      }
      return body;
    }

    // Send HTTP request
    client->printf("GET %s HTTP/1.1\r\n", path);
    client->printf("Host: %s:%d\r\n", _host, _port);
    client->printf("X-Api-Key: %s\r\n", _api_key);
    client->print("Connection: close\r\n\r\n");

    // Wait for response
    uint32_t start = millis();
    while (!client->available() && millis() - start < OCTO_TIMEOUT_MS) {
      delay(10);
    }

    if (!client->available()) {
      client->stop();
      _fail_count++;
      return body;
    }

    // Parse response: skip headers, read body
    bool in_headers = true;
    while (client->available()) {
      String line = client->readStringUntil('\n');

      if (in_headers) {
        // Empty line marks end of headers
        if (line.length() <= 2) {
          in_headers = false;
          continue;
        }

        // Check HTTP status code in first line
        if (line.startsWith("HTTP/") && !line.substring(9, 12).equals("200")) {
          client->stop();
          _fail_count++;
          return body;
        }
        continue;
      }

      // Accumulate body
      body += line;
      if (body.length() > OCTO_BUF_SIZE) break;
    }

    client->stop();
    status.connected = true;
    _fail_count = 0;
    return body;
  }

  // Poll GET /api/printer -> nozzle and bed temperatures
  void octoPollPrinter() {
    String body = octoHttpGet("/api/printer");
    if (!body.length()) return;

    char *buf = (char *)body.c_str();
    float f;

    // tool0 temperatures: "tool0":{"actual":210.0,"target":210.0,...}
    const char *tool0 = strstr(buf, "\"tool0\"");
    if (tool0) {
      if (PrtJsonGetFloat(tool0, "\"actual\"", &f))  status.nozzle_temp = f;
      if (PrtJsonGetFloat(tool0, "\"target\"", &f))  status.nozzle_target = f;
    }

    // bed temperatures: "bed":{"actual":60.0,"target":60.0,...}
    const char *bed = strstr(buf, "\"bed\"");
    if (bed) {
      if (PrtJsonGetFloat(bed, "\"actual\"", &f))  status.bed_temp = f;
      if (PrtJsonGetFloat(bed, "\"target\"", &f))  status.bed_target = f;
    }

    status.data_valid = true;
    status.last_update = TasmotaGlobal.uptime;
  }

  // Poll GET /api/job -> progress, remaining time, state
  void octoPollJob() {
    String body = octoHttpGet("/api/job");
    if (!body.length()) return;

    char *buf = (char *)body.c_str();
    float f;
    int i;

    // Print progress: "completion": 45.2
    if (PrtJsonGetFloat(buf, "\"completion\"", &f)) {
      status.progress = (uint8_t)f;
    }

    // Remaining time in seconds: "printTimeLeft": 1234
    if (PrtJsonGetInt(buf, "\"printTimeLeft\"", &i)) {
      status.remaining_min = (i > 0) ? (i / 60) : 0;
    }

    // Printer state: "state": "Printing" / "Operational" / "Paused" / etc.
    char state_str[20] = "";
    if (PrtJsonGetStr(buf, "\"state\"", state_str, sizeof(state_str))) {
      if (!strcmp(state_str, "Printing")) {
        status.state = PRINTER_STATE_RUNNING;
      } else if (!strcmp(state_str, "Operational")) {
        status.state = PRINTER_STATE_IDLE;
      } else if (!strcmp(state_str, "Paused") || !strcmp(state_str, "Pausing")) {
        status.state = PRINTER_STATE_PAUSE;
      } else if (!strcmp(state_str, "Finishing")) {
        status.state = PRINTER_STATE_FINISH;
      } else if (strstr(state_str, "Error")) {
        status.state = PRINTER_STATE_ERROR;
      } else {
        status.state = PRINTER_STATE_UNKNOWN;
      }
    }

    AddLog(LOG_LEVEL_DEBUG,
           PSTR("OCTO[%d]: nozzle=%.0f/%.0f bed=%.0f/%.0f %d%% %s"),
           _slot, status.nozzle_temp, status.nozzle_target,
           status.bed_temp, status.bed_target,
           status.progress, stateStr());
  }
};

#endif  // USE_GALOPED
