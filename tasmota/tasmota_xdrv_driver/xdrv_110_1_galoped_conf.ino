/*
  xdrv_110_galoped.ino - Galoped support library

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
  Main function(s) implementation
*/

#ifdef USE_GALOPED

#define GALOPED_STRINGIFY_(x) #x
#define GALOPED_STRINGIFY(x) GALOPED_STRINGIFY_(x)

#define WEB_HANDLE_GALOPED "galoped"

#define GALOPED_INFO_MAX_LINE 32
#define GALOPED_INFO_NUM_FIELDS 5

#include "IniFile.h"

#define WEB_HANDLE_GALOPED_CFG "galopcfg"

#define TABLE_INFO_ROW_START "<tr><th>"
#define TABLE_INFO_ROW_MID "</th><td>"
#define TABLE_INFO_ROW_END "</td></tr>"

// RGB LED modes
#define GALOPED_RGB_STATIC      0  // Static (fixed color)
#define GALOPED_RGB_DYNAMIC     1  // Dynamic: green-yellow-red follows Gauge1
#define GALOPED_RGB_GRADIENT    2  // Static gradient: green-yellow-red always
#define GALOPED_RGB_MODE_MAX    2

#define GALOPED_SETTINGS_VERSION 0x01010100

struct GalopedSettings {
  uint32_t crc32;
  uint32_t version;
  uint8_t  rgb_mode;
};

static GalopedSettings galoped_settings;

// Device indicator mode
#define GALOPED_DISPLAY_NONE  0 // No automation, just indicator
#define GALOPED_DISPLAY_CO2   1 // Display CO2 level


/*********************************************************************************************\
 * Driver Settings load and save
\*********************************************************************************************/

static void GalopedSettingsDefault(void) {
  memset(&galoped_settings, 0x00, sizeof(galoped_settings));
  galoped_settings.version = GALOPED_SETTINGS_VERSION;
  galoped_settings.rgb_mode = GALOPED_RGB_STATIC;
}

static void GalopedSettingsLoad(bool erase) {
  GalopedSettingsDefault();

#ifdef USE_UFILESYS
  char filename[20];
  snprintf_P(filename, sizeof(filename), PSTR(TASM_FILE_DRIVER), XDRV_110);
  if (erase) {
    TfsDeleteFile(filename);
  } else if (TfsLoadFile(filename, (uint8_t*)&galoped_settings, sizeof(galoped_settings))) {
    if (galoped_settings.version != GALOPED_SETTINGS_VERSION) {
      galoped_settings.version = GALOPED_SETTINGS_VERSION;
      GalopedSettingsSave();
    }
    AddLog(LOG_LEVEL_INFO, PSTR("GAL: Settings loaded, rgb_mode=%d"), galoped_settings.rgb_mode);
  } else {
    AddLog(LOG_LEVEL_DEBUG, PSTR("GAL: Settings file not found, using defaults"));
  }
#endif  // USE_UFILESYS
}

static void GalopedSettingsSave(void) {
#ifdef USE_UFILESYS
  uint32_t crc32 = GetCfgCrc32((uint8_t*)&galoped_settings + 4, sizeof(galoped_settings) - 4);
  if (crc32 != galoped_settings.crc32) {
    galoped_settings.crc32 = crc32;
    char filename[20];
    snprintf_P(filename, sizeof(filename), PSTR(TASM_FILE_DRIVER), XDRV_110);
    TfsSaveFile(filename, (const uint8_t*)&galoped_settings, sizeof(galoped_settings));
    AddLog(LOG_LEVEL_DEBUG, PSTR("GAL: Settings saved"));
  }
#endif  // USE_UFILESYS
}

static bool GalopedSettingsRestore(void) {
  XdrvMailbox.data = (char*)&galoped_settings;
  XdrvMailbox.index = sizeof(galoped_settings);
  return true;
}

struct GalopedInfo {
  char serial[GALOPED_INFO_MAX_LINE];
  char display[GALOPED_INFO_MAX_LINE];
  char color[GALOPED_INFO_MAX_LINE];
  char backlight[GALOPED_INFO_MAX_LINE];
  char assembled[GALOPED_INFO_MAX_LINE];
  char personal[GALOPED_INFO_MAX_LINE];
  char mac[GALOPED_INFO_MAX_LINE];
  uint8_t display_mode;
  bool loaded;
  bool valid;
  bool mac_match;
};

static GalopedInfo galoped_info = { "", "", "", "", "", "", "", GALOPED_DISPLAY_NONE, false, false, false };

