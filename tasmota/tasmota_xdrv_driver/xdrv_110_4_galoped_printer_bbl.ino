/*
  xdrv_110_4_galoped_printer_bbl.ino - BambuLab printer implementation

  https://github.com/petrows/smarthome-galoped-dekad
  Copyright (C) 2026 by Petr Golovachev <petro@petro.ws>
  License: GPLv3+
*/

#ifdef USE_GALOPED

#include <PubSubClient.h>

#define BBL_MQTT_PORT      8883
#define BBL_MQTT_KEEPALIVE 30
#define BBL_MQTT_BUF_SIZE  (32*1024)
#define BBL_RECONNECT_SEC  30
#define BBL_PUSHALL_SEC    60
#define BBL_CLOUD_HOST_US  "us.mqtt.bambulab.com"
#define BBL_CLOUD_HOST_CN  "cn.mqtt.bambulab.com"
#define BBL_MODE_LOCAL  0
#define BBL_MODE_CLOUD  1
#define BBL_REGION_US   0
#define BBL_REGION_EU   1
#define BBL_REGION_CN   2

static int BblBase64UrlDecode(const char *src, int src_len, char *dst, int dst_max) {
  char *tmp = (char*)malloc(src_len + 4);
  if (!tmp) return -1;
  for (int i = 0; i < src_len; i++) {
    if (src[i]=='-') tmp[i]='+'; else if (src[i]=='_') tmp[i]='/'; else tmp[i]=src[i];
  }
  int pad = (4 - (src_len % 4)) % 4;
  for (int i = 0; i < pad; i++) tmp[src_len+i] = '=';
  tmp[src_len+pad] = '\0';
  int dl = decode_base64((unsigned char*)tmp, (unsigned char*)dst);
  free(tmp);
  if (dl >= dst_max) dl = dst_max - 1;
  dst[dl] = '\0';
  return dl;
}

