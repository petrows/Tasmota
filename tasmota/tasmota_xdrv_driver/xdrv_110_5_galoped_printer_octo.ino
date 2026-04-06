/*
  xdrv_110_5_galoped_printer_octo.ino - OctoPrint printer implementation

  https://github.com/petrows/smarthome-galoped-dekad

  Copyright (C) 2026 by Petr Golovachev <petro@petro.ws>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/*
  OctoPrint REST API polling using raw WiFiClient (no HTTPClientLight
  to avoid Berry/I2C compile conflicts in Tasmota).

  Endpoints:
    GET /api/printer   -> tool0.actual, tool0.target, bed.actual, bed.target
    GET /api/job       -> progress.completion, progress.printTimeLeft, state
  Auth: X-Api-Key header
*/

#ifdef USE_GALOPED
// #ifdef USE_GALOPED_OCTO

#define OCTO_POLL_SEC        5
#define OCTO_RECONNECT_SEC   30
#define OCTO_TIMEOUT_MS      5000
#define OCTO_DEFAULT_PORT    80
#define OCTO_BUF_SIZE        1024

class GalopedPrinterOctoprint : public GalopedPrinter {
public:
  GalopedPrinterOctoprint(uint8_t slot) : GalopedPrinter(slot) {
    memset(_host, 0, sizeof(_host));
    memset(_api_key, 0, sizeof(_api_key));
    _port = OCTO_DEFAULT_PORT;
    _last_poll = 0;
    _fail_count = 0;
  }

  ~GalopedPrinterOctoprint() override { disconnect(); }

  uint8_t type() override { return PRINTER_TYPE_OCTOPRINT; }
  const char* typeName() override { return "Octo"; }

  void loadSettings(File &ini_file) override {
    IniFile ini(ini_file);
    ini.getValueStr("printer", "host", _host, sizeof(_host));
    ini.getValueStr("printer", "api_key", _api_key, sizeof(_api_key));
    int32_t port = OCTO_DEFAULT_PORT;
    ini.getValueInt("printer", "port", port);
    _port = port;
    AddLog(LOG_LEVEL_INFO, PSTR("OCTO[%d]: Loaded host=%s:%d"), _slot, _host, _port);
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
    uint32_t interval = (_fail_count > 3) ? OCTO_RECONNECT_SEC : OCTO_POLL_SEC;
    if (TasmotaGlobal.uptime - _last_poll < interval) return;
    _last_poll = TasmotaGlobal.uptime;
    pollPrinter();
    pollJob();
  }

  void webFormFields() override {
    WSContentSend_P(PSTR(
      "<p><b>Host / IP</b><br><input name='oh' maxlength='39' value='%s'></p>"), _host);
    WSContentSend_P(PSTR(
      "<p><b>Port</b><br><input name='op' type='number' value='%d'></p>"), _port);
    WSContentSend_P(PSTR(
      "<p><b>API Key</b><br><input name='ok' type='password' maxlength='63' value='%s'></p>"), _api_key);
  }

  void webFormSave() override {
    char tmp[128];
    WebGetArg(PSTR("oh"), tmp, sizeof(tmp)); strlcpy(_host, tmp, sizeof(_host));
    WebGetArg(PSTR("op"), tmp, sizeof(tmp)); _port = atoi(tmp);
    if (_port == 0) _port = OCTO_DEFAULT_PORT;
    WebGetArg(PSTR("ok"), tmp, sizeof(tmp)); strlcpy(_api_key, tmp, sizeof(_api_key));
  }

private:
  char     _host[40];
  char     _api_key[64];
  uint16_t _port;
  uint32_t _last_poll;
  uint8_t  _fail_count;

  bool isConfigured() { return strlen(_host) > 0 && strlen(_api_key) > 0; }

