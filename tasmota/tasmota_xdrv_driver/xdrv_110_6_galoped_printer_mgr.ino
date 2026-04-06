/*
  xdrv_110_6_galoped_printer_mgr.ino - Printer manager, factory, and web UI

  https://github.com/petrows/smarthome-galoped-dekad

  Copyright (C) 2026 by Petr Golovachev <petro@petro.ws>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/*
  This file MUST compile after _3, _4, _5 (all subclasses defined).
  Provides: factory, INI load/save, lifecycle, web config, web sensor, JSON.
*/

#ifdef USE_GALOPED

/*********************************************************************************************\
 * Factory - create printer by type
\*********************************************************************************************/

static GalopedPrinter* PrinterCreate(uint8_t ptype, uint8_t slot) {
  switch (ptype) {
    case PRINTER_TYPE_BAMBULAB:  return new GalopedPrinterBBL(slot);
    case PRINTER_TYPE_OCTOPRINT: return new GalopedPrinterOctoprint(slot);
    default: return nullptr;
  }
}

/*********************************************************************************************\
 * INI file load/save per printer slot
\*********************************************************************************************/

static void PrinterGetFilename(uint8_t slot, char *buf, int buf_size) {
  snprintf(buf, buf_size, "/printer_%d.ini", slot);
}

static void PrinterLoadSlot(uint8_t slot) {
  // Delete old instance
  if (galoped_printers[slot]) {
    galoped_printers[slot]->disconnect();
    delete galoped_printers[slot];
    galoped_printers[slot] = nullptr;
  }

#ifdef USE_UFILESYS
  char filename[20];
  PrinterGetFilename(slot, filename, sizeof(filename));

  File ini_file = ffsp->open(filename, "r");
  if (!ini_file) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("PRT: Slot %d: no config"), slot);
    return;
  }

  // Read printer type
  IniFile ini(ini_file);
  char type_str[16] = "";
  ini.getValueStr("printer", "type", type_str, sizeof(type_str));

  uint8_t ptype = PRINTER_TYPE_NONE;
  if (strcmp(type_str, "bambulab") == 0)       ptype = PRINTER_TYPE_BAMBULAB;
  else if (strcmp(type_str, "octoprint") == 0) ptype = PRINTER_TYPE_OCTOPRINT;

  if (ptype == PRINTER_TYPE_NONE) {
    ini_file.close();
    AddLog(LOG_LEVEL_DEBUG, PSTR("PRT: Slot %d: unknown type '%s'"), slot, type_str);
    return;
  }

  GalopedPrinter *p = PrinterCreate(ptype, slot);
  if (!p) { ini_file.close(); return; }

  // Re-seek for type-specific loading
  ini_file.seek(0);
  p->loadSettings(ini_file);
  ini_file.close();

  galoped_printers[slot] = p;
  p->begin();
  AddLog(LOG_LEVEL_INFO, PSTR("PRT: Slot %d: loaded %s"), slot, p->typeName());
#endif
}

static void PrinterSaveSlot(uint8_t slot) {
#ifdef USE_UFILESYS
  char filename[20];
  PrinterGetFilename(slot, filename, sizeof(filename));

  GalopedPrinter *p = galoped_printers[slot];
  if (!p) {
    ffsp->remove(filename);
    return;
  }

  String ini;
  ini += "[printer]\n";
  switch (p->type()) {
    case PRINTER_TYPE_BAMBULAB:  ini += "type=bambulab\n"; break;
    case PRINTER_TYPE_OCTOPRINT: ini += "type=octoprint\n"; break;
  }
  p->saveSettings(ini);

  File file = ffsp->open(filename, "w");
  if (file) {
    file.print(ini);
    file.close();
    AddLog(LOG_LEVEL_INFO, PSTR("PRT: Slot %d: saved %d bytes"), slot, ini.length());
  }
#endif
}

/*********************************************************************************************\
 * Printer manager lifecycle (called from Xdrv110)
\*********************************************************************************************/

void PrinterInit(void) {
  for (uint8_t i = 0; i < GALOPED_PRINTER_MAX; i++) {
    PrinterLoadSlot(i);
  }
}

void PrinterLoop(void) {
  for (uint8_t i = 0; i < GALOPED_PRINTER_MAX; i++) {
    if (galoped_printers[i]) galoped_printers[i]->loop();
  }
}

void PrinterEverySecond(void) {
  for (uint8_t i = 0; i < GALOPED_PRINTER_MAX; i++) {
    if (galoped_printers[i]) galoped_printers[i]->everySecond();
  }
}

GalopedPrinter* PrinterGet(uint8_t slot) {
  if (slot >= GALOPED_PRINTER_MAX) return nullptr;
  return galoped_printers[slot];
}

