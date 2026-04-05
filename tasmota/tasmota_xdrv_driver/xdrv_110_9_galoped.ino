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

#ifdef USE_GALOPED

#ifndef ESP32
#error "Galoped supports the ESP-32 only"
#endif

/**
 * @brief Custom Galoped commands:
 *
 * GalopedSet - set indication value in defined units
 * GalopedZero - reset drive and set to 0
 */

const char kGalopedCommands[] PROGMEM = "Galoped" "|"  // Prefix
  "|" "Set"
  "|" "Zero"
  ;

void (* const GalopedCommand[])(void) PROGMEM = {
  &GalopedHandlerCommand,
  &GalopedHandlerCommandSet,
  &GalopedHandlerCommandZero,
};

// ---------- Interface ----------

bool Xdrv110(uint32_t function) {
  bool result = false;

  switch (function) {
    case FUNC_PRE_INIT:
      GalopedSettingsLoad(0);
      break;

    case FUNC_INIT:
      GalopedInit();
      BblInit();
      result = true;
      break;

    case FUNC_LOOP:
      BblLoop();
      break;

    case FUNC_EVERY_SECOND:
      BblEverySecond();
      GalopedLoop();
      break;

    case FUNC_SAVE_SETTINGS:
      GalopedSettingsSave();
      break;

    case FUNC_RESET_SETTINGS:
      GalopedSettingsLoad(1);
      break;

    case FUNC_RESTORE_SETTINGS:
      result = GalopedSettingsRestore();
      break;

    case FUNC_COMMAND:
      result = DecodeCommand(kGalopedCommands, GalopedCommand);
      break;

    case FUNC_ACTIVE:
      break;

#ifdef USE_WEBSERVER
    case FUNC_WEB_ADD_HANDLER:
      WebServer_on(PSTR("/" WEB_HANDLE_GALOPED), GalopedPage);
      WebServer_on(PSTR("/" WEB_HANDLE_GALOPED_CFG), GalopedConfigPage);
      WebServer_on(PSTR("/" WEB_HANDLE_BBL_CFG), BblConfigPage);
      break;

    case FUNC_WEB_ADD_MAIN_BUTTON:
      WSContentSend_P(HTTP_FORM_BUTTON, PSTR(WEB_HANDLE_GALOPED), PSTR("Galoped"));
      break;

    case FUNC_WEB_ADD_BUTTON:
      WSContentSend_P(HTTP_FORM_BUTTON, PSTR(WEB_HANDLE_GALOPED_CFG), PSTR("Configure Galoped"));
      WSContentSend_P(HTTP_FORM_BUTTON, PSTR(WEB_HANDLE_BBL_CFG), PSTR("Configure BambuLab"));
      break;

    case FUNC_WEB_SENSOR:
      result = GalopedStatusWeb();
      break;

    case FUNC_JSON_APPEND:
      BblShowJson(true);
      break;

#endif  // USE_WEBSERVER
  }

  return result;
}

#endif  // USE_GALOPED
