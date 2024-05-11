// SPDX-License-Identifier: Apache-2.0
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include <apdu.h>
#include <crypto-util.h>
#include <bd/lfs_filebd.h>
#include <device.h>
#include <fs.h>
#include <lfs.h>
#include <oath.h>
#include <pass.h>

static void test_helper_resp(uint8_t *data, size_t data_len, uint8_t ins, uint16_t expected_error, uint8_t *expected_resp, size_t resp_len) {
  uint8_t c_buf[1024], r_buf[1024];
  // only tag, no length nor data
  CAPDU C = {.data = c_buf}; RAPDU R = {.data = r_buf};
  CAPDU *capdu = &C;
  RAPDU *rapdu = &R;

  capdu->ins = ins;
  if (ins == OATH_INS_CALCULATE || ins == OATH_INS_SELECT) capdu->p2 = 1;
  if (ins == OATH_INS_SET_DEFAULT) capdu->p1 = 1 + (data_len & 1); // choose one slot
  capdu->lc = data_len;
  if (data_len > 0) {
    // re alloc to help asan find overflow error
    capdu->data = malloc(data_len);
    memcpy(capdu->data, data, data_len);
  } else {
    // when lc = 0, data should never be read
    capdu->data = NULL;
  }

  oath_process_apdu(capdu, rapdu);
  if (data_len > 0) {
    free(capdu->data);
  }
  assert_int_equal(rapdu->sw, expected_error);
  print_hex(RDATA, LL);
  if (expected_resp != NULL) {
    assert_int_equal(rapdu->len, resp_len);
    assert_memory_equal(RDATA, expected_resp, resp_len);
  }
}

static void check_pass_config(bool present, uint8_t slot, uint8_t *data) {
  int i, s;
  uint8_t c_buf[1024], r_buf[1024];
  CAPDU C = {.data = c_buf}; RAPDU R = {.data = r_buf};
  s = pass_read_config(&C, &R);
  assert_int_equal(s, 0);
  print_hex(R.data, R.len);
  printf(" R\n");
  for (i = 0, s = 1; i < R.len; s++) {
    uint8_t ptype, name_len, with_enter;
    uint8_t *name;
    ptype = R.data[i++];
    if (ptype == PASS_SLOT_OATH) {
      name_len = R.data[i++];
      name = &R.data[i];
      i += name_len;
    }
    if (ptype > PASS_SLOT_OFF) with_enter = R.data[i++];
    if (s == slot) {
      if (present) {
        assert_int_equal(ptype, PASS_SLOT_OATH);
        assert_int_equal(name_len, data[1]);
        assert_memory_equal(name, &data[2], name_len);
        assert_int_equal(with_enter, 0);
      } else {
        assert_int_equal(ptype, PASS_SLOT_OFF);
      }
    }
  }
  assert_int_equal(i, R.len);
}

static void test_helper(uint8_t *data, size_t data_len, uint8_t ins, uint16_t expected_error) {
  // don't check resp
  test_helper_resp(data, data_len, ins, expected_error, NULL, 0);
}

static void test_select_ins(void **state) {
  uint8_t c_buf[1024], r_buf[1024];
  CAPDU C = {.data = c_buf}; RAPDU R = {.data = r_buf};
  CAPDU *capdu = &C;
  RAPDU *rapdu = &R;

  INS = OATH_INS_SELECT;
  P1 = 0x04;

  oath_process_apdu(capdu, rapdu);
  assert_int_equal(rapdu->sw, SW_NO_ERROR);
}

static void test_invalid_ins(void **state) {
  test_helper(NULL, 0, 0xDD, 0x6D00);
}

