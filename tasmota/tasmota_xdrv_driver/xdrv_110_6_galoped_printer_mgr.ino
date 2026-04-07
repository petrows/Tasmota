/*
  xdrv_110_6_galoped_printer_mgr.ino - Printer manager, factory, web UI

  https://github.com/petrows/smarthome-galoped-dekad
  Copyright (C) 2026 by Petr Golovachev <petro@petro.ws>
  License: GPLv3+

  Provides:
  - Factory to create printer instances by type
  - INI file persistence (one file per slot: /printer_0.ini, /printer_1.ini)
  - Lifecycle management for all printer slots
  - Unified web configuration page with slot tabs
  - Web sensor display and JSON output
  - Compatibility wrappers for galoped_conf.ino (slot 0)
*/

#ifdef USE_GALOPED

struct GalopedPrinter;

/*********************************************************************************************\
 * Factory - create printer instance by type
\*********************************************************************************************/

/**
 * Create a new printer object.
 * Returns void* due to Arduino .ino forward-declaration limitations
 * (GalopedPrinter* would trigger broken prototype generation).
 */
static void* PrinterCreate(uint8_t ptype, uint8_t slot) {
  switch (ptype) {
    case PRINTER_TYPE_BAMBULAB:
      return new GalopedPrinterBBL(slot);
    case PRINTER_TYPE_OCTOPRINT:
      return new GalopedPrinterOctoprint(slot);
    default:
      return nullptr;
  }
}

/*********************************************************************************************\
 * INI file load/save
 *
 * Each printer slot is stored as /printer_N.ini on LittleFS.
 * Format:
 *   [printer]
 *   type=bambulab
 *   host=192.168.1.100
 *   serial=XXXXXXXXXXXX
 *   access_code=12345678
 *   ...
\*********************************************************************************************/

static void PrinterLoadSlot(uint8_t slot) {
  // Clean up existing instance
  if (galoped_printers[slot]) {
    galoped_printers[slot]->prtDisconnect();
    delete galoped_printers[slot];
    galoped_printers[slot] = nullptr;
  }

#ifdef USE_UFILESYS
  // Build filename
  char filename[20];
  snprintf(filename, sizeof(filename), "/printer_%d.ini", slot);

  // Open INI file
  File file = LittleFS.open(filename, "r");
  if (!file) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("PRT: Slot %d: no config file"), slot);
    return;
  }

  // Read printer type
  IniFile ini(file);
  char type_str[16] = "";
  ini.getValueStr("printer", "type", type_str, sizeof(type_str));

  uint8_t ptype = PRINTER_TYPE_NONE;
  if (!strcmp(type_str, "bambulab"))       ptype = PRINTER_TYPE_BAMBULAB;
  else if (!strcmp(type_str, "octoprint")) ptype = PRINTER_TYPE_OCTOPRINT;

  if (ptype == PRINTER_TYPE_NONE) {
    file.close();
    return;
  }

  // Create printer instance and load its settings
  GalopedPrinter *printer = (GalopedPrinter *)PrinterCreate(ptype, slot);
  if (!printer) {
    file.close();
    return;
  }

  // Re-seek to beginning for type-specific settings
  file.seek(0);
  printer->prtLoadSettings(file);
  file.close();

  // Register and start
  galoped_printers[slot] = printer;
  printer->prtBegin();

  AddLog(LOG_LEVEL_INFO, PSTR("PRT: Slot %d: loaded %s printer"),
         slot, printer->prtTypeName());
#endif
}

static void PrinterSaveSlot(uint8_t slot) {
#ifdef USE_UFILESYS
  char filename[20];
  snprintf(filename, sizeof(filename), "/printer_%d.ini", slot);

  GalopedPrinter *printer = galoped_printers[slot];

  // No printer in this slot - remove config file
  if (!printer) {
    LittleFS.remove(filename);
    return;
  }

  // Build INI content
  String ini;
  ini += "[printer]\n";

  // Write type
  if (printer->prtType() == PRINTER_TYPE_BAMBULAB) {
    ini += "type=bambulab\n";
  } else if (printer->prtType() == PRINTER_TYPE_OCTOPRINT) {
    ini += "type=octoprint\n";
  }

  // Append type-specific settings
  printer->prtSaveSettings(ini);

  // Write to file
  File file = LittleFS.open(filename, "w");
  if (file) {
    file.print(ini);
    file.close();
    AddLog(LOG_LEVEL_INFO, PSTR("PRT: Slot %d: saved (%d bytes)"), slot, ini.length());
  }
#endif
}

/*********************************************************************************************\
 * Lifecycle functions (called from Xdrv110)
\*********************************************************************************************/

void PrinterInit(void) {
  for (uint8_t i = 0; i < GALOPED_PRINTER_MAX; i++) {
    PrinterLoadSlot(i);
  }
}

void PrinterLoop(void) {
  for (uint8_t i = 0; i < GALOPED_PRINTER_MAX; i++) {
    if (galoped_printers[i]) {
      galoped_printers[i]->prtLoop();
    }
  }
}