struct GalopedGauge {
  uint8_t id;
  char name[8];
  char unit[8];
  uint16_t scale_deg;
};

static GalopedGauge galoped_gauge_1 = { 1, "", "", 320 };
static GalopedGauge galoped_gauge_2 = { 2, "", "", 270 };

// Read the information file and verify signature
static void GalopedReadInfoFile(void) {
  galoped_info.loaded = false;
  galoped_info.valid = false;

  // Common buffer
  char buf[32];
  size_t buf_size = sizeof(buf)/sizeof(buf[0]);

  GalopedVerifyRsaFileSignature("/galoped.ini", "/galoped.sig");

  File ini_file = LittleFS.open("/galoped.ini", "r");
  if (!ini_file) {
    AddLog(LOG_LEVEL_INFO, PSTR("GAL: Info file read error"));
    return;
  }
  IniFile ini(ini_file);
  ini.getValueStr("galoped", "serial", galoped_info.serial, GALOPED_INFO_MAX_LINE);
  ini.getValueStr("galoped", "display", galoped_info.display, GALOPED_INFO_MAX_LINE);
  ini.getValueStr("galoped", "color", galoped_info.color, GALOPED_INFO_MAX_LINE);
  ini.getValueStr("galoped", "backlight", galoped_info.backlight, GALOPED_INFO_MAX_LINE);
  ini.getValueStr("galoped", "assembled", galoped_info.assembled, GALOPED_INFO_MAX_LINE);
  ini.getValueStr("galoped", "personal", galoped_info.personal, GALOPED_INFO_MAX_LINE);
  ini.getValueStr("galoped", "mac", galoped_info.mac, GALOPED_INFO_MAX_LINE);

  bzero(buf, buf_size);
  ini.getValueStr("galoped", "model", buf, buf_size);
  AddLog(LOG_LEVEL_INFO, PSTR("GAL: Model %s"), buf);
  if (strcmp(buf, "co2") == 0) {
    // Standart Galoped CO2 meter
    AddLog(LOG_LEVEL_INFO, PSTR("GAL: Device mode: CO2"));
    galoped_info.display_mode = GALOPED_DISPLAY_CO2;
  }

  ini_file.close();

  galoped_info.valid = galoped_sign.ok;
  galoped_info.loaded = true;

  // Check MAC address matches device
  galoped_info.mac_match = (strcasecmp(galoped_info.mac, WiFiHelper::macAddress().c_str()) == 0);

  AddLog(LOG_LEVEL_INFO, PSTR("GAL: Info loaded, serial=%s, signature %s, MAC %s"),
         galoped_info.serial,
         galoped_info.valid ? "valid" : "INVALID",
         galoped_info.mac_match ? "match" : "MISMATCH");
}

/*********************************************************************************************\
 * Web UI
\*********************************************************************************************/

#ifdef USE_WEBSERVER