  // Raw HTTP GET using WiFiClient - returns body or empty string
  String httpGet(const char *path) {
    WiFiClient client;
    String body;

    if (!client.connect(_host, _port)) {
      _fail_count++;
      if (_fail_count > 3) { status.connected = false; status.data_valid = false; }
      return body;
    }

    // Send request
    client.printf("GET %s HTTP/1.1\r\n", path);
    client.printf("Host: %s:%d\r\n", _host, _port);
    client.printf("X-Api-Key: %s\r\n", _api_key);
    client.print("Connection: close\r\n\r\n");

    // Wait for response
    uint32_t start = millis();
    while (!client.available() && millis() - start < OCTO_TIMEOUT_MS) {
      delay(10);
    }
    if (!client.available()) {
      client.stop();
      _fail_count++;
      return body;
    }

    // Skip HTTP headers
    bool headers_done = false;
    while (client.available()) {
      String line = client.readStringUntil('\n');
      if (!headers_done) {
        if (line.length() <= 2) { headers_done = true; continue; }
        // Check status code in first line
        if (line.startsWith("HTTP/") && !line.substring(9, 12).equals("200")) {
          AddLog(LOG_LEVEL_DEBUG, PSTR("OCTO[%d]: HTTP error from %s"), _slot, path);
          client.stop();
          _fail_count++;
          return body;
        }
        continue;
      }
      body += line;
      // Limit body size
      if (body.length() > OCTO_BUF_SIZE) break;
    }
    client.stop();

    status.connected = true;
    _fail_count = 0;
    return body;
  }

  void pollPrinter() {
    String body = httpGet("/api/printer");
    if (body.length() == 0) return;
    char *buf = (char*)body.c_str();
    float f;

    const char *tool0 = strstr(buf, "\"tool0\"");
    if (tool0) {
      if (BblJsonGetFloat(tool0, "\"actual\"", &f)) status.nozzle_temp = f;
      if (BblJsonGetFloat(tool0, "\"target\"", &f)) status.nozzle_target = f;
    }
    const char *bed = strstr(buf, "\"bed\"");
    if (bed) {
      if (BblJsonGetFloat(bed, "\"actual\"", &f)) status.bed_temp = f;
      if (BblJsonGetFloat(bed, "\"target\"", &f)) status.bed_target = f;
    }
    status.data_valid = true;
    status.last_update = TasmotaGlobal.uptime;
  }

  void pollJob() {
    String body = httpGet("/api/job");
    if (body.length() == 0) return;
    char *buf = (char*)body.c_str();
    float f; int i;

    if (BblJsonGetFloat(buf, "\"completion\"", &f)) status.progress = (uint8_t)f;
    if (BblJsonGetInt(buf, "\"printTimeLeft\"", &i)) status.remaining_min = (i > 0) ? (i / 60) : 0;

    char state_str[20] = "";
    if (BblJsonGetStr(buf, "\"state\"", state_str, sizeof(state_str))) {
      if      (strcmp(state_str, "Printing") == 0)    status.state = PRINTER_STATE_RUNNING;
      else if (strcmp(state_str, "Operational") == 0)  status.state = PRINTER_STATE_IDLE;
      else if (strcmp(state_str, "Paused") == 0 || strcmp(state_str, "Pausing") == 0)
                                                       status.state = PRINTER_STATE_PAUSE;
      else if (strcmp(state_str, "Finishing") == 0)     status.state = PRINTER_STATE_FINISH;
      else if (strstr(state_str, "Error"))             status.state = PRINTER_STATE_ERROR;
      else                                             status.state = PRINTER_STATE_UNKNOWN;
    }

    AddLog(LOG_LEVEL_DEBUG, PSTR("OCTO[%d]: nozzle=%.0f/%.0f bed=%.0f/%.0f %d%% %s"),
      _slot, status.nozzle_temp, status.nozzle_target,
      status.bed_temp, status.bed_target, status.progress, stateStr());
  }
};

// #endif  // USE_GALOPED_OCTO
#endif  // USE_GALOPED
