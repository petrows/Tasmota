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

#define GALOPED_INFO_FILE "/galoped.inf"
#define GALOPED_INFO_MAX_LINE 256
#define GALOPED_INFO_NUM_FIELDS 5

// Default values (used when info file is missing or invalid)
const char* galopedSerialDefault = "000";
const char* galopedDisplayDefault = "Unknown";
const char* galopedColorDefault = "Unknown";

#define TABLE_INFO_ROW_START "<tr><th>"
#define TABLE_INFO_ROW_MID "</th><td>"
#define TABLE_INFO_ROW_END "</td></tr>"

struct GalopedInfo {
  char serial[GALOPED_INFO_MAX_LINE];
  char display[GALOPED_INFO_MAX_LINE];
  char color[GALOPED_INFO_MAX_LINE];
  char mac[GALOPED_INFO_MAX_LINE];
  char signature[GALOPED_INFO_MAX_LINE];
  bool loaded;
  bool valid;
  bool mac_match;
};

static GalopedInfo galoped_info = { "", "", "", "", "", false, false, false };

// Public key for signature verification
#define GALOPED_SIG_E 17
#define GALOPED_SIG_N 3233

// Modular exponentiation: (base ^ exp) mod mod
static uint32_t GalopedModPow(uint32_t base, uint32_t exp, uint32_t mod) {
  uint64_t result = 1;
  uint64_t b = base % mod;
  while (exp > 0) {
    if (exp & 1) {
      result = (result * b) % mod;
    }
    exp >>= 1;
    b = (b * b) % mod;
  }
  return (uint32_t)result;
}

// Simple hash of a string to a value in range [0, mod)
static uint32_t GalopedHash(const char* data, uint32_t mod) {
  uint32_t hash = 5381;
  while (*data) {
    hash = ((hash << 5) + hash) + (uint8_t)(*data);
    data++;
  }
  return hash % mod;
}

// Verify signature against serial|display|color|mac
static bool GalopedVerifySignature(const char* serial, const char* display,
                                   const char* color, const char* mac,
                                   const char* signature_str) {
  // Build message: "serial|display|color|mac"
  char message[GALOPED_INFO_MAX_LINE * 4 + 4];
  snprintf(message, sizeof(message), "%s|%s|%s|%s", serial, display, color, mac);

  uint32_t message_hash = GalopedHash(message, GALOPED_SIG_N);

  // Parse signature as decimal number
  uint32_t sig = strtoul(signature_str, nullptr, 10);
  if (sig == 0 && signature_str[0] != '0') {
    return false;  // Invalid signature string
  }

  // Verify: (sig ^ e) mod n == message_hash
  uint32_t recovered = GalopedModPow(sig, GALOPED_SIG_E, GALOPED_SIG_N);
  return recovered == message_hash;
}

// Read the information file and verify signature
static void GalopedReadInfoFile(void) {
  galoped_info.loaded = false;
  galoped_info.valid = false;

  String content = TfsLoadString(GALOPED_INFO_FILE);
  if (content.length() == 0) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("GAL: Info file '%s' not found or empty"), GALOPED_INFO_FILE);
    return;
  }

  char* fields[GALOPED_INFO_NUM_FIELDS] = {
    galoped_info.serial,
    galoped_info.display,
    galoped_info.color,
    galoped_info.mac,
    galoped_info.signature
  };

  // Parse lines from loaded string
  uint32_t field_idx = 0;
  int start = 0;
  while (field_idx < GALOPED_INFO_NUM_FIELDS && start <= (int)content.length()) {
    int end = content.indexOf('\n', start);
    if (end < 0) { end = content.length(); }
    String line = content.substring(start, end);
    line.trim();
    start = end + 1;
    if (line.length() == 0) { continue; }
    if (line.length() >= GALOPED_INFO_MAX_LINE) {
      AddLog(LOG_LEVEL_DEBUG, PSTR("GAL: Line %d too long"), field_idx + 1);
      return;
    }
    strlcpy(fields[field_idx], line.c_str(), GALOPED_INFO_MAX_LINE);
    field_idx++;
  }

  if (field_idx < GALOPED_INFO_NUM_FIELDS) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("GAL: Info file incomplete (%d/%d fields)"), field_idx, GALOPED_INFO_NUM_FIELDS);
    return;
  }

  galoped_info.loaded = true;

  // Verify signature
  galoped_info.valid = GalopedVerifySignature(
    galoped_info.serial, galoped_info.display,
    galoped_info.color, galoped_info.mac, galoped_info.signature
  );

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

void HandleGaloped(void) {
  if (!HttpCheckPriviledgedAccess()) { return; }

  AddLog(LOG_LEVEL_DEBUG, PSTR(D_LOG_HTTP "Galoped"));

  // Re-read info file on each page load to pick up changes
  GalopedReadInfoFile();

  const char* serial = galoped_info.loaded ? galoped_info.serial : galopedSerialDefault;
  const char* display = galoped_info.loaded ? galoped_info.display : galopedDisplayDefault;
  const char* color = galoped_info.loaded ? galoped_info.color : galopedColorDefault;

  WSContentStart_P(PSTR("Galoped"));
  WSContentSendStyle();
  WSContentSend_P(HTTP_MENU_HEAD, "Galoped info");
  WSContentSend_P(PSTR("<table style=\"width:100%%\">"));
  WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Serial number" TABLE_INFO_ROW_MID "%s" TABLE_INFO_ROW_END), serial);
  WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Display" TABLE_INFO_ROW_MID "%s" TABLE_INFO_ROW_END), display);
  WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Color" TABLE_INFO_ROW_MID "%s" TABLE_INFO_ROW_END), color);
  if (galoped_info.loaded) {
    const char* mac_style = galoped_info.mac_match
      ? "color:green;font-weight:bold"
      : "color:red;font-weight:bold";
    const char* mac_status = galoped_info.mac_match ? " (OK)" : " (MISMATCH)";
    WSContentSend_P(PSTR(TABLE_INFO_ROW_START "MAC (info)" TABLE_INFO_ROW_MID "%s <span style=\"%s\">%s</span>" TABLE_INFO_ROW_END),
                    galoped_info.mac, mac_style, mac_status);
  }
  WSContentSeparatorIThin();

  // Signature status
  if (galoped_info.loaded) {
    bool all_ok = galoped_info.valid && galoped_info.mac_match;
    const char* status = all_ok ? "OK" : "INVALID";
    const char* style = all_ok
      ? "color:green;font-weight:bold"
      : "color:red;font-weight:bold";
    WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Signature" TABLE_INFO_ROW_MID "<span style=\"%s\">%s</span>" TABLE_INFO_ROW_END), style, status);
  } else {
    WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Signature" TABLE_INFO_ROW_MID "<span style=\"color:orange\">No info file</span>" TABLE_INFO_ROW_END));
  }
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