static void test_put(void **state) {
  (void)state;

  uint8_t c_buf[1024], r_buf[1024];
  // name: abc, algo: TOTP+SHA1, digit: 6, key: 0x00 0x01 0x02
  uint8_t data[] = {0x71, 0x03, 'a', 'b', 'c', 0x73, 0x05, 0x21, 0x06, 0x00, 0x01, 0x02};
  CAPDU C = {.data = c_buf}; RAPDU R = {.data = r_buf};
  CAPDU *capdu = &C;
  RAPDU *rapdu = &R;

  capdu->ins = OATH_INS_PUT;
  capdu->data = data;
  capdu->lc = sizeof(data);

  oath_process_apdu(capdu, rapdu);
  assert_int_equal(rapdu->sw, SW_NO_ERROR);

  // duplicated name
  oath_process_apdu(capdu, rapdu);
  assert_int_equal(rapdu->sw, SW_CONDITIONS_NOT_SATISFIED);

  for (int i = 0; i != 10; ++i) {
    data[2] = 'b' + i;
    oath_process_apdu(capdu, rapdu);
    assert_int_equal(rapdu->sw, SW_NO_ERROR);
  }

  // property: increasing-only
  uint8_t data_with_prop[] = {0x71, 0x03, 'i', 'n', 'c', 0x73, 0x05, 0x21, 0x06, 0x00, 0x01, 0x02, 0x78, 0x01};
  capdu->data = data_with_prop;
  capdu->lc = sizeof(data_with_prop);
  oath_process_apdu(capdu, rapdu);
  assert_int_equal(rapdu->sw, SW_NO_ERROR);
}

static void test_hotp_touch(void **state) {
  // name: H1, algo: HOTP+SHA1, digit: 6, key in base32: JBSWY3DPEHPK3PXP
  uint8_t data[] = {
    OATH_TAG_NAME, 0x02, 'H', '1',
    OATH_TAG_KEY, 0x0c, 0x11, 0x06, 'H', 'e', 'l', 'l', 'o', '!', 0xDE, 0xAD, 0xBE, 0xEF,
  };
  // name: H1n, algo: HOTP+SHA1, digit: 8, key in base32: JBSWY3DPEHPK3PXP
  uint8_t data8[] = {
    OATH_TAG_NAME, 0x03, 'H', '1', 'n',
    OATH_TAG_KEY, 0x0c, 0x11, 0x08, 'H', 'e', 'l', 'l', 'o', '!', 0xDE, 0xAD, 0xBE, 0xEF,
    OATH_TAG_COUNTER, 0x04, 0x00, 0x00, 0x00, 0x02,
  };
  const char * codes[] = {
    "996554", "602287", "143627"
  };
  const char * codes8[] = {
    "41996554", "88602287", "91143627",
    "05960129", "38768897", "68883951",
  };
  int ret;
  char buf[9];

  // add an record w/o initial counter value
  test_helper(data, sizeof(data), OATH_INS_PUT, SW_NO_ERROR);

  test_helper(data, 4, OATH_INS_SET_DEFAULT, SW_NO_ERROR);
  check_pass_config(true, 1, data);

  for (int i = 0; i < 3; i++) {
    ret = pass_handle_touch(TOUCH_SHORT, buf);
    assert_int_equal(ret, 6);
    buf[ret] = '\0';
    assert_string_equal(buf, codes[i]);
  }

  test_helper(data, 4, OATH_INS_DELETE, SW_NO_ERROR);
  check_pass_config(false, 1, data);

  ret = pass_handle_touch(TOUCH_SHORT, buf);
  assert_int_equal(ret, 0);

  // add an record w/ initial counter value
  test_helper(data8, sizeof(data8), OATH_INS_PUT, SW_NO_ERROR);
  test_helper(data8, 5, OATH_INS_SET_DEFAULT, SW_NO_ERROR);
  check_pass_config(true, 2, data8);

  for (int i = 2; i < 6; i++) {
    ret = pass_handle_touch(TOUCH_LONG, buf);
    assert_int_equal(ret, 8);
    buf[ret] = '\0';
    // printf("code[%d]: %s\n", i+1, buf);
    assert_string_equal(buf, codes8[i]);
  }

  ret = pass_handle_touch(TOUCH_SHORT, buf);
  assert_int_equal(ret, 0);

  ret = pass_handle_touch(199, buf);
  assert_int_equal(ret, -1);

  uint8_t rfc4226example[] = {
    OATH_TAG_NAME, 0x05, '.', '4', '2', '2', '6',
    OATH_TAG_KEY, 22, 0x11, 0x06, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30
  };
  const char * results[] = {
    "755224",
    "287082",
    "359152",
    "969429",
    "338314",
    "254676",
    "287922",
    "162583",
    "399871",
    "520489",
    "403154",
  };
  test_helper(rfc4226example, sizeof(rfc4226example), OATH_INS_PUT, SW_NO_ERROR);
  test_helper(rfc4226example, 7, OATH_INS_SET_DEFAULT, SW_NO_ERROR);
  check_pass_config(true, 2, rfc4226example);

  ret = pass_handle_touch(TOUCH_SHORT, buf);
  assert_int_equal(ret, 0);
  
  for (int i = 1; i <= 10; i++) {
    ret = pass_handle_touch(TOUCH_LONG, buf);
    assert_int_equal(ret, 6);
    buf[ret] = '\0';
    // printf("code[%d]: %s\n", i, buf);
    assert_string_equal(buf, results[i]);
  }
  test_helper(rfc4226example, 7, OATH_INS_DELETE, SW_NO_ERROR);
  check_pass_config(false, 2, rfc4226example);
}

