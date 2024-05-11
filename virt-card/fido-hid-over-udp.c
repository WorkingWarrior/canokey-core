// Copyright 2019 SoloKeys Developers
//
// Licensed under the Apache License, Version 2.0, <LICENSE-APACHE or
// http://apache.org/licenses/LICENSE-2.0> or the MIT license <LICENSE-MIT or
// http://opensource.org/licenses/MIT>, at your option. This file may not be
// copied, modified, or distributed except according to those terms.

#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "device.h"
#include "ctaphid.h"
#include "fabrication.h"
#include "applets.h"

static int udp_server() {
  static bool run_already = false;
  static int fd = -1;
  if (run_already && fd >= 0) return fd;
  run_already = true;

  if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    perror("socket failed");
    return 1;
  }

  int flags = fcntl(fd, F_GETFD);
  flags |= FD_CLOEXEC;
  fcntl(fd, F_SETFD, flags);

  struct timeval read_timeout;
  read_timeout.tv_sec = 0;
  read_timeout.tv_usec = 10;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof(struct timeval)) != 0) {
    perror("setsockopt");
    exit(1);
  }

  int reuseaddr = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr)) != 0) {
    perror("setsockopt");
    exit(1);
  }

  struct sockaddr_in serveraddr;
  memset(&serveraddr, 0, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET;
  serveraddr.sin_port = htons(8111);
  serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(fd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0) {
    perror("bind failed");
    exit(1);
  }
  return fd;
}

static int udp_recv(int fd, uint8_t *buf, int size) {

  fd_set input;
  FD_ZERO(&input);
  FD_SET(fd, &input);
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 100;
  int n = select(fd + 1, &input, NULL, NULL, &timeout);
  if (n == -1) {
    perror("select\n");
    exit(1);
  } else if (n == 0)
    return 0;
  if (!FD_ISSET(fd, &input)) {
  }
  int length = recvfrom(fd, buf, size, 0, NULL, 0);
  if (length < 0) {
    perror("recvfrom failed");
    exit(1);
  }
  return length;
}

static void udp_send(int fd, uint8_t *buf, int size) {
  struct sockaddr_in serveraddr;
  memset(&serveraddr, 0, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET;
  serveraddr.sin_port = htons(7112);
  serveraddr.sin_addr.s_addr = htonl(0x7f000001); // (127.0.0.1)

  if (sendto(fd, buf, size, 0, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0) {
    perror("sendto failed");
    exit(1);
  }
}

static int current_fd;
static uint8_t udp_send_current_fd(uint8_t *report, uint16_t len) {
  // printf("udp_send_current_fd %hu\n", len);
  udp_send(current_fd, report, len);
  return 0;
}

static void emulate_reboot(void) {
  testmode_set_initial_ticks(0);
  testmode_set_initial_ticks(device_get_tick());
  applets_install();
}

int main() {
  current_fd = udp_server();
  card_fabrication_procedure("lfs-root");
  // emulate the NFC mode, where user-presence tests are skipped
  set_nfc_state(1);
  ctap_hid_init(udp_send_current_fd);
  emulate_reboot();
  for (;;) {
    uint8_t buf[HID_RPT_SIZE];
    int length = udp_recv(current_fd, buf, sizeof(buf));
    if (length > 0) {
      // printf("udp_recv %d\n", length);
      uint8_t magic_cmd[] = "\xac\x10\x52\xca\x95\xe5\x69\xde\x69\xe0\x2e\xbf"
                            "\xf3\x33\x48\x5f\x13\xf9\xb2\xda\x34\xc5\xa8\xa3"
                            "\x40\x52\x66\x97\xa9\xab\x2e\x0b\x39\x4d\x8d\x04"
                            "\x97\x3c\x13\x40\x05\xbe\x1a\x01\x40\xbf\xf6\x04"
                            "\x5b\xb2\x6e\xb7\x7a\x73\xea\xa4\x78\x13\xf6\xb4"
                            "\x9a\x72\x50\xdc";
      if (memcmp(magic_cmd, buf, 64) == 0) {
        printf("MAGIC REBOOT command received!\r\n");
        // exit(0);
        emulate_reboot();
        continue;
        // close(current_fd);
        // char *const argv[] = {"fido-hid-over-udp", NULL};
        // int ret = execv("/proc/self/exe", argv);
        // printf("ERROR exec %d", ret);
        // return 0;
      } else if (length > 14 && memcmp(buf, "\x99\x10\x52\xca\x95\xe5\x69\xde\x69\xe0\x2e\xbf", 12) == 0) {
        uint8_t *data = buf + 12;
        testmode_inject_error(data[0], data[1], length-14, data+2);
        continue;
      }
      ctap_hid_set_report_cb(0, HID_REPORT_TYPE_INVALID, buf, length);
    }
    ctap_hid_loop(0);
  }
  return 0;
}