void GalopedPage(void) {
  if (!HttpCheckPriviledgedAccess()) { return; }

  AddLog(LOG_LEVEL_DEBUG, PSTR(D_LOG_HTTP "Galoped"));

  // Re-read info file on each page load to pick up changes
  if (!galoped_info.loaded) {
    GalopedReadInfoFile();
  }

  WSContentStart_P(PSTR("Galoped"));
  WSContentSendStyle();
  WSContentSend_P(HTTP_MENU_HEAD, "Galoped info");

  // Data and sig valid?
  bool all_ok = galoped_info.valid && galoped_info.mac_match;

  WSContentSend_P(PSTR(HTTP_TABLE100));

  if (!all_ok) {
    // Information page error: display error and exit
    const char* error_msg = "Unknown error";
    if (!galoped_info.loaded) {
      error_msg = "Info read error";
    } else if (!galoped_info.mac_match) {
      error_msg = "Invalid device MAC address";
    } else if (!galoped_info.valid) {
      error_msg = "Invalid device signature";
    }
    WSContentSend_P(PSTR("<tr><td><div style='padding:5px;text-align:center;'><b style='color:red'>Device information not available</b><br/><br/>%s</div></td></tr>"), error_msg);
  } else {
    if (strlen(galoped_info.personal)) {
      WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Built for" TABLE_INFO_ROW_MID "<b style='color:gold'>%s<b>" TABLE_INFO_ROW_END), galoped_info.personal);
      WSContentSeparatorIThin();
    }
    WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Serial number" TABLE_INFO_ROW_MID "%s" TABLE_INFO_ROW_END), galoped_info.serial);
    if (strlen(galoped_info.display)) {
      WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Display" TABLE_INFO_ROW_MID "%s" TABLE_INFO_ROW_END), galoped_info.display);
    }
    if (strlen(galoped_info.backlight)) {
      WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Backlight" TABLE_INFO_ROW_MID "%s" TABLE_INFO_ROW_END), galoped_info.backlight);
    }
    if (strlen(galoped_info.assembled)) {
      WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Assembled" TABLE_INFO_ROW_MID "%s" TABLE_INFO_ROW_END), galoped_info.assembled);
    }
    if (strlen(galoped_info.color)) {
      const char* color_style = "";
      if (strcmp(galoped_info.color, "white") == 0) {
        color_style = "color:black;background-color:white;";
      } else if (strcmp(galoped_info.color, "black") == 0) {
        color_style = "color:white;background-color:black;";
      }
      WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Color" TABLE_INFO_ROW_MID "<span style='border:1px solid #666;padding:3px;%s'>%s</div>" TABLE_INFO_ROW_END), color_style, galoped_info.color);
    }
  } // End signed block
  WSContentSeparatorIThin();
  // WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Signature" TABLE_INFO_ROW_MID "<b style='color:green;'>OK</b>" TABLE_INFO_ROW_END));
#if defined(GALOPED_VERSION)
  WSContentSend_P(PSTR(TABLE_INFO_ROW_START "FW Version" TABLE_INFO_ROW_MID GALOPED_STRINGIFY(GALOPED_VERSION) TABLE_INFO_ROW_END));
#endif
  WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Chipset" TABLE_INFO_ROW_MID "%s" TABLE_INFO_ROW_END), GetDeviceHardwareRevision().c_str());
  WSContentSend_P(PSTR(TABLE_INFO_ROW_START D_MAC_ADDRESS TABLE_INFO_ROW_MID "%s" TABLE_INFO_ROW_END), WiFiHelper::macAddress().c_str());
  if (static_cast<uint32_t>(WiFi.localIP()) != 0) {
    WSContentSend_P(PSTR(TABLE_INFO_ROW_START D_IP_ADDRESS TABLE_INFO_ROW_MID "%_I" TABLE_INFO_ROW_END), (uint32_t)WiFi.localIP());
  }
  WSContentSeparatorIThin();
  WSContentSend_P(PSTR("</table>"));

  // Page bottom
  // Settings link:
  WSContentSend_P(PSTR("<p style='text-align:center;padding:5px;font-weight:bold;'><a href='" WEB_HANDLE_GALOPED_CFG "' target='_blank'>Galoped settings</a></p>"));
  // Webpage link:
  WSContentSend_P(PSTR("<p style='text-align:center;padding:5px;font-weight:bold;'><a href='https://gp.petro.ws/?mac=%s' target='_blank'>Galoped homepage</a></p>"), WiFiHelper::macAddress().c_str());
  // Return button
  WSContentSpaceButton(BUTTON_MAIN);
  WSContentStop();
}

void GalopedConfigPage(void) {
  if (!HttpCheckPriviledgedAccess()) { return; }

  AddLog(LOG_LEVEL_DEBUG, PSTR(D_LOG_HTTP "Galoped config"));

  if (Webserver->hasArg(F("save"))) {
    char tmp[8];
    WebGetArg(PSTR("rm"), tmp, sizeof(tmp));
    uint8_t mode = atoi(tmp);
    if (mode <= GALOPED_RGB_MODE_MAX) {
      galoped_settings.rgb_mode = mode;
      GalopedSettingsSave();
    }
    HandleConfiguration();
    return;
  }

  WSContentStart_P(PSTR("Galoped Configuration"));
  WSContentSendStyle();

  WSContentSend_P(PSTR("<fieldset><legend><b>&nbsp;RGB LED&nbsp;</b></legend>"));
  WSContentSend_P(PSTR("<form method='get' action='" WEB_HANDLE_GALOPED_CFG "'>"));
  WSContentSend_P(PSTR("<p><b>RGB Mode</b><br>"
    "<select id='rm' name='rm'>"
    "<option value='%d'%s>Static</option>"
    "<option value='%d'%s>Dynamic (Gauge)</option>"
    "<option value='%d'%s>Gradient</option>"
    "</select></p>"),
    GALOPED_RGB_STATIC,  (galoped_settings.rgb_mode == GALOPED_RGB_STATIC)   ? " selected" : "",
    GALOPED_RGB_DYNAMIC,  (galoped_settings.rgb_mode == GALOPED_RGB_DYNAMIC)  ? " selected" : "",
    GALOPED_RGB_GRADIENT, (galoped_settings.rgb_mode == GALOPED_RGB_GRADIENT) ? " selected" : "");
  WSContentSend_P(PSTR("<br><button name='save' type='submit' class='button bgrn'>" D_SAVE "</button>"));
  WSContentSend_P(PSTR("</form></fieldset>"));

  WSContentSpaceButton(BUTTON_CONFIGURATION);
  WSContentStop();
}

