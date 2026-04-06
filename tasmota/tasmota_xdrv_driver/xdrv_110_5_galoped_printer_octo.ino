/*
  xdrv_110_5_galoped_printer_octo.ino - OctoPrint printer implementation

  https://github.com/petrows/smarthome-galoped-dekad
  Copyright (C) 2026 by Petr Golovachev <petro@petro.ws>
  License: GPLv3+
*/

#ifdef USE_GALOPED

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
  ~GalopedPrinterOctoprint() override { prtDisconnect(); }

  uint8_t prtType() override { return PRINTER_TYPE_OCTOPRINT; }
  const char* prtTypeName() override { return "Octo"; }

  void prtLoadSettings(File &ini_file) override {
    IniFile ini(ini_file);
    ini.getValueStr("printer","host",_host,sizeof(_host));
    ini.getValueStr("printer","api_key",_api_key,sizeof(_api_key));
    int32_t p=OCTO_DEFAULT_PORT; ini.getValueInt("printer","port",p); _port=p;
    AddLog(LOG_LEVEL_INFO, PSTR("OCTO[%d]: host=%s:%d"), _slot, _host, _port);
  }
  void prtSaveSettings(String &out) override {
    out+="host="; out+=_host; out+="\n";
    out+="port="; out+=_port; out+="\n";
    out+="api_key="; out+=_api_key; out+="\n";
  }
  void prtBegin() override {}
  void prtDisconnect() override { status.connected=false; status.data_valid=false; }
  void prtLoop() override {}
  void prtEverySecond() override {
    if (!octoIsConfigured() || !WifiHasIP()) return;
    uint32_t iv = (_fail_count>3) ? OCTO_RECONNECT_SEC : OCTO_POLL_SEC;
    if (TasmotaGlobal.uptime - _last_poll < iv) return;
    _last_poll = TasmotaGlobal.uptime;
    octoPollPrinter(); octoPollJob();
  }
  void prtWebFormFields() override {
    WSContentSend_P(PSTR("<p><b>Host / IP</b><br><input name='oh' maxlength='39' value='%s'></p>"), _host);
    WSContentSend_P(PSTR("<p><b>Port</b><br><input name='op' type='number' value='%d'></p>"), _port);
    WSContentSend_P(PSTR("<p><b>API Key</b><br><input name='ok' type='password' maxlength='63' value='%s'></p>"), _api_key);
  }
  void prtWebFormSave() override {
    char tmp[128];
    WebGetArg(PSTR("oh"),tmp,sizeof(tmp)); strlcpy(_host,tmp,sizeof(_host));
    WebGetArg(PSTR("op"),tmp,sizeof(tmp)); _port=atoi(tmp); if (!_port) _port=OCTO_DEFAULT_PORT;
    WebGetArg(PSTR("ok"),tmp,sizeof(tmp)); strlcpy(_api_key,tmp,sizeof(_api_key));
  }

private:
  char _host[40], _api_key[64];
  uint16_t _port;
  uint32_t _last_poll;
  uint8_t _fail_count;

  bool octoIsConfigured() { return strlen(_host) && strlen(_api_key); }

  String octoHttpGet(const char *path) {
    WiFiClient client; String body;
    if (!client.connect(_host,_port)) {
      if (++_fail_count>3) { status.connected=false; status.data_valid=false; }
      return body;
    }
    client.printf("GET %s HTTP/1.1\r\nHost: %s:%d\r\nX-Api-Key: %s\r\nConnection: close\r\n\r\n",
      path, _host, _port, _api_key);
    uint32_t t0=millis();
    while (!client.available() && millis()-t0<OCTO_TIMEOUT_MS) delay(10);
    if (!client.available()) { client.stop(); _fail_count++; return body; }
    bool hdr=true;
    while (client.available()) {
      String line = client.readStringUntil('\n');
      if (hdr) {
        if (line.length()<=2) { hdr=false; continue; }
        if (line.startsWith("HTTP/") && !line.substring(9,12).equals("200")) {
          client.stop(); _fail_count++; return body;
        }
        continue;
      }
      body += line;
      if (body.length()>OCTO_BUF_SIZE) break;
    }
    client.stop(); status.connected=true; _fail_count=0;
    return body;
  }

  void octoPollPrinter() {
    String body = octoHttpGet("/api/printer"); if (!body.length()) return;
    char *b=(char*)body.c_str(); float f;
    const char *t0=strstr(b,"\"tool0\"");
    if (t0) { PrtJsonGetFloat(t0,"\"actual\"",&f); status.nozzle_temp=f; PrtJsonGetFloat(t0,"\"target\"",&f); status.nozzle_target=f; }
    const char *bd=strstr(b,"\"bed\"");
    if (bd) { PrtJsonGetFloat(bd,"\"actual\"",&f); status.bed_temp=f; PrtJsonGetFloat(bd,"\"target\"",&f); status.bed_target=f; }
    status.data_valid=true; status.last_update=TasmotaGlobal.uptime;
  }

  void octoPollJob() {
    String body = octoHttpGet("/api/job"); if (!body.length()) return;
    char *b=(char*)body.c_str(); float f; int i;
    if (PrtJsonGetFloat(b,"\"completion\"",&f)) status.progress=(uint8_t)f;
    if (PrtJsonGetInt(b,"\"printTimeLeft\"",&i)) status.remaining_min=(i>0)?(i/60):0;
    char ss[20]="";
    if (PrtJsonGetStr(b,"\"state\"",ss,sizeof(ss))) {
      if (!strcmp(ss,"Printing")) status.state=PRINTER_STATE_RUNNING;
      else if (!strcmp(ss,"Operational")) status.state=PRINTER_STATE_IDLE;
      else if (!strcmp(ss,"Paused")||!strcmp(ss,"Pausing")) status.state=PRINTER_STATE_PAUSE;
      else if (!strcmp(ss,"Finishing")) status.state=PRINTER_STATE_FINISH;
      else if (strstr(ss,"Error")) status.state=PRINTER_STATE_ERROR;
      else status.state=PRINTER_STATE_UNKNOWN;
    }
    AddLog(LOG_LEVEL_DEBUG, PSTR("OCTO[%d]: n=%.0f/%.0f b=%.0f/%.0f %d%% %s"),
      _slot, status.nozzle_temp, status.nozzle_target,
      status.bed_temp, status.bed_target, status.progress, stateStr());
  }
};

#endif  // USE_GALOPED