static void test_static_pass(void **state) {
  int len, ret;
  uint8_t c_buf[1024], r_buf[1024];
  const char static_pass[PASS_MAX_PASSWORD_LENGTH+1] = 
    "a0aaa0a0a0aaaaa0a0a00a0a0bbabba0";
  char readback[PASS_MAX_PASSWORD_LENGTH*2];
  CAPDU C = {.data = c_buf}; RAPDU R = {.data = r_buf};
  CAPDU *capdu = &C;
  RAPDU *rapdu = &R;

  P1 = 2;
  c_buf[0] = PASS_SLOT_STATIC;
  c_buf[1] = sizeof(static_pass);
  memcpy(c_buf+2, static_pass, c_buf[1]);
  c_buf[c_buf[1]+2] = 0;
  LC = c_buf[1]+3;
  pass_write_config(&C, &R);
  assert_int_equal(SW, SW_WRONG_LENGTH);

  len = c_buf[1] = PASS_MAX_PASSWORD_LENGTH;
  memcpy(c_buf+2, static_pass, c_buf[1]);
  c_buf[c_buf[1]+2] = 0;
  LC = c_buf[1]+3;
  ret = pass_write_config(&C, &R);
  assert_int_equal(ret, 0);

  ret = pass_handle_touch(TOUCH_LONG, readback);
  assert_int_equal(ret, len);
  assert_memory_equal(readback, static_pass, len);

  c_buf[c_buf[1]+2] = 1;
  LC = c_buf[1]+3;
  ret = pass_write_config(&C, &R);
  assert_int_equal(ret, 0);

  ret = pass_handle_touch(TOUCH_LONG, readback);
  assert_int_equal(ret, len+1);
  assert_memory_equal(readback, static_pass, len);
  assert_int_equal(readback[len], '\r');

  pass_install(0); // reload from file
  
  ret = pass_handle_touch(TOUCH_LONG, readback);
  assert_int_equal(ret, len+1);
  assert_memory_equal(readback, static_pass, len);
  assert_int_equal(readback[len], '\r');
}

