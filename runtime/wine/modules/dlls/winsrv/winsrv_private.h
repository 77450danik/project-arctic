/*
 * Arctic window server
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_WINSRV_PRIVATE_H
#define __WINE_WINSRV_PRIVATE_H

void start_raw_input_thread(void);
void start_hung_app_thread(void);
void print_screen(void);
void screen_snip(void);
void set_input_language( HKL layout );
HKL get_input_language(void);

#endif