#endif  // USE_WEBSERVER

void GalopedInit(void) {
  // Load primary settings file and init
  GalopedReadInfoFile();
}

// Returns Hue (0-360) for green→yellow→red transition
// 0.0 ratio = green (120°), 0.5 = yellow (60°), 1.0 = red (0°)
uint16_t GalopedColorGYR(float value, float min_val, float max_val) {
  float ratio = (value - min_val) / (max_val - min_val);
  if (ratio < 0.0f) ratio = 0.0f;
  if (ratio > 1.0f) ratio = 1.0f;
  return (uint16_t)(120.0f * (1.0f - ratio));
}

// Convert Hue (0-360) to RGB with given brightness (0-255)
void GalopedHueToRGB(uint16_t hue, uint8_t brightness, uint8_t *r, uint8_t *g, uint8_t *b) {
  // HSV to RGB with S=100%, V=brightness
  float h = (float)hue / 60.0f;
  float v = (float)brightness / 255.0f;
  int i = (int)h;
  float f = h - i;
  uint8_t q = (uint8_t)(brightness * (1.0f - f));
  uint8_t t = (uint8_t)(brightness * f);
  switch (i % 6) {
    case 0: *r = brightness; *g = t;          *b = 0; break;
    case 1: *r = q;          *g = brightness; *b = 0; break;
    case 2: *r = 0;          *g = brightness; *b = t; break;
    case 3: *r = 0;          *g = q;          *b = brightness; break;
    case 4: *r = t;          *g = 0;          *b = brightness; break;
    case 5: *r = brightness; *g = 0;          *b = q; break;
  }
}

// Set static green-yellow-red gradient on addressable LEDs, preserving user brightness
void GalopedSetGradient(void) {
  uint32_t num_pixels = Ws2812PixelCount();
  if (num_pixels == 0) return;

  uint8_t brightness = changeUIntScale(Settings->light_dimmer, 0, 100, 0, 255);
  for (uint32_t i = 0; i < num_pixels; i++) {
    uint16_t hue = (uint16_t)(120.0f * (1.0f - (float)i / (float)(num_pixels - 1)));
    uint8_t r, g, b;
    GalopedHueToRGB(hue, brightness, &r, &g, &b);
    Ws2812SetPixelColor(i, r, g, b, 0);
  }
  Ws2812ForceUpdate();
  Ws2812Show();
}

// External sensors data
// CO2
extern uint16_t senseair_co2;
uint16_t galoped_value_co2 = 0;

// Main function to control everything
void GalopedLoop(void) {
  // In not (yet) init -> exit
  if (!galoped_info.loaded) {
    return;
  }

  // Check if Light1 is on
  bool light_on = bitRead(TasmotaGlobal.power, Light.device - 1);

  // Apply gradient when mode is active and light is on
  if (light_on && GALOPED_RGB_GRADIENT == galoped_settings.rgb_mode) {
    GalopedSetGradient();
  }

  // CO2 device?
  if (GALOPED_DISPLAY_CO2 == galoped_info.display_mode) {
    if (galoped_value_co2 != senseair_co2) {
      // Value changed
      AddLog(LOG_LEVEL_INFO, PSTR("GAL: CO2 value %d"), senseair_co2);
      galoped_value_co2 = senseair_co2;

      // Common buffer
      char buf[32];
      size_t buf_size = sizeof(buf)/sizeof(buf[0]);

      // Calculate value
      // Dead zone (15*12 steps) + linear (300*12 steps / 1800 units range)
      uint16_t drive_pos = 180 + ((int(galoped_value_co2) - 400) * 2);

      // Generate drive command
      snprintf_P(buf, buf_size, PSTR("GaugeSet1 %d"), drive_pos);
      ExecuteCommand(buf, SRC_SENSOR);

      // Update Backlight color (hue only, preserving user brightness)
      if (light_on && GALOPED_RGB_DYNAMIC == galoped_settings.rgb_mode) {
        uint16_t hue = GalopedColorGYR((float)galoped_value_co2, 400, 2200);
        snprintf_P(buf, buf_size, PSTR("HSBColor %d,100"), hue);
        ExecuteCommand(buf, SRC_SENSOR);
      }
    }
  }
}

#endif  // USE_GALOPED