/*********************************************************************************************\
 * Web UI - Printer configuration page
\*********************************************************************************************/

#ifdef USE_WEBSERVER

void PrinterConfigPage(void) {
  if (!HttpCheckPriviledgedAccess()) { return; }

  char tmp[256];
  uint8_t slot = 0;
  WebGetArg(PSTR("slot"), tmp, sizeof(tmp));
  if (strlen(tmp)) slot = atoi(tmp);
  if (slot >= GALOPED_PRINTER_MAX) slot = 0;

  // Handle save
  if (Webserver->hasArg(F("save"))) {
    WebGetArg(PSTR("pt"), tmp, sizeof(tmp));
    uint8_t new_type = atoi(tmp);

    // Delete old
    if (galoped_printers[slot]) {
      galoped_printers[slot]->disconnect();
      delete galoped_printers[slot];
      galoped_printers[slot] = nullptr;
    }

    if (new_type != PRINTER_TYPE_NONE) {
      GalopedPrinter *p = PrinterCreate(new_type, slot);
      if (p) {
        p->webFormSave();
        galoped_printers[slot] = p;
        PrinterSaveSlot(slot);
        p->begin();
      }
    } else {
      PrinterSaveSlot(slot);  // Deletes file
    }

    HandleConfiguration();
    return;
  }

  WSContentStart_P(PSTR("3D Printer Configuration"));
  WSContentSendStyle();

  // Slot selector tabs
  WSContentSend_P(PSTR("<div style='text-align:center;margin-bottom:10px'>"));
  for (uint8_t i = 0; i < GALOPED_PRINTER_MAX; i++) {
    const char *style = (i == slot)
      ? "background:#1fa3ec;color:white;padding:5px 15px;margin:2px;border:none;font-weight:bold"
      : "background:#eee;color:#333;padding:5px 15px;margin:2px;border:1px solid #ccc";
    const char *label = galoped_printers[i] ? galoped_printers[i]->typeName() : "Empty";
    WSContentSend_P(PSTR("<a href='" WEB_HANDLE_PRINTER_CFG "?slot=%d' "
      "style='%s;text-decoration:none;display:inline-block'>Printer %d: %s</a>"),
      i, style, i + 1, label);
  }
  WSContentSend_P(PSTR("</div>"));

  GalopedPrinter *p = galoped_printers[slot];
  uint8_t current_type = p ? p->type() : PRINTER_TYPE_NONE;

  // JS to toggle type-specific fields
  WSContentSend_P(PSTR(
    "<script>"
    "function ptChange(){"
      "var t=document.getElementById('pt').value;"
      "var d=document.querySelectorAll('.ptype');"
      "for(var i=0;i<d.length;i++) d[i].style.display='none';"
      "var e=document.getElementById('pt_'+t);"
      "if(e) e.style.display='';"
    "}"
    "</script>"
  ));

  WSContentSend_P(PSTR("<fieldset><legend><b>&nbsp;Printer %d&nbsp;</b></legend>"), slot + 1);
  WSContentSend_P(PSTR("<form method='get' action='" WEB_HANDLE_PRINTER_CFG "'>"));
  WSContentSend_P(PSTR("<input type='hidden' name='slot' value='%d'>"), slot);

  // Type selector
  WSContentSend_P(PSTR(
    "<p><b>Printer Type</b><br>"
    "<select id='pt' name='pt' onchange='ptChange()'>"
    "<option value='%d'%s>-- None --</option>"
    "<option value='%d'%s>BambuLab</option>"
    "<option value='%d'%s>OctoPrint</option>"
    "</select></p>"),
    PRINTER_TYPE_NONE,      current_type == PRINTER_TYPE_NONE      ? " selected" : "",
    PRINTER_TYPE_BAMBULAB,  current_type == PRINTER_TYPE_BAMBULAB  ? " selected" : "",
    PRINTER_TYPE_OCTOPRINT, current_type == PRINTER_TYPE_OCTOPRINT ? " selected" : "");

  // BambuLab fields
  WSContentSend_P(PSTR("<div id='pt_%d' class='ptype'%s>"),
    PRINTER_TYPE_BAMBULAB,
    current_type == PRINTER_TYPE_BAMBULAB ? "" : " style='display:none'");
  if (p && p->type() == PRINTER_TYPE_BAMBULAB) {
    p->webFormFields();
  } else {
    GalopedPrinterBBL dummy(slot);
    dummy.webFormFields();
  }
  WSContentSend_P(PSTR("</div>"));

  // OctoPrint fields
  WSContentSend_P(PSTR("<div id='pt_%d' class='ptype'%s>"),
    PRINTER_TYPE_OCTOPRINT,
    current_type == PRINTER_TYPE_OCTOPRINT ? "" : " style='display:none'");
  if (p && p->type() == PRINTER_TYPE_OCTOPRINT) {
    p->webFormFields();
  } else {
    GalopedPrinterOctoprint dummy(slot);
    dummy.webFormFields();
  }
  WSContentSend_P(PSTR("</div>"));

  // Status
  if (p) {
    WSContentSend_P(PSTR("<hr><p>Status: <b style='color:%s'>%s</b></p>"),
      p->status.connected ? "green" : "red",
      p->status.connected ? "Connected" : "Disconnected");
  }

  WSContentSend_P(PSTR("<br><button name='save' type='submit' class='button bgrn'>" D_SAVE "</button>"));
  WSContentSend_P(PSTR("</form></fieldset>"));

  WSContentSpaceButton(BUTTON_CONFIGURATION);
  WSContentStop();
}

