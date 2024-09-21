/*
 * USBSID-Pico is a RPi Pico (RP2040) based board for interfacing one or two
 * MOS SID chips and/or hardware SID emulators over (WEB)USB with your computer,
 * phone or ASID supporting player
 *
 * gpio.c
 * This file is part of USBSID-Pico (https://github.com/LouDnl/USBSID-Pico)
 * File author: LouD
 *
 * Copyright (c) 2024 LouD
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "usbsid.h"

void initPins()
{

  /* GPIO defaults for PIO bus */
  gpio_set_dir(RES, GPIO_OUT);
  gpio_set_function(RES, GPIO_FUNC_SIO);
  gpio_put(RES, 0);
  gpio_put(RES, 1);  /* RESET TO HIGH */
  gpio_init(CS1);
  gpio_init(CS2);
  gpio_init(RW);
  gpio_put(CS1, 1);
  gpio_put(CS2, 1);
  gpio_put(RW, 0);
  gpio_set_dir(CS1, GPIO_OUT);
  gpio_set_dir(CS2, GPIO_OUT);
  gpio_set_dir(RW, GPIO_OUT);

}

void initVUE(void)
{
  /* PWM led */
  gpio_init( BUILTIN_LED );
  gpio_set_dir(BUILTIN_LED, GPIO_OUT);
  gpio_set_function(BUILTIN_LED, GPIO_FUNC_PWM);
  /* Init Vue */
  int led_pin_slice = pwm_gpio_to_slice_num( BUILTIN_LED );
	pwm_config configLED = pwm_get_default_config();
	pwm_config_set_clkdiv( &configLED, 1 );
	pwm_config_set_wrap( &configLED, 65535 );  /* LED max */
	pwm_init( led_pin_slice, &configLED, true );
	gpio_set_drive_strength( BUILTIN_LED, GPIO_DRIVE_STRENGTH_2MA );
	pwm_set_gpio_level( BUILTIN_LED, 0 );  /* turn off led */
}

/* PIO, StateMachines & DMA */
void setupPioBus() {

  /* control bus */
  {
    offset_control = pio_add_program(pio, &bus_control_program);
    sm_control = pio_claim_unused_sm(pio, true);
    for (uint i = RW; i < CS2 + 1; ++i)
      pio_gpio_init(pio, i);
    pio_sm_config c_control = bus_control_program_get_default_config(offset_control);
    sm_config_set_out_pins(&c_control, RW, 3);
    sm_config_set_in_pins(&c_control, D0);
    sm_config_set_jmp_pin(&c_control, RW);
    pio_sm_init(pio, sm_control, offset_control, &c_control);
    pio_sm_set_enabled(pio, sm_control, true);
  }

  /* databus */
  {
    offset_data = pio_add_program(pio, &data_bus_program);
    sm_data = pio_claim_unused_sm(pio, true);
    for (uint i = D0; i < A5 + 1; ++i) {
      pio_gpio_init(pio, i);
    }
    pio_sm_config c_data = data_bus_program_get_default_config(offset_data);
    pio_sm_set_pindirs_with_mask(pio, sm_data, PIO_PINDIRMASK, PIO_PINDIRMASK);  /* WORKING */
    sm_config_set_out_pins(&c_data, D0, A5 + 1);
    sm_config_set_fifo_join(&c_data, PIO_FIFO_JOIN_TX);
    pio_sm_init(pio, sm_data, offset_data, &c_data);
    pio_sm_set_enabled(pio, sm_data, true);
  }

  /* dma controlbus */
  {
    dma_tx_control = dma_claim_unused_channel(true);
    dma_channel_config tx_config_control = dma_channel_get_default_config(dma_tx_control);
    channel_config_set_transfer_data_size(&tx_config_control, DMA_SIZE_16);
    channel_config_set_read_increment(&tx_config_control, true);
    channel_config_set_write_increment(&tx_config_control, false);
    channel_config_set_dreq(&tx_config_control, pio_get_dreq(pio, sm_control, true));
    dma_channel_configure(dma_tx_control, &tx_config_control, &pio->txf[sm_control], NULL, 1, false);
  }

  /* dma tx databus */
  {
    dma_tx_data = dma_claim_unused_channel(true);
    dma_channel_config tx_config_data = dma_channel_get_default_config(dma_tx_data);
    channel_config_set_transfer_data_size(&tx_config_data, DMA_SIZE_32);
    channel_config_set_read_increment(&tx_config_data, true);
    channel_config_set_write_increment(&tx_config_data, false);
    channel_config_set_dreq(&tx_config_data, pio_get_dreq(pio, sm_data, true));
    dma_channel_configure(dma_tx_data, &tx_config_data, &pio->txf[sm_data], NULL, 1, false);
  }

  /* dma rx databus */
  {
    dma_rx_data = dma_claim_unused_channel(true);
    dma_channel_config rx_config = dma_channel_get_default_config(dma_rx_data);
    channel_config_set_transfer_data_size(&rx_config, DMA_SIZE_32);
    channel_config_set_read_increment(&rx_config, false);
    channel_config_set_write_increment(&rx_config, true);
    channel_config_set_dreq(&rx_config, pio_get_dreq(pio, sm_control, false));
    dma_channel_configure(dma_rx_data, &rx_config, NULL, &pio->rxf[sm_control], 1, false);
  }

}