// should be called after test_put
static void test_calc(void **state) {
  (void)state;

  uint8_t data[] = {
    // name
    OATH_TAG_NAME, 0x03, 'a', 'b', 'c',
    // challenge: 0x21 0x06 0x00 0x01 0x02
    OATH_TAG_CHALLENGE, 0x05, 0x21, 0x06, 0x00, 0x01, 0x02};
  uint8_t resp[] = {
    // hmac: cbba68f6d4c567bc4b0ffff136befc3d2d86231a
    // part of hmac:             fff136be
    // mask:                     7f000000
    OATH_TAG_RESPONSE, 0x05, 0x06, 0x7F, 0xF1, 0x36, 0xBE};
  test_helper_resp(data, sizeof(data), OATH_INS_CALCULATE, SW_NO_ERROR, resp, sizeof(resp));

  data[sizeof(data)-1] = 1; // decrease the value of challenge
  test_helper(data, sizeof(data), OATH_INS_CALCULATE, SW_NO_ERROR);

  // length of data exceeds the Lc
  test_helper(data, sizeof(data) - 1, OATH_INS_CALCULATE, SW_WRONG_LENGTH);
  test_helper(data, 1, OATH_INS_CALCULATE, SW_WRONG_LENGTH);
  test_helper(data, 2, OATH_INS_CALCULATE, SW_WRONG_LENGTH);

  // omit the TAG_CHALLENGE
  test_helper(data, 5, OATH_INS_CALCULATE, SW_WRONG_LENGTH);

  // zero-length challenge
  data[6] = 0;
  test_helper(data, sizeof(data), OATH_INS_CALCULATE, SW_WRONG_DATA);

  data[6] = MAX_CHALLENGE_LEN + 1;
  test_helper(data, sizeof(data), OATH_INS_CALCULATE, SW_WRONG_DATA);
}


// should be called after test_put
static void test_increasing_only(void **state) {
  (void)state;

  uint8_t data[] = {
    OATH_TAG_NAME, 0x03, 'i', 'n', 'c',
    OATH_TAG_CHALLENGE, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};

  test_helper(data, sizeof(data), OATH_INS_CALCULATE, SW_NO_ERROR);

  data[sizeof(data)-1] = 1; // decrease the value of challenge
  test_helper(data, sizeof(data), OATH_INS_CALCULATE, SW_SECURITY_STATUS_NOT_SATISFIED);

  data[sizeof(data)-1] = 2;
  test_helper(data, sizeof(data), OATH_INS_CALCULATE, SW_NO_ERROR);

  data[sizeof(data)-1] = 3;
  test_helper(data, sizeof(data), OATH_INS_CALCULATE, SW_NO_ERROR);

  data[sizeof(data)-1] = 2;
  test_helper(data, sizeof(data), OATH_INS_CALCULATE, SW_SECURITY_STATUS_NOT_SATISFIED);
}

static void test_list(void **state) {
  (void)state;

  uint8_t c_buf[1024], r_buf[1024];
  CAPDU C = {.data = c_buf}; RAPDU R = {.data = r_buf};
  CAPDU *capdu = &C;
  RAPDU *rapdu = &R;

  capdu->ins = OATH_INS_LIST;
  capdu->lc = 0;
  capdu->le = 64;

  oath_process_apdu(capdu, rapdu);
  assert_int_equal(rapdu->sw, 0x61FF);
  print_hex(RDATA, LL);

  capdu->ins = OATH_INS_SEND_REMAINING;
  capdu->le = 0xFF;
  oath_process_apdu(capdu, rapdu);
  assert_int_equal(rapdu->sw, SW_NO_ERROR);
  print_hex(RDATA, LL);
}

