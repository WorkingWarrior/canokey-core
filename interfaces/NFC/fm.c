// SPDX-License-Identifier: Apache-2.0
#include "device.h"
#include <stdint.h>

static void device_delay_us(int us) {
  for (int i = 0; i < us * 10; ++i)
    asm volatile("nop");
}

uint8_t fm_read_reg(uint16_t reg) {
  uint8_t val;
#if NFC_CHIP == NFC_CHIP_FM11NC
  fm_nss_low();
  uint8_t addr = reg;
  addr |= 0x20;
  fm_transmit(&addr, 1);
  fm_receive(&val, 1);
  fm_nss_high();
#elif NFC_CHIP == NFC_CHIP_FM11NT
  fm11nt_read(reg, &val, 1);
#endif
  return val;
}

void fm_read_regs(uint16_t reg, uint8_t *buf, uint8_t len) {
#if NFC_CHIP == NFC_CHIP_FM11NC
  fm_nss_low();
  uint8_t addr = reg;
  addr |= 0x20;
  fm_transmit(&addr, 1);
  fm_receive(buf, len);
  fm_nss_high();
#elif NFC_CHIP == NFC_CHIP_FM11NT
  fm11nt_read(reg, buf, len);
#endif
}

void fm_write_reg(uint16_t reg, uint8_t val) {
#if NFC_CHIP == NFC_CHIP_FM11NC
  fm_nss_low();
  uint8_t addr = reg;
  fm_transmit(&addr, 1);
  fm_transmit(&val, 1);
  fm_nss_high();
#elif NFC_CHIP == NFC_CHIP_FM11NT
  fm11nt_write(reg, &val, 1);
#endif
}

void fm_write_regs(uint16_t reg, const uint8_t *buf, uint8_t len) {
#if NFC_CHIP == NFC_CHIP_FM11NC
  fm_nss_low();
  uint8_t addr = reg;
  fm_transmit(&addr, 1);
  fm_transmit(buf, len);
  fm_nss_high();
#elif NFC_CHIP == NFC_CHIP_FM11NT
  fm11nt_write(reg, buf, len);
#endif
}

void fm_read_eeprom(uint16_t addr, uint8_t *buf, uint8_t len) {
#if NFC_CHIP == NFC_CHIP_FM11NC
  fm_nss_low();
  device_delay_us(100);
  uint8_t data[2] = {0x60 | (addr >> 8), addr & 0xFF};
  fm_transmit(data, 2);
  fm_receive(buf, len);
  fm_nss_high();
#elif NFC_CHIP == NFC_CHIP_FM11NT
  fm11nt_read(addr, buf, len);
#endif
}

void fm_write_eeprom(uint16_t addr, const uint8_t *buf, uint8_t len) {
#if NFC_CHIP == NFC_CHIP_FM11NC
  fm_nss_low();
  device_delay_us(100);
  uint8_t data[2] = {0xCE, 0x55};
  fm_transmit(data, 2);
  fm_nss_high();

  device_delay_us(100);

  fm_nss_low();
  data[0] = 0x40 | (addr >> 8);
  data[1] = addr & 0xFF;
  fm_transmit(data, 2);
  fm_transmit(buf, len);
  fm_nss_high();
#elif NFC_CHIP == NFC_CHIP_FM11NT
  fm11nt_write(addr, buf, len);
  device_delay(10);
#endif
}

void fm_read_fifo(uint8_t *buf, uint8_t len) {
#if NFC_CHIP == NFC_CHIP_FM11NC
  fm_nss_low();
  uint8_t addr = 0xA0;
  fm_transmit(&addr, 1);
  fm_receive(buf, len);
  fm_nss_high();
#elif NFC_CHIP == NFC_CHIP_FM11NT
  fm11nt_read(FM_REG_FIFO_ACCESS, buf, len);
#endif
}

void fm_write_fifo(uint8_t *buf, uint8_t len) {
#if NFC_CHIP == NFC_CHIP_FM11NC
  fm_nss_low();
  uint8_t addr = 0x80;
  fm_transmit(&addr, 1);
  fm_transmit(buf, len);
  fm_nss_high();
#elif NFC_CHIP == NFC_CHIP_FM11NT
  fm11nt_write(FM_REG_FIFO_ACCESS, buf, len);
#endif
}

