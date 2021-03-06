/*
 * ws2812b.h
 *
 *  Created on: Apr 11, 2019
 *      Author: kz
 */

#ifndef WS2812B_H_
#define WS2812B_H_

#include <stdio.h>

#include "driver/spi_master.h"

#include "common.h"
#include "rgb_color.h"

void spiAfterCallback(spi_transaction_t *);

int convertRgb2Bits(unsigned char *rgbBuff,
					unsigned char *ledBuff,
					int leds);



#endif /* MAIN_WS2812B_H_ */
