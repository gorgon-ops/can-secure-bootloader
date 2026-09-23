/**
 * @file    flash_stm32.h
 * @brief   Hardware flash operations for flash_writer. See flash_stm32.c for
 *          the erase-versus-watchdog constraint that decides where these may
 *          be called from.
 */
#ifndef FLASH_STM32_H
#define FLASH_STM32_H

#include "flash_writer.h"

/** @brief Fill in the erase and program callbacks for this part. */
void FlashStm32_GetOps(flash_ops_t *ops);

#endif /* FLASH_STM32_H */