void fm11_init(void) {
#if NFC_CHIP == NFC_CHIP_FM11NC
  uint8_t buf[7];
  uint8_t data1[] = {0x44, 0x00, 0x04, 0x20};
  uint8_t data2[] = {0x05, 0x72, 0x02, 0x00, 0xB3, 0x99, 0x00};
  do {
    fm_write_eeprom(0x3A0, data1, sizeof(data1));
    fm_read_eeprom(0x3A0, buf, sizeof(data1));
  } while (memcmp(data1, buf, sizeof(data1)) != 0);
  do {
    fm_write_eeprom(0x3B0, data2, sizeof(data2));
    fm_read_eeprom(0x3B0, buf, sizeof(data2));
  } while (memcmp(data2, buf, sizeof(data2)) != 0);
#elif NFC_CHIP == NFC_CHIP_FM11NT
  uint8_t crc_buffer[13];
  const uint8_t user_cfg[] = {0x91, 0x82, 0x21, 0xCD};
  const uint8_t atqa_sak[] = {0x44, 0x00, 0x04, 0x20};
  fm_csn_low();
  device_delay_us(200);
  fm_write_eeprom(FM_EEPROM_USER_CFG0, user_cfg, sizeof(user_cfg));
  fm_write_eeprom(FM_EEPROM_ATQA, atqa_sak, sizeof(atqa_sak));
  fm_read_eeprom(FM_EEPROM_SN, crc_buffer, 9);
  DBG_MSG("SN: "); PRINT_HEX(crc_buffer, 9);
  memcpy(crc_buffer + 9, atqa_sak, sizeof(atqa_sak));
  const uint8_t crc8 = fm_crc8(crc_buffer, sizeof(crc_buffer));
  fm_write_eeprom(FM_EEPROM_CRC8, &crc8, 1);
  fm_csn_high();
#endif
}

#if NFC_CHIP == NFC_CHIP_FM11NT

#define I2C_ADDR 0x57

void fm11nt_read(uint16_t addr, uint8_t *buf, uint8_t len) {
  uint8_t slave_id = (I2C_ADDR << 1) | 0;
  i2c_start();
  i2c_write_byte(slave_id);

  // set reg/eeprom addr
  i2c_write_byte(addr >> 8);
  i2c_write_byte(addr & 0xFF);

  // switch to read mode
  slave_id |= 1;
  i2c_start();
  i2c_write_byte(slave_id);

  // master transmit
  for (size_t k = 0; k < len; k++) {
    buf[k] = i2c_read_byte();
    if (k == len - 1) {
      // master sends NACK to slave
      i2c_send_nack();
      // Generate STOP condition
      i2c_stop();
      break;
    } else {
      // master sends ACK to slave
      i2c_send_ack();
    }
    // wait to receive next byte from slave
    scl_delay();
  }
}

void fm11nt_write(const uint16_t addr, const uint8_t *buf, const uint8_t len) {
  const uint8_t slave_id = (I2C_ADDR << 1) | 0;
  i2c_start();
  i2c_write_byte(slave_id);

  // set reg/eeprom addr
  i2c_write_byte(addr >> 8);
  i2c_write_byte(addr & 0xFF);

  // master transmit
  for (size_t i = 0; i < len; i++) {
    // master write a byte to salve and check ACK signal
    i2c_write_byte(buf[i]);
  }
  i2c_stop();
}

uint8_t fm_crc8(const uint8_t *data, const uint8_t data_length) {
  int crc8 = 0xff;
  for (int i = 0; i < data_length; i++) {
    crc8 ^= data[i];
    for (int j = 0; j < 8; j++) {
      if ((crc8 & 0x01) == 0x01)
        crc8 = (crc8 >> 1) ^ 0xb8;
      else
        crc8 >>= 1;
      crc8 &= 0xff;
    }
  }
  return crc8 & 0xff;
}

#endif