static void test_calc_all(void **state) {
  (void)state;

  uint8_t c_buf[1024], r_buf[1024];
  uint8_t data[] = {0x74, 0x08, 0x00, 0x00, 0x00, 0x21, 0x06, 0x00, 0x01, 0x03};
  CAPDU C = {.data = c_buf}; RAPDU R = {.data = r_buf};
  CAPDU *capdu = &C;
  RAPDU *rapdu = &R;

  capdu->ins = OATH_INS_SELECT;
  capdu->data = data;
  capdu->p2 = 1;
  capdu->lc = sizeof(data);
  capdu->le = 64;

  oath_process_apdu(capdu, rapdu);
  assert_int_equal(rapdu->sw, 0x61FF);
  print_hex(RDATA, LL);

  capdu->ins = OATH_INS_SEND_REMAINING;
  capdu->p2 = 0;
  capdu->le = 0xFF;
  oath_process_apdu(capdu, rapdu);
  assert_int_equal(rapdu->sw, SW_NO_ERROR);
  print_hex(RDATA, LL);

  // length of data exceeds the Lc
  test_helper(data, sizeof(data) - 1, OATH_INS_SELECT, SW_WRONG_LENGTH);
  test_helper(data, 1, OATH_INS_SELECT, SW_WRONG_LENGTH);
  test_helper(data, 2, OATH_INS_SELECT, SW_WRONG_LENGTH);

  // zero-length challenge
  data[1] = 0;
  test_helper(data, sizeof(data), OATH_INS_CALCULATE, SW_WRONG_DATA);

  data[1] = MAX_CHALLENGE_LEN + 1;
  test_helper(data, sizeof(data), OATH_INS_CALCULATE, SW_WRONG_DATA);
}

// regression tests for crashes discovered by fuzzing
static void test_regression_fuzz(void **state) {
  (void)state;

  if (1) {
    // put only tag, no length nor data
    uint8_t data[] = {OATH_TAG_NAME};
    test_helper(data, sizeof(data), OATH_INS_PUT, SW_WRONG_LENGTH);
  }

  if (1) {
    // put with broken HOTP tag
    uint8_t data[] = {
      // name tag
      OATH_TAG_NAME, 0x01, 0x20,
      // key tag
      OATH_TAG_KEY, 0x03, 0x11, 0x04, 0x00,
      // HOTP tag
      OATH_TAG_COUNTER, 0x04};
    test_helper(data, sizeof(data), OATH_INS_PUT, SW_WRONG_LENGTH);
  }

  if (1) {
    // delete with only name tag
    uint8_t data[] = {OATH_TAG_NAME};
    test_helper(data, sizeof(data), OATH_INS_DELETE, SW_WRONG_LENGTH);
  }

  if (1) {
    // calculate with only name tag
    uint8_t data[] = {OATH_TAG_NAME};
    test_helper(data, sizeof(data), OATH_INS_CALCULATE, SW_WRONG_LENGTH);
  }

  if (1) {
    // set default with only name tag
    uint8_t data[] = {OATH_TAG_NAME};
    test_helper(data, sizeof(data), OATH_INS_SET_DEFAULT, SW_WRONG_LENGTH);
  }

  if (1) {
    // put with empty key tag
    uint8_t data[] = {OATH_TAG_NAME, 0x01, 0x00, OATH_TAG_KEY, 3};
    test_helper(data, sizeof(data), OATH_INS_PUT, SW_WRONG_LENGTH);
  }
}

static void test_put_long_key(void **state) {
  (void)state;

  // put with too long key length(0xFF)
  uint8_t data[] = {
    // name tag
    OATH_TAG_NAME, 0x01, 0x20,
    // key tag
    OATH_TAG_KEY, 0xff, 0x11, 0x10, 0x00};
  test_helper(data, sizeof(data), OATH_INS_PUT, SW_WRONG_DATA);
}

static void test_put_unsupported_algo(void **state) {
  (void)state;

  // put with wrong algo(0x0)
  uint8_t data[] = {
    // name tag
    OATH_TAG_NAME, 0x01, 0x20,
    // key tag
    OATH_TAG_KEY, 0x03, 0x00, 0x10, 0x00};
  test_helper(data, sizeof(data), OATH_INS_PUT, SW_WRONG_DATA);
}