void PrinterEverySecond(void) {
  for (uint8_t i = 0; i < GALOPED_PRINTER_MAX; i++) {
    if (galoped_printers[i]) {
      galoped_printers[i]->prtEverySecond();
    }
  }
}

// Get printer instance by slot index (internal use)
static void* PrinterGet(uint8_t slot) {
  if (slot >= GALOPED_PRINTER_MAX) return nullptr;
  return galoped_printers[slot];
}

/*********************************************************************************************\
 * Web UI - Printer configuration page
\*********************************************************************************************/

#ifdef USE_WEBSERVER

void PrinterConfigPage(void) {
  if (!HttpCheckPriviledgedAccess()) return;

  // Determine which slot to edit
  char tmp[256];
  uint8_t slot = 0;

  WebGetArg(PSTR("slot"), tmp, sizeof(tmp));
  if (strlen(tmp)) {
    slot = atoi(tmp);
  }
  if (slot >= GALOPED_PRINTER_MAX) {
    slot = 0;
  }

  // Handle form save
  if (Webserver->hasArg(F("save"))) {
    WebGetArg(PSTR("pt"), tmp, sizeof(tmp));
    uint8_t new_type = atoi(tmp);

    // Remove old printer
    if (galoped_printers[slot]) {
      galoped_printers[slot]->prtDisconnect();
      delete galoped_printers[slot];
      galoped_printers[slot] = nullptr;
    }

    // Create new printer if type selected
    if (new_type != PRINTER_TYPE_NONE) {
      GalopedPrinter *p = (GalopedPrinter *)PrinterCreate(new_type, slot);
      if (p) {
        p->prtWebFormSave();
        galoped_printers[slot] = p;
        PrinterSaveSlot(slot);
        p->prtBegin();
      }
    } else {
      PrinterSaveSlot(slot);  // Removes config file
    }

    HandleConfiguration();
    return;
  }

  // Render page
  WSContentStart_P(PSTR("3D Printer Configuration"));
  WSContentSendStyle();

  // Slot selector tabs
  WSContentSend_P(PSTR("<div style='text-align:center;margin-bottom:10px'>"));
  for (uint8_t i = 0; i < GALOPED_PRINTER_MAX; i++) {
    const char *style = (i == slot)
      ? "background:#1fa3ec;color:white;font-weight:bold"
      : "background:#eee;color:#333;border:1px solid #ccc";
    const char *label = galoped_printers[i]
      ? galoped_printers[i]->prtTypeName()
      : "Empty";

    WSContentSend_P(PSTR(
      "<a href='" WEB_HANDLE_PRINTER_CFG "?slot=%d' "
      "style='%s;padding:5px 15px;margin:2px;text-decoration:none;display:inline-block'>"
      "P%d: %s</a>"),
      i, style, i + 1, label);
  }
  WSContentSend_P(PSTR("</div>"));

  GalopedPrinter *p = galoped_printers[slot];
  uint8_t current_type = p ? p->prtType() : PRINTER_TYPE_NONE;

  // JavaScript to show/hide type-specific field groups
  WSContentSend_P(PSTR(
    "<script>"
    "function ptC() {"
      "var t = document.getElementById('pt').value;"
      "var divs = document.querySelectorAll('.ptype');"
      "for (var i = 0; i < divs.length; i++) divs[i].style.display = 'none';"
      "var el = document.getElementById('pt_' + t);"
      "if (el) el.style.display = '';"
    "}"
    "</script>"
  ));

  // Form
  WSContentSend_P(PSTR("<fieldset><legend><b>&nbsp;Printer %d&nbsp;</b></legend>"), slot + 1);
  WSContentSend_P(PSTR("<form method='get' action='" WEB_HANDLE_PRINTER_CFG "'>"));
  WSContentSend_P(PSTR("<input type='hidden' name='slot' value='%d'>"), slot);

  // Printer type selector
  WSContentSend_P(PSTR(
    "<p><b>Type</b><br>"
    "<select id='pt' name='pt' onchange='ptC()'>"
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
  if (p && current_type == PRINTER_TYPE_BAMBULAB) {
    p->prtWebFormFields();
  } else {
    GalopedPrinterBBL dummy(slot);
    dummy.prtWebFormFields();
  }
  WSContentSend_P(PSTR("</div>"));

  // OctoPrint fields
  WSContentSend_P(PSTR("<div id='pt_%d' class='ptype'%s>"),
    PRINTER_TYPE_OCTOPRINT,
    current_type == PRINTER_TYPE_OCTOPRINT ? "" : " style='display:none'");
  if (p && current_type == PRINTER_TYPE_OCTOPRINT) {
    p->prtWebFormFields();
  } else {
    GalopedPrinterOctoprint dummy(slot);
    dummy.prtWebFormFields();
  }
  WSContentSend_P(PSTR("</div>"));

  // Connection status indicator
  if (p) {
    WSContentSend_P(PSTR("<hr><p>Status: <b style='color:%s'>%s</b></p>"),
      p->status.connected ? "green" : "red",
      p->status.connected ? "Connected" : "Disconnected");
  }

  // Save button
  WSContentSend_P(PSTR(
    "<br><button name='save' type='submit' class='button bgrn'>"
    D_SAVE "</button></form></fieldset>"));

  WSContentSpaceButton(BUTTON_CONFIGURATION);
  WSContentStop();
}

/*********************************************************************************************\
 * Web sensor display on main page
\*********************************************************************************************/

bool PrinterStatusWeb(void) {
  bool has_data = false;

  for (uint8_t i = 0; i < GALOPED_PRINTER_MAX; i++) {
    GalopedPrinter *p = galoped_printers[i];
    if (!p || !p->status.data_valid) continue;

    has_data = true;
    const char *name = p->prtTypeName();

    // Nozzle temperature with color coding
    const char *temp_color = p->status.nozzle_temp > 200 ? "#F44" :
                             p->status.nozzle_temp > 100 ? "#FA0" : "#4F4";

    WSContentSend_P(PSTR("{s}%s Nozzle{m}<span style='color:%s'>%.0f</span>/%.0f°C{e}"),
      name, temp_color, p->status.nozzle_temp, p->status.nozzle_target);

    // Bed temperature
    WSContentSend_P(PSTR("{s}%s Bed{m}%.0f/%.0f°C{e}"),
      name, p->status.bed_temp, p->status.bed_target);

    // Progress and ETA (only when printing)
    if (p->status.state != PRINTER_STATE_IDLE &&
        p->status.state != PRINTER_STATE_UNKNOWN) {
      WSContentSend_P(PSTR("{s}%s Progress{m}%d%%{e}"), name, p->status.progress);

      if (p->status.remaining_min > 0) {
        uint16_t hours = p->status.remaining_min / 60;
        uint16_t mins  = p->status.remaining_min % 60;
        if (hours > 0) {
          WSContentSend_P(PSTR("{s}%s ETA{m}%dh %dm{e}"), name, hours, mins);
        } else {
          WSContentSend_P(PSTR("{s}%s ETA{m}%dm{e}"), name, mins);
        }
      }
    }

    // Printer state
    WSContentSend_P(PSTR("{s}%s Status{m}%s{e}"), name, p->stateStr());
  }

  return has_data;
}

/*********************************************************************************************\
 * JSON output for MQTT teleperiod
\*********************************************************************************************/

void PrinterShowJson(void) {
  for (uint8_t i = 0; i < GALOPED_PRINTER_MAX; i++) {
    GalopedPrinter *p = galoped_printers[i];
    if (!p || !p->status.data_valid) continue;

    ResponseAppend_P(
      PSTR(",\"%s%d\":{"
           "\"Nozzle\":%.1f,\"NozzleTarget\":%.1f,"
           "\"Bed\":%.1f,\"BedTarget\":%.1f,"
           "\"Progress\":%d,\"Remaining\":%d,"
           "\"State\":\"%s\"}"),
      p->prtTypeName(), i,
      p->status.nozzle_temp, p->status.nozzle_target,
      p->status.bed_temp, p->status.bed_target,
      p->status.progress, p->status.remaining_min,
      p->stateStr());
  }
}

#endif  // USE_WEBSERVER

/*********************************************************************************************\
 * Compatibility wrappers
 *
 * These functions provide backward compatibility for galoped_conf.ino,
 * which accesses printer data via the old Bbl* function names.
 * All wrappers use printer slot 0 (primary printer).
\*********************************************************************************************/

bool BblStatusIsValid() {
  GalopedPrinter *p = (GalopedPrinter *)PrinterGet(0);
  return p && p->status.data_valid;
}

bool BblStatusIsRunning() {
  GalopedPrinter *p = (GalopedPrinter *)PrinterGet(0);
  return p && p->isRunning();
}

bool BblStatusIsFinished() {
  GalopedPrinter *p = (GalopedPrinter *)PrinterGet(0);
  return p && p->isFinished();
}

bool BblStatusIsError() {
  GalopedPrinter *p = (GalopedPrinter *)PrinterGet(0);
  return !p || p->isError();
}

float BblGetNozzleTemp() {
  GalopedPrinter *p = (GalopedPrinter *)PrinterGet(0);
  return p ? p->status.nozzle_temp : 0;
}

uint8_t BblGetProgress() {
  GalopedPrinter *p = (GalopedPrinter *)PrinterGet(0);
  return p ? p->status.progress : 0;
}

uint8_t BblGetGCodeStatus() {
  GalopedPrinter *p = (GalopedPrinter *)PrinterGet(0);
  return p ? p->status.state : PRINTER_STATE_UNKNOWN;
}

bool BblStatusWeb(void) {
  return PrinterStatusWeb();
}

#endif  // USE_GALOPED
