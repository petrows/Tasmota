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

#define XDRV_110 110

#define WEB_HANDLE_GALOPED "galoped"

const char* galopedSerial = "009";
const char* galopedDisplay = "CO2";
const char* galopedColor = "Black";

#define TABLE_INFO_ROW_START "<tr><th>"
#define TABLE_INFO_ROW_MID "</th><td>"
#define TABLE_INFO_ROW_END "</td></tr>"

/*********************************************************************************************\
 * Web UI
\*********************************************************************************************/

#ifdef USE_WEBSERVER



void HandleGaloped(void) {
  if (!HttpCheckPriviledgedAccess()) { return; }

  AddLog(LOG_LEVEL_DEBUG, PSTR(D_LOG_HTTP "Galoped"));

  WSContentStart_P(PSTR("Galoped"));
  WSContentSendStyle();
  WSContentSend_P(HTTP_MENU_HEAD, "Galoped info");
  WSContentSend_P(PSTR("<table style=\"width:100%%\">"));
  WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Serial number" TABLE_INFO_ROW_MID "%s" TABLE_INFO_ROW_END), galopedSerial);
  WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Display" TABLE_INFO_ROW_MID "%s" TABLE_INFO_ROW_END), galopedDisplay);
  WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Color" TABLE_INFO_ROW_MID "%s" TABLE_INFO_ROW_END), galopedColor);
  WSContentSeparatorIThin();
  if (static_cast<uint32_t>(WiFi.localIP()) != 0) {
    WSContentSend_P(PSTR(TABLE_INFO_ROW_START D_MAC_ADDRESS TABLE_INFO_ROW_MID "%s" TABLE_INFO_ROW_END), WiFiHelper::macAddress().c_str());
    WSContentSend_P(PSTR(TABLE_INFO_ROW_START D_IP_ADDRESS TABLE_INFO_ROW_MID "%_I" TABLE_INFO_ROW_END), (uint32_t)WiFi.localIP());
    WSContentSeparatorIThin();
  }
  WSContentSend_P(PSTR("</table>"));
  WSContentSpaceButton(BUTTON_MAIN);
  WSContentStop();
}

#endif  // USE_WEBSERVER

// ---------- Interface ----------

bool Xdrv110(uint32_t function) {
  bool result = false;

  switch (function) {
    case FUNC_INIT:
      result = true;
      break;

    case FUNC_COMMAND:
      break;

    case FUNC_ACTIVE:
      break;

#ifdef USE_WEBSERVER
    case FUNC_WEB_ADD_HANDLER:
      WebServer_on(PSTR("/" WEB_HANDLE_GALOPED), HandleGaloped);
      break;

    case FUNC_WEB_ADD_MAIN_BUTTON:
      WSContentSend_P(HTTP_FORM_BUTTON, PSTR(WEB_HANDLE_GALOPED), PSTR("Galoped"));
      break;

#endif  // USE_WEBSERVER
  }

  return result;
}

#endif  // USE_GALOPED
