/*
  xdrv_110_6_galoped_printer_mgr.ino - Printer manager, factory, web UI

  https://github.com/petrows/smarthome-galoped-dekad
  Copyright (C) 2026 by Petr Golovachev <petro@petro.ws>
  License: GPLv3+
*/

#ifdef USE_GALOPED

// ─── Factory ─────────────────────────────────────────────────────────────────

static GalopedPrinter* PrinterCreate(uint8_t ptype, uint8_t slot) {
  switch (ptype) {
    case PRINTER_TYPE_BAMBULAB:  return new GalopedPrinterBBL(slot);
    case PRINTER_TYPE_OCTOPRINT: return new GalopedPrinterOctoprint(slot);
    default: return nullptr;
  }
}

// ─── INI load/save ───────────────────────────────────────────────────────────

static void PrinterLoadSlot(uint8_t slot) {
  if (galoped_printers[slot]) { galoped_printers[slot]->prtDisconnect(); delete galoped_printers[slot]; galoped_printers[slot]=nullptr; }
#ifdef USE_UFILESYS
  char fn[20]; snprintf(fn,sizeof(fn),"/printer_%d.ini",slot);
  File f = LittleFS.open(fn,"r");
  if (!f) { AddLog(LOG_LEVEL_DEBUG, PSTR("PRT: Slot %d: no cfg"), slot); return; }
  IniFile ini(f);
  char ts[16]=""; ini.getValueStr("printer","type",ts,sizeof(ts));
  uint8_t pt=PRINTER_TYPE_NONE;
  if (!strcmp(ts,"bambulab")) pt=PRINTER_TYPE_BAMBULAB;
  else if (!strcmp(ts,"octoprint")) pt=PRINTER_TYPE_OCTOPRINT;
  if (pt==PRINTER_TYPE_NONE) { f.close(); return; }
  GalopedPrinter *p = PrinterCreate(pt,slot);
  if (!p) { f.close(); return; }
  f.seek(0);
  p->prtLoadSettings(f);
  f.close();
  galoped_printers[slot] = p;
  p->prtBegin();
  AddLog(LOG_LEVEL_INFO, PSTR("PRT: Slot %d: %s"), slot, p->prtTypeName());
#endif
}