static bool BblExtractUidFromToken(const char *token, char *buf, int buf_size) {
  const char *d1 = strchr(token,'.'); if (!d1) return false;
  const char *ps = d1+1;
  const char *d2 = strchr(ps,'.'); if (!d2) return false;
  int pl = d2-ps; if (pl>1024) return false;
  char *dec = (char*)malloc(pl+4); if (!dec) return false;
  if (BblBase64UrlDecode(ps,pl,dec,pl+4)<=0) { free(dec); return false; }
  JsonParser parser(dec);
  JsonParserObject root = parser.getRootObject();
  bool found = false;
  if (root) {
    static const char *keys[] = {"uid","sub","user_id"};
    for (uint32_t i=0; i<3 && !found; i++) {
      JsonParserToken t = root[keys[i]];
      if (t) {
        const char *s = t.getStr();
        if (s && strlen(s)) { snprintf(buf,buf_size,"u_%s",s); found=true; }
        else if (t.isInt()) { snprintf(buf,buf_size,"u_%d",t.getInt()); found=true; }
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
    memset(_host,0,sizeof(_host)); memset(_serial,0,sizeof(_serial));
    memset(_access_code,0,sizeof(_access_code)); memset(_cloud_token,0,sizeof(_cloud_token));
  }
  ~GalopedPrinterBBL() override { prtDisconnect(); delete _mqtt; _mqtt=nullptr; delete _tls; _tls=nullptr; }

  uint8_t prtType() override { return PRINTER_TYPE_BAMBULAB; }
  const char* prtTypeName() override { return "BBL"; }

  void prtLoadSettings(File &ini_file) override {
    IniFile ini(ini_file); int32_t v;
    if (ini.getValueInt("printer","mode",v)) _mode=v;
    if (ini.getValueInt("printer","cloud_region",v)) _region=v;
    ini.getValueStr("printer","host",_host,sizeof(_host));
    ini.getValueStr("printer","serial",_serial,sizeof(_serial));
    ini.getValueStr("printer","access_code",_access_code,sizeof(_access_code));
    ini.getValueStr("printer","cloud_token",_cloud_token,sizeof(_cloud_token));
    AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: host=%s serial=%s mode=%d"), _slot, _host, _serial, _mode);
  }
  void prtSaveSettings(String &out) override {
    out+="mode="; out+=_mode; out+="\n";
    out+="host="; out+=_host; out+="\n";
    out+="serial="; out+=_serial; out+="\n";
    out+="access_code="; out+=_access_code; out+="\n";
    out+="cloud_region="; out+=_region; out+="\n";
    out+="cloud_token="; out+=_cloud_token; out+="\n";
  }
  void prtBegin() override {}
  void prtDisconnect() override {
    if (_mqtt) _mqtt->disconnect();
    if (_tls) _tls->stop();
    status.connected = false;
  }
  void prtLoop() override {
    if (_mqtt && status.connected && !_mqtt->loop()) {
      AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: Connection lost"), _slot);
      status.connected=false; status.data_valid=false;
      if (_tls) _tls->stop();
    }
  }
  void prtEverySecond() override {
    if (!bblIsConfigured()) return;
    if (_mqtt && status.connected) {
      if (TasmotaGlobal.uptime - _last_pushall >= BBL_PUSHALL_SEC) bblSendPushAll();
    } else if (TasmotaGlobal.uptime - _last_reconnect >= BBL_RECONNECT_SEC) {
      bblDoConnect();
    }
  }
  void prtWebFormFields() override {
    WSContentSend_P(PSTR("<p><b>Serial</b><br><input name='bs' maxlength='19' value='%s'></p>"), _serial);
    WSContentSend_P(PSTR("<p><b>Host / IP</b><br><input name='bh' maxlength='39' value='%s'></p>"), _host);
    WSContentSend_P(PSTR("<p><b>LAN Access Code</b><br><input name='ba' type='password' maxlength='19' value='%s'></p>"), _access_code);
  }
  void prtWebFormSave() override {
    char tmp[256];
    WebGetArg(PSTR("bs"),tmp,sizeof(tmp)); strlcpy(_serial,tmp,sizeof(_serial));
    WebGetArg(PSTR("bh"),tmp,sizeof(tmp)); strlcpy(_host,tmp,sizeof(_host));
    WebGetArg(PSTR("ba"),tmp,sizeof(tmp)); strlcpy(_access_code,tmp,sizeof(_access_code));
    _mode = BBL_MODE_LOCAL;
  }

private:
  uint8_t _mode, _region;
  char _host[40], _serial[20], _access_code[20], _cloud_token[256];
  BearSSL::WiFiClientSecure_light *_tls;
  PubSubClient *_mqtt;
  uint32_t _last_reconnect, _last_pushall;

  bool bblIsConfigured() {
    return (_mode==BBL_MODE_LOCAL)
      ? (strlen(_host) && strlen(_serial) && strlen(_access_code))
      : (strlen(_serial) && strlen(_cloud_token));
  }

  static void bblMqttCbStatic(char *topic, uint8_t *payload, unsigned int length) {
    for (uint8_t i=0; i<GALOPED_PRINTER_MAX; i++) {
      GalopedPrinter *p = galoped_printers[i];
      if (p && p->prtType()==PRINTER_TYPE_BAMBULAB) {
        GalopedPrinterBBL *b = static_cast<GalopedPrinterBBL*>(p);
        if (strstr(topic, b->_serial)) { b->bblOnMessage(payload,length); return; }
      }
    }
  }

  void bblOnMessage(uint8_t *payload, unsigned int length) {
    if (length<10) return;
    char saved = ((char*)payload)[length];
    ((char*)payload)[length] = '\0';
    const char *j = (const char*)payload;
    if (!strstr(j,"\"print\"")) { ((char*)payload)[length]=saved; return; }
    float f; int i; bool upd=false;
    if (PrtJsonGetFloat(j,"\"nozzle_temper\"",&f))        { status.nozzle_temp=f; upd=true; }
    if (PrtJsonGetFloat(j,"\"nozzle_target_temper\"",&f)) { status.nozzle_target=f; upd=true; }
    if (PrtJsonGetFloat(j,"\"bed_temper\"",&f))           { status.bed_temp=f; upd=true; }
    if (PrtJsonGetFloat(j,"\"bed_target_temper\"",&f))    { status.bed_target=f; upd=true; }
    if (PrtJsonGetInt(j,"\"mc_percent\"",&i))             { status.progress=i; upd=true; }
    if (PrtJsonGetInt(j,"\"mc_remaining_time\"",&i))      { status.remaining_min=i; upd=true; }
    char ss[16]="";
    if (PrtJsonGetStr(j,"\"gcode_state\"",ss,sizeof(ss))) {
      if (!strcmp(ss,"IDLE")) status.state=PRINTER_STATE_IDLE;
      else if (!strcmp(ss,"RUNNING")) status.state=PRINTER_STATE_RUNNING;
      else if (!strcmp(ss,"PAUSE")) status.state=PRINTER_STATE_PAUSE;
      else if (!strcmp(ss,"FINISH")) status.state=PRINTER_STATE_FINISH;
      else status.state=PRINTER_STATE_UNKNOWN;
      upd=true;
    }
    ((char*)payload)[length]=saved;
    if (upd) {
      status.data_valid=true; status.last_update=TasmotaGlobal.uptime;
      AddLog(LOG_LEVEL_DEBUG, PSTR("BBL[%d]: n=%.0f/%.0f b=%.0f/%.0f %d%% %s"),
        _slot, status.nozzle_temp, status.nozzle_target,
        status.bed_temp, status.bed_target, status.progress, stateStr());
    }
  }

  void bblDoConnect() {
    if (!WifiHasIP()) return;
    _last_reconnect = TasmotaGlobal.uptime;
    if (_tls) _tls->stop();
    if (!_tls) { _tls = new BearSSL::WiFiClientSecure_light(4096,4096); if (!_tls) return; }
    _tls->setInsecure();
    if (!_mqtt) {
      _mqtt = new PubSubClient();
      _mqtt->setClient(*_tls); _mqtt->setBufferSize(BBL_MQTT_BUF_SIZE);
      _mqtt->setKeepAlive(BBL_MQTT_KEEPALIVE); _mqtt->setCallback(bblMqttCbStatic);
    }
    const char *host,*user,*pass; char cid[64]; static char uid[48];
    if (_mode==BBL_MODE_LOCAL) { host=_host; user="bblp"; pass=_access_code; }
    else {
      host = (_region==BBL_REGION_CN) ? BBL_CLOUD_HOST_CN : BBL_CLOUD_HOST_US;
      if (!BblExtractUidFromToken(_cloud_token,uid,sizeof(uid))) {
        AddLog(LOG_LEVEL_ERROR, PSTR("BBL[%d]: JWT fail"), _slot); return;
      }
      user=uid; pass=_cloud_token;
    }
    snprintf(cid,sizeof(cid),"bblp_%08X_%d",ESP_getChipId(),_slot);
    AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: -> %s:%d user=%s"), _slot, host, BBL_MQTT_PORT, user);
    IPAddress ip;
    if (!WifiHostByName(host,ip)) { AddLog(LOG_LEVEL_ERROR, PSTR("BBL[%d]: DNS fail"), _slot); return; }
    _mqtt->setServer(ip,BBL_MQTT_PORT);
    if (_mqtt->connect(cid,user,pass)) {
      status.connected=true;
      char t[64]; snprintf(t,sizeof(t),"device/%s/report",_serial);
      _mqtt->subscribe(t);
      AddLog(LOG_LEVEL_INFO, PSTR("BBL[%d]: OK sub %s"), _slot, t);
      bblSendPushAll();
    } else {
      AddLog(LOG_LEVEL_ERROR, PSTR("BBL[%d]: fail rc=%d"), _slot, _mqtt->state());
      status.connected=false; _tls->stop();
    }
  }

  void bblSendPushAll() {
    if (!_mqtt || !status.connected) return;
    char t[64]; snprintf(t,sizeof(t),"device/%s/request",_serial);
    _mqtt->publish(t,"{\"pushing\":{\"command\":\"pushall\"}}");
    _last_pushall = TasmotaGlobal.uptime;
  }
};

#endif  // USE_GALOPED
