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

#define XDRV_110 110

#define WEB_HANDLE_GALOPED "galoped"

#define GALOPED_INFO_MAX_LINE 32
#define GALOPED_INFO_NUM_FIELDS 5

#include "IniFile.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

#define TABLE_INFO_ROW_START "<tr><th>"
#define TABLE_INFO_ROW_MID "</th><td>"
#define TABLE_INFO_ROW_END "</td></tr>"

struct GalopedInfo {
  char serial[GALOPED_INFO_MAX_LINE];
  char display[GALOPED_INFO_MAX_LINE];
  char color[GALOPED_INFO_MAX_LINE];
  char backlight[GALOPED_INFO_MAX_LINE];
  char assembled[GALOPED_INFO_MAX_LINE];
  char personal[GALOPED_INFO_MAX_LINE];
  char mac[GALOPED_INFO_MAX_LINE];
  bool loaded;
  bool valid;
  bool mac_match;
};

static GalopedInfo galoped_info = { "", "", "", "", "", "", "", false, false, false };

struct RsaVerifyResult {
  bool ok;
  char message[128];
};

static RsaVerifyResult galoped_sign = { false, "" };

// Verify RSA/PKCS#1 signature of a file using mbedtls (ESP32 hardware-accelerated).
//   data_filename   – path on LittleFS to the file being verified
//   sig_filename    – path on LittleFS to the binary DER-encoded signature file
// Returns RsaVerifyResult { ok=true, message="OK" } on success, or
//   { ok=false, message="<description>" } on any failure.
static bool GalopedVerifyRsaFileSignature(
    const char* data_filename,
    const char* sig_filename)
{
  int ret;
  galoped_sign.ok = false;

  // --- Parse public key ---
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  // mbedtls PEM parser requires the null terminator to be included in the length
  ret = mbedtls_pk_parse_public_key(&pk,
      (unsigned char*)Galoped_sig_pub,
      Galoped_sig_pub_len);
  if (ret != 0) {
    snprintf(galoped_sign.message, sizeof(galoped_sign.message), "Public key parse failed: -0x%04X", (unsigned)(-ret));
    mbedtls_pk_free(&pk);
    return false;
  }

  // --- Compute SHA-256 of the data file ---
  File data_file = LittleFS.open(data_filename, "r");
  if (!data_file) {
    strlcpy(galoped_sign.message, "Data file not found", sizeof(galoped_sign.message));
    mbedtls_pk_free(&pk);
    return false;
  }

  mbedtls_sha256_context sha_ctx;
  mbedtls_sha256_init(&sha_ctx);
  mbedtls_sha256_starts(&sha_ctx, 0);   // 0 = SHA-256 (not SHA-224)

  uint8_t io_buf[512];
  while (data_file.available()) {
    size_t n = data_file.read(io_buf, sizeof(io_buf));
    if (n > 0) {
      mbedtls_sha256_update(&sha_ctx, io_buf, n);
    }
  }
  data_file.close();

  uint8_t hash[32];
  mbedtls_sha256_finish(&sha_ctx, hash);
  mbedtls_sha256_free(&sha_ctx);

  // --- Read the signature file ---
  File sig_file = LittleFS.open(sig_filename, "r");
  if (!sig_file) {
    strlcpy(galoped_sign.message, "Signature file not found", sizeof(galoped_sign.message));
    mbedtls_pk_free(&pk);
    return false;
  }

  size_t sig_len = sig_file.size();
  // RSA-4096 produces 512-byte signatures; reject obviously invalid sizes
  if (sig_len == 0 || sig_len > 512) {
    snprintf(galoped_sign.message, sizeof(galoped_sign.message), "Signature file has invalid size: %u", (unsigned)sig_len);
    sig_file.close();
    mbedtls_pk_free(&pk);
    return false;
  }

  uint8_t sig_buf[512];
  sig_file.read(sig_buf, sig_len);
  sig_file.close();

  // --- Verify signature ---
  ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig_buf, sig_len);
  mbedtls_pk_free(&pk);

  if (ret != 0) {
    snprintf(galoped_sign.message, sizeof(galoped_sign.message), "Signature invalid: -0x%04X", (unsigned)(-ret));
    return false;
  }

  galoped_sign.ok = true;
  strlcpy(galoped_sign.message, "OK", sizeof(galoped_sign.message));
  return true;
}

// Read the information file and verify signature
static void GalopedReadInfoFile(void) {
  galoped_info.loaded = false;
  galoped_info.valid = false;

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
    WSContentSend_P(PSTR("<div style='padding:5px;text-align:center;'><b style='color:red'>Device information not available</b><br/><br/>%s</div>"), error_msg);
  } else {
    WSContentSend_P(PSTR(HTTP_TABLE100));

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
    // WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Signature" TABLE_INFO_ROW_MID "<b style='color:green;'>OK</b>" TABLE_INFO_ROW_END));
    WSContentSeparatorIThin();
    WSContentSend_P(PSTR(TABLE_INFO_ROW_START "Chipset" TABLE_INFO_ROW_MID "%s" TABLE_INFO_ROW_END), GetDeviceHardwareRevision().c_str());
    if (static_cast<uint32_t>(WiFi.localIP()) != 0) {
      WSContentSend_P(PSTR(TABLE_INFO_ROW_START D_MAC_ADDRESS TABLE_INFO_ROW_MID "%s" TABLE_INFO_ROW_END), WiFiHelper::macAddress().c_str());
      WSContentSend_P(PSTR(TABLE_INFO_ROW_START D_IP_ADDRESS TABLE_INFO_ROW_MID "%_I" TABLE_INFO_ROW_END), (uint32_t)WiFi.localIP());
      WSContentSeparatorIThin();
    }
    WSContentSend_P(PSTR("</table>"));
  }
  // Page bottom
  WSContentSend_P(PSTR("<p style='text-align:center;padding:5px;font-weight:bold;'><a href='https://gp.petro.ws/' target='_blank'>Galoped homepage</a></p>"));
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
      WebServer_on(PSTR("/" WEB_HANDLE_GALOPED), GalopedPage);
      break;

    case FUNC_WEB_ADD_MAIN_BUTTON:
      WSContentSend_P(HTTP_FORM_BUTTON, PSTR(WEB_HANDLE_GALOPED), PSTR("Galoped"));
      break;

#endif  // USE_WEBSERVER
  }

  return result;
}

#endif  // USE_GALOPED