static void test_put_unsupported_counter(void **state) {
  (void)state;

  // put with unsupported counter type(except HOTP)
  uint8_t data[] = {
    // name tag
    OATH_TAG_NAME, 0x01, 0x20,
    // key tag (TOTP + SHA1)
    OATH_TAG_KEY, 0x03, 0x21, 0x10, 0x00,
    // HOTP tag
    OATH_TAG_COUNTER, 0x04, 0x00, 0x00, 0x00, 0x00};
  test_helper(data, sizeof(data), OATH_INS_PUT, SW_WRONG_DATA);
}

static void test_space_full(void **state) {
  (void)state;

  uint8_t c_buf[128], r_buf[128];
  // name: abc, algo: TOTP+SHA1, digit: 6, key: 0x00 0x01 0x02
  uint8_t data[] = {0x71, 0x03, 'A', '-', '0', 0x73, 0x05, 0x21, 0x06, 0x00, 0x01, 0x02, 0x78, OATH_PROP_TOUCH};
  CAPDU C = {.data = c_buf}; RAPDU R = {.data = r_buf};
  CAPDU *capdu = &C;
  RAPDU *rapdu = &R;

  capdu->ins = OATH_INS_PUT;
  capdu->data = data;
  capdu->lc = sizeof(data);

  // make it full
  int record_added = 0;
  for (int i = 0; i != 100; ++i) {
    data[2] = ' ' + i;
    oath_process_apdu(capdu, rapdu);
    if (rapdu->sw != SW_NO_ERROR) break;
    record_added++;
  }
  assert_int_equal(rapdu->sw, SW_NOT_ENOUGH_SPACE);

  memcpy(c_buf, data, sizeof(data));
  c_buf[2] = ' '; // delete the first one we put
  test_helper(c_buf, sizeof(data), OATH_INS_DELETE, SW_NO_ERROR);

  // then try again
  oath_process_apdu(capdu, rapdu);
  assert_int_equal(rapdu->sw, SW_NO_ERROR);

  // leave some space for further tests
  for (int i = 1; i != 20; ++i) {
    c_buf[2] = ' ' + i;
    test_helper(c_buf, sizeof(data), OATH_INS_DELETE, SW_NO_ERROR);
  }
}

int main() {
  struct lfs_config cfg;
  lfs_filebd_t bd;
  struct lfs_filebd_config bdcfg = {.read_size = 1, .prog_size = 512, .erase_size = 512, .erase_count = 256};
  bd.cfg = &bdcfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.context = &bd;
  cfg.read = &lfs_filebd_read;
  cfg.prog = &lfs_filebd_prog;
  cfg.erase = &lfs_filebd_erase;
  cfg.sync = &lfs_filebd_sync;
  cfg.read_size = 1;
  cfg.prog_size = 512;
  cfg.block_size = 512;
  cfg.block_count = 256;
  cfg.block_cycles = 50000;
  cfg.cache_size = 512;
  cfg.lookahead_size = 32;
  lfs_filebd_create(&cfg, "lfs-root", &bdcfg);

  fs_format(&cfg);
  fs_mount(&cfg);
  oath_install(1);
  pass_install(1);

  const struct CMUnitTest tests[] = {
      cmocka_unit_test(test_select_ins),
      cmocka_unit_test(test_invalid_ins),
      cmocka_unit_test(test_put),
      cmocka_unit_test(test_put_long_key),
      cmocka_unit_test(test_put_unsupported_algo),
      cmocka_unit_test(test_put_unsupported_counter),
      cmocka_unit_test(test_calc),
      cmocka_unit_test(test_increasing_only),
      cmocka_unit_test(test_list),
      cmocka_unit_test(test_calc_all),
      cmocka_unit_test(test_hotp_touch),
      cmocka_unit_test(test_static_pass),
      cmocka_unit_test(test_space_full),
      cmocka_unit_test(test_regression_fuzz),
  };

  int ret = cmocka_run_group_tests(tests, NULL, NULL);

  lfs_filebd_destroy(&cfg);

  return ret;
}
