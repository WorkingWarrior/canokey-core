/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CANOKEY_CORE_INCLUDE_PASS_H
#define CANOKEY_CORE_INCLUDE_PASS_H

#include <apdu.h>

#define PASS_MAX_PASSWORD_LENGTH 32

int pass_install(uint8_t reset);
int pass_read_config(const CAPDU *capdu, RAPDU *rapdu);
int pass_write_config(const CAPDU *capdu, RAPDU *rapdu);
int pass_handle_touch(uint8_t touch_type, char *output);

#endif // CANOKEY_CORE_INCLUDE_PASS_H