static void PrinterSaveSlot(uint8_t slot) {
#ifdef USE_UFILESYS
  char fn[20]; snprintf(fn,sizeof(fn),"/printer_%d.ini",slot);
  GalopedPrinter *p = galoped_printers[slot];
  if (!p) { LittleFS.remove(fn); return; }
  String ini; ini+="[printer]\n";
  if (p->prtType()==PRINTER_TYPE_BAMBULAB) ini+="type=bambulab\n";
  else if (p->prtType()==PRINTER_TYPE_OCTOPRINT) ini+="type=octoprint\n";
  p->prtSaveSettings(ini);
  File f = LittleFS.open(fn,"w");
  if (f) { f.print(ini); f.close(); AddLog(LOG_LEVEL_INFO, PSTR("PRT: Slot %d saved"), slot); }
#endif
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

void PrinterInit(void)        { for (uint8_t i=0;i<GALOPED_PRINTER_MAX;i++) PrinterLoadSlot(i); }
void PrinterLoop(void)        { for (uint8_t i=0;i<GALOPED_PRINTER_MAX;i++) if (galoped_printers[i]) galoped_printers[i]->prtLoop(); }
void PrinterEverySecond(void) { for (uint8_t i=0;i<GALOPED_PRINTER_MAX;i++) if (galoped_printers[i]) galoped_printers[i]->prtEverySecond(); }

static GalopedPrinter* PrinterGet(uint8_t slot) {
  return (slot<GALOPED_PRINTER_MAX) ? galoped_printers[slot] : nullptr;
}

// ─── Web UI ──────────────────────────────────────────────────────────────────

#ifdef USE_WEBSERVER

void PrinterConfigPage(void) {
  if (!HttpCheckPriviledgedAccess()) return;
  char tmp[256]; uint8_t slot=0;
  WebGetArg(PSTR("slot"),tmp,sizeof(tmp));
  if (strlen(tmp)) slot=atoi(tmp);
  if (slot>=GALOPED_PRINTER_MAX) slot=0;

  if (Webserver->hasArg(F("save"))) {
    WebGetArg(PSTR("pt"),tmp,sizeof(tmp));
    uint8_t nt=atoi(tmp);
    if (galoped_printers[slot]) { galoped_printers[slot]->prtDisconnect(); delete galoped_printers[slot]; galoped_printers[slot]=nullptr; }
    if (nt!=PRINTER_TYPE_NONE) {
      GalopedPrinter *p=PrinterCreate(nt,slot);
      if (p) { p->prtWebFormSave(); galoped_printers[slot]=p; PrinterSaveSlot(slot); p->prtBegin(); }
    } else { PrinterSaveSlot(slot); }
    HandleConfiguration(); return;
  }

  WSContentStart_P(PSTR("3D Printer Configuration"));
  WSContentSendStyle();

  // Slot tabs
  WSContentSend_P(PSTR("<div style='text-align:center;margin-bottom:10px'>"));
  for (uint8_t i=0;i<GALOPED_PRINTER_MAX;i++) {
    const char *st=(i==slot)?"background:#1fa3ec;color:white;font-weight:bold":"background:#eee;color:#333;border:1px solid #ccc";
    const char *lb=galoped_printers[i]?galoped_printers[i]->prtTypeName():"Empty";
    WSContentSend_P(PSTR("<a href='" WEB_HANDLE_PRINTER_CFG "?slot=%d' style='%s;padding:5px 15px;margin:2px;text-decoration:none;display:inline-block'>P%d: %s</a>"), i, st, i+1, lb);
  }
  WSContentSend_P(PSTR("</div>"));

  GalopedPrinter *p=galoped_printers[slot];
  uint8_t ct = p ? p->prtType() : PRINTER_TYPE_NONE;

  WSContentSend_P(PSTR("<script>function ptC(){var t=document.getElementById('pt').value;"
    "var d=document.querySelectorAll('.ptype');for(var i=0;i<d.length;i++) d[i].style.display='none';"
    "var e=document.getElementById('pt_'+t);if(e) e.style.display='';}</script>"));

  WSContentSend_P(PSTR("<fieldset><legend><b>&nbsp;Printer %d&nbsp;</b></legend>"), slot+1);
  WSContentSend_P(PSTR("<form method='get' action='" WEB_HANDLE_PRINTER_CFG "'><input type='hidden' name='slot' value='%d'>"), slot);

  WSContentSend_P(PSTR("<p><b>Type</b><br><select id='pt' name='pt' onchange='ptC()'>"
    "<option value='%d'%s>-- None --</option>"
    "<option value='%d'%s>BambuLab</option>"
    "<option value='%d'%s>OctoPrint</option></select></p>"),
    PRINTER_TYPE_NONE,      ct==PRINTER_TYPE_NONE?" selected":"",
    PRINTER_TYPE_BAMBULAB,  ct==PRINTER_TYPE_BAMBULAB?" selected":"",
    PRINTER_TYPE_OCTOPRINT, ct==PRINTER_TYPE_OCTOPRINT?" selected":"");

  // BBL fields
  WSContentSend_P(PSTR("<div id='pt_%d' class='ptype'%s>"), PRINTER_TYPE_BAMBULAB, ct==PRINTER_TYPE_BAMBULAB?"":" style='display:none'");
  if (p && ct==PRINTER_TYPE_BAMBULAB) p->prtWebFormFields();
  else { GalopedPrinterBBL d(slot); d.prtWebFormFields(); }
  WSContentSend_P(PSTR("</div>"));

  // Octo fields
  WSContentSend_P(PSTR("<div id='pt_%d' class='ptype'%s>"), PRINTER_TYPE_OCTOPRINT, ct==PRINTER_TYPE_OCTOPRINT?"":" style='display:none'");
  if (p && ct==PRINTER_TYPE_OCTOPRINT) p->prtWebFormFields();
  else { GalopedPrinterOctoprint d(slot); d.prtWebFormFields(); }
  WSContentSend_P(PSTR("</div>"));

  if (p) WSContentSend_P(PSTR("<hr><p>Status: <b style='color:%s'>%s</b></p>"),
    p->status.connected?"green":"red", p->status.connected?"Connected":"Disconnected");

  WSContentSend_P(PSTR("<br><button name='save' type='submit' class='button bgrn'>" D_SAVE "</button></form></fieldset>"));
  WSContentSpaceButton(BUTTON_CONFIGURATION);
  WSContentStop();
}

bool PrinterStatusWeb(void) {
  bool r=false;
  for (uint8_t i=0;i<GALOPED_PRINTER_MAX;i++) {
    GalopedPrinter *p=galoped_printers[i]; if (!p||!p->status.data_valid) continue; r=true;
    const char *n=p->prtTypeName();
    const char *tc=p->status.nozzle_temp>200?"#F44":p->status.nozzle_temp>100?"#FA0":"#4F4";
    WSContentSend_P(PSTR("{s}%s Nozzle{m}<span style='color:%s'>%.0f</span>/%.0f°C{e}"), n, tc, p->status.nozzle_temp, p->status.nozzle_target);
    WSContentSend_P(PSTR("{s}%s Bed{m}%.0f/%.0f°C{e}"), n, p->status.bed_temp, p->status.bed_target);
    if (p->status.state!=PRINTER_STATE_IDLE && p->status.state!=PRINTER_STATE_UNKNOWN) {
      WSContentSend_P(PSTR("{s}%s Progress{m}%d%%{e}"), n, p->status.progress);
      if (p->status.remaining_min>0) {
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
  for (uint8_t i=0;i<GALOPED_PRINTER_MAX;i++) {
    GalopedPrinter *p=galoped_printers[i]; if (!p||!p->status.data_valid) continue;
    ResponseAppend_P(PSTR(",\"%s%d\":{\"Nozzle\":%.1f,\"NozzleTarget\":%.1f,"
      "\"Bed\":%.1f,\"BedTarget\":%.1f,\"Progress\":%d,\"Remaining\":%d,\"State\":\"%s\"}"),
      p->prtTypeName(), i, p->status.nozzle_temp, p->status.nozzle_target,
      p->status.bed_temp, p->status.bed_target, p->status.progress, p->status.remaining_min, p->stateStr());
  }
}

#endif  // USE_WEBSERVER

// ─── Compatibility wrappers (slot 0 for galoped_conf.ino) ────────────────────

bool BblStatusIsValid()    { GalopedPrinter *p=PrinterGet(0); return p && p->status.data_valid; }
bool BblStatusIsRunning()  { GalopedPrinter *p=PrinterGet(0); return p && p->isRunning(); }
bool BblStatusIsFinished() { GalopedPrinter *p=PrinterGet(0); return p && p->isFinished(); }
bool BblStatusIsError()    { GalopedPrinter *p=PrinterGet(0); return !p || p->isError(); }
float BblGetNozzleTemp()   { GalopedPrinter *p=PrinterGet(0); return p ? p->status.nozzle_temp : 0; }
uint8_t BblGetProgress()   { GalopedPrinter *p=PrinterGet(0); return p ? p->status.progress : 0; }
uint8_t BblGetGCodeStatus(){ GalopedPrinter *p=PrinterGet(0); return p ? p->status.state : PRINTER_STATE_UNKNOWN; }
bool BblStatusWeb(void)    { return PrinterStatusWeb(); }

#endif  // USE_GALOPED
