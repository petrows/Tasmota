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
  FRAM MB85RC04V (4Kbit / 512 bytes) I2C driver

  The MB85RC04V uses a 9-bit memory address:
  - Bit 8 (MSB) is embedded into the I2C device address as the LSB
  - Bits 7..0 are sent as the register/address byte
  Base I2C address: 0x50
*/

#ifdef USE_GALOPED

#define GALOPED_FRAM_BASE_ADDR  0x50
#define GALOPED_FRAM_SIZE       512

static bool galoped_fram_detected = false;

// Initialize FRAM: detect presence on I2C bus
bool GalopedFramInit(void) {
  galoped_fram_detected = false;

  if (!TasmotaGlobal.i2c_enabled) { return false; }

  Wire.beginTransmission(GALOPED_FRAM_BASE_ADDR);
  if (0 == Wire.endTransmission()) {
    galoped_fram_detected = true;
    AddLog(LOG_LEVEL_INFO, PSTR("GAL: FRAM MB85RC04V detected at 0x%02X"), GALOPED_FRAM_BASE_ADDR);
  }

  return galoped_fram_detected;
}

// Write a single byte to FRAM at given address (0..511)
static bool GalopedFramWriteByte(uint16_t addr, uint8_t data) {
  if (!galoped_fram_detected || addr >= GALOPED_FRAM_SIZE) { return false; }

  // Bit 8 of address goes into device address LSB
  uint8_t dev_addr = GALOPED_FRAM_BASE_ADDR | ((addr >> 8) & 0x01);
  uint8_t mem_addr = addr & 0xFF;

  Wire.beginTransmission(dev_addr);
  Wire.write(mem_addr);
  Wire.write(data);
  return (0 == Wire.endTransmission());
}

// Read a single byte from FRAM at given address (0..511)
static bool GalopedFramReadByte(uint16_t addr, uint8_t *data) {
  if (!galoped_fram_detected || addr >= GALOPED_FRAM_SIZE) { return false; }

  uint8_t dev_addr = GALOPED_FRAM_BASE_ADDR | ((addr >> 8) & 0x01);
  uint8_t mem_addr = addr & 0xFF;

  Wire.beginTransmission(dev_addr);
  Wire.write(mem_addr);
  if (0 != Wire.endTransmission()) { return false; }

  Wire.requestFrom((int)dev_addr, 1);
  if (Wire.available() < 1) { return false; }

  *data = Wire.read();
  return true;
}

// Write uint16 value to FRAM (big-endian, 2 consecutive bytes)
bool GalopedFramWriteUint16(uint16_t addr, uint16_t value) {
  if (!GalopedFramWriteByte(addr, (uint8_t)(value >> 8))) { return false; }
  return GalopedFramWriteByte(addr + 1, (uint8_t)(value & 0xFF));
}

// Read uint16 value from FRAM (big-endian, 2 consecutive bytes)
bool GalopedFramReadUint16(uint16_t addr, uint16_t *value) {
  uint8_t hi, lo;
  if (!GalopedFramReadByte(addr, &hi)) { return false; }
  if (!GalopedFramReadByte(addr + 1, &lo)) { return false; }
  *value = ((uint16_t)hi << 8) | lo;
  return true;
}

#endif  // USE_GALOPED