uint8_t __not_in_flash_func(bus_operation)(uint8_t command, uint8_t address, uint8_t data)
{
  if ((command & 0xF0) != 0x10) {
      return 0; // Sync bit not set, ignore operation
  }
  bool is_read = (command & 0x0F) == 0x01;
  switch (command & 0x0F) {
    case PAUSE:
      control_word = 0b110110;
      dma_channel_set_read_addr(dma_tx_control, &control_word, true); /* Control lines RW, CS1 & CS2 DMA transfer */
      break;
    case CLEAR_BUS:
      dir_mask = 0b1111111111111111;
      data_word = (dir_mask << 16) | 0x0;
      dma_channel_set_read_addr(dma_tx_data, &data_word, true); /* Data & Address DMA transfer */
      break;
    case WRITE:
    case READ:
    default:
      control_word = 0b110000;
      data_word = (address & 0x3F) << 8 | data;
      dir_mask = 0x0;
      dir_mask |= (is_read ? 0b1111111100000000 : 0b1111111111111111);
      data_word = (dir_mask << 16) | data_word;
      control_word |= (is_read ? 1 : 0);
      switch (address) {
        case 0x0 ... 0x39:
          control_word |= 1 << 2; // CS1 low, CS2 high
          break;
        case 0x40 ... 0x79:
          control_word |= 1 << 1; // CS1 high, CS2 low
          break;
      }
      dma_channel_set_read_addr(dma_tx_data, &data_word, true); /* Data & Address DMA transfer */
      dma_channel_set_read_addr(dma_tx_control, &control_word, true); /* Control lines RW, CS1 & CS2 DMA transfer */
      break;
  }

  switch (command & 0x0F) {
    case READ:
      read_data = 0x0;
      dma_channel_set_write_addr(dma_rx_data, &read_data, true);
      dma_channel_wait_for_finish_blocking(dma_rx_data);
      GPIODBG("[R]$%08x 0b"PRINTF_BINARY_PATTERN_INT32" $%04x 0b"PRINTF_BINARY_PATTERN_INT16"\r\n", read_data, PRINTF_BYTE_TO_BINARY_INT32(read_data), control_word, PRINTF_BYTE_TO_BINARY_INT16(control_word));
      return (read_data >> 24) & 0xF;
    case PAUSE:
    case WRITE:
      dma_channel_wait_for_finish_blocking(dma_tx_control);
      GPIODBG("[W]$%08x 0b"PRINTF_BINARY_PATTERN_INT32" $%04x 0b"PRINTF_BINARY_PATTERN_INT16"\r\n", data_word, PRINTF_BYTE_TO_BINARY_INT32(data_word), control_word, PRINTF_BYTE_TO_BINARY_INT16(control_word));
      return 0;
    case CLEAR_BUS:
      /* don't wait, just fall through */
    default:
      return 0;
  }

  /* Start DMA transfers */
  // dma_channel_set_read_addr(dma_tx_data, &data_word, true); /* Data & Address */
  // dma_channel_set_read_addr(dma_tx_control, &control_word, true); /* Control lines RW, CS1 & CS2 DMA transfer */
  // if (is_read) {
  //     read_data = 0x0;
  //     dma_channel_set_write_addr(dma_rx_data, &read_data, true);
  //     dma_channel_wait_for_finish_blocking(dma_rx_data);
  //     GPIODBG("[R]$%08x 0b"PRINTF_BINARY_PATTERN_INT32" $%04x 0b"PRINTF_BINARY_PATTERN_INT16"\r\n", read_data, PRINTF_BYTE_TO_BINARY_INT32(read_data), control_word, PRINTF_BYTE_TO_BINARY_INT16(control_word));
  //     return (read_data >> 24) & 0xF;
  // } else {
  //     dma_channel_wait_for_finish_blocking(dma_tx_control);
  //     GPIODBG("[W]$%08x 0b"PRINTF_BINARY_PATTERN_INT32" $%04x 0b"PRINTF_BINARY_PATTERN_INT16"\r\n", data_word, PRINTF_BYTE_TO_BINARY_INT32(data_word), control_word, PRINTF_BYTE_TO_BINARY_INT16(control_word));
  //     return 0;
  // }
}

void clearBus(void)
{
  bus_operation((0x10 | CLEAR_BUS), 0x0, 0x0);
}

void pauseSID(void)
{
  bus_operation((0x10 | PAUSE), 0x0, 0x0);
}

void resetSID(void)
{
  disableSID();
  clearBus();
  sleep_us(1);
  enableSID();
}

void enableSID(void)
{
  gpio_put(RES, 1);
  for (int i = 0; i < NUMSIDS; i++) {
    bus_operation((0x10 | WRITE), ((0x20 * i) + 0x18), 0x0F);  /* Volume to 0 */
  }
}

void disableSID(void)
{
  for (int i = 0; i < NUMSIDS; i++) {
    bus_operation((0x10 | WRITE), ((0x20 * i) + 0x18), 0x0);  /* Volume to 0 */
  }
  gpio_put(CS1, 1);
  gpio_put(CS2, 1);
  gpio_put(RES, 0);
}
