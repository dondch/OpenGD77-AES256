/* Host-test stand-in for the firmware SPI_Flash.h: only what dmr_aes_hook.c uses. */
#ifndef TEST_STUB_SPI_FLASH_H
#define TEST_STUB_SPI_FLASH_H
#include <stdbool.h>
#include <stdint.h>

bool SPI_Flash_read(uint32_t addrress, uint8_t *buf, int size);
bool SPI_Flash_write(uint32_t addr, uint8_t *dataBuf, int size);
#endif