/*********************************************************************************************\
 * Web sensor display + JSON for all printers
\*********************************************************************************************/

bool PrinterStatusWeb(void) {
  bool result = false;
  for (uint8_t i = 0; i < GALOPED_PRINTER_MAX; i++) {
    GalopedPrinter *p = galoped_printers[i];
    if (!p || !p->status.data_valid) continue;
    result = true;

    const char *name = p->typeName();
    const char *tc = p->status.nozzle_temp > 200 ? "#FF4444" :
                     p->status.nozzle_temp > 100 ? "#FFAA00" : "#44FF44";

    WSContentSend_P(PSTR("{s}%s Nozzle{m}<span style='color:%s'>%.0f</span>/%.0f°C{e}"),
      name, tc, p->status.nozzle_temp, p->status.nozzle_target);
    WSContentSend_P(PSTR("{s}%s Bed{m}%.0f/%.0f°C{e}"),
      name, p->status.bed_temp, p->status.bed_target);

    if (p->status.state != PRINTER_STATE_IDLE && p->status.state != PRINTER_STATE_UNKNOWN) {
      WSContentSend_P(PSTR("{s}%s Progress{m}%d%%{e}"), name, p->status.progress);
      if (p->status.remaining_min > 0) {
        uint16_t h = p->status.remaining_min / 60;
        uint16_t m = p->status.remaining_min % 60;
        if (h > 0) WSContentSend_P(PSTR("{s}%s ETA{m}%dh%dm{e}"), name, h, m);
        else       WSContentSend_P(PSTR("{s}%s ETA{m}%dm{e}"), name, m);
      }
    }
    WSContentSend_P(PSTR("{s}%s Status{m}%s{e}"), name, p->stateStr());
  }
  return result;
}

void PrinterShowJson(void) {
  for (uint8_t i = 0; i < GALOPED_PRINTER_MAX; i++) {
    GalopedPrinter *p = galoped_printers[i];
    if (!p || !p->status.data_valid) continue;
    ResponseAppend_P(PSTR(",\"%s%d\":{\"Nozzle\":%.1f,\"NozzleTarget\":%.1f,"
      "\"Bed\":%.1f,\"BedTarget\":%.1f,"
      "\"Progress\":%d,\"Remaining\":%d,\"State\":\"%s\"}"),
      p->typeName(), i,
      p->status.nozzle_temp, p->status.nozzle_target,
      p->status.bed_temp, p->status.bed_target,
      p->status.progress, p->status.remaining_min,
      p->stateStr());
  }
}

#endif  // USE_WEBSERVER

/*********************************************************************************************\
 * Compatibility wrappers - access primary printer (slot 0)
 * Used by galoped_conf.ino for gauge/LED integration
\*********************************************************************************************/

bool BblStatusIsValid() {
  GalopedPrinter *p = PrinterGet(0);
  return p && p->status.data_valid;
}

bool BblStatusIsRunning() {
  GalopedPrinter *p = PrinterGet(0);
  return p && p->isRunning();
}

bool BblStatusIsFinished() {
  GalopedPrinter *p = PrinterGet(0);
  return p && p->isFinished();
}

bool BblStatusIsError() {
  GalopedPrinter *p = PrinterGet(0);
  return !p || p->isError();
}

float BblGetNozzleTemp() {
  GalopedPrinter *p = PrinterGet(0);
  return p ? p->status.nozzle_temp : 0;
}

uint8_t BblGetProgress() {
  GalopedPrinter *p = PrinterGet(0);
  return p ? p->status.progress : 0;
}

uint8_t BblGetGCodeStatus() {
  GalopedPrinter *p = PrinterGet(0);
  return p ? p->status.state : PRINTER_STATE_UNKNOWN;
}

// Compatibility: BblStatusWeb is now PrinterStatusWeb
bool BblStatusWeb(void) {
  return PrinterStatusWeb();
}

#endif  // USE_GALOPED
