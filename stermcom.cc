/****************************************************************************
 * stermcom.cc
 *
 *   Copyright (c) 2015 Yoshinori Sugino
 *   This software is released under the MIT License.
 ****************************************************************************/
#include <fcntl.h>
#include <libgen.h>
#include <sys/select.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>

#include "common_type.h"
#include "debug.h"
#include "file_descriptor.h"
#include "signal_settings.h"
#include "terminal_interface.h"

namespace {

using status_t = common::status_t;

volatile sig_atomic_t g_should_continue = 1;

struct Arguments {
  uint32_t baud_rate;
};

status_t mainLoop(const int32_t &tty_fd, const Arguments &args) {
  fd_set fds_r, fds_w;
  uint8_t one_char;
  std::list<uint8_t> string_buffer{};
  ssize_t rw_size;
  util::TerminalInterface stdin_term(STDIN_FILENO), tty_term(tty_fd);

  if (stdin_term.SetRawMode() == status_t::kFailure)
    return status_t::kFailure;
  if (tty_term.SetRawMode() == status_t::kFailure)
    return status_t::kFailure;
  if (tty_term.SetBaudRate(args.baud_rate, util::direction_t::kOut) ==
      status_t::kFailure)
    return status_t::kFailure;
  if (tty_term.SetBaudRate(0, util::direction_t::kIn) == status_t::kFailure)
    return status_t::kFailure;

  assert(tty_fd > STDIN_FILENO && "Assertion for select()");

  while (g_should_continue) {
    FD_ZERO(&fds_r);
    FD_ZERO(&fds_r);

    FD_SET(STDIN_FILENO, &fds_r);
    FD_SET(tty_fd, &fds_r);
    FD_SET(tty_fd, &fds_w);

    errno = 0;
    auto ret = select(tty_fd + 1, &fds_r, &fds_w, nullptr, nullptr);
    if (ret == -1) {
      if (errno == EINTR) {
        DEBUG_PRINTF("Signal was caught when select() is waiting");
      } else {
        printf("Error\n");
        return status_t::kFailure;
      }
      break;
    }

    if (FD_ISSET(STDIN_FILENO, &fds_r)) {
      rw_size = read(STDIN_FILENO, &one_char, 1);
      if (one_char == 0x18) break;  // 0x18: Ctrl-x
      if (rw_size > 0) string_buffer.push_back(one_char);
    }
    if (FD_ISSET(tty_fd, &fds_w)) {
      if (!string_buffer.empty()) {
        rw_size = write(tty_fd, &string_buffer.front(), 1);
        if (rw_size > 0) string_buffer.pop_front();
      }
    }
    if (FD_ISSET(tty_fd, &fds_r)) {
      uint8_t tty_read_buffer;
      rw_size = read(tty_fd, &tty_read_buffer, 1);
      if (rw_size > 0) write(STDOUT_FILENO, &tty_read_buffer, 1);
    }
  }

  return status_t::kSuccess;
}

}  // namespace

int main(int argc, char *argv[]) {
  if (argc != 3) {
    auto path_name = argv[0];
    printf("USAGE: ./%s baud_rate device_node\n", basename(path_name));
    return EXIT_FAILURE;
  }

  Arguments args;
  args.baud_rate = strtol(argv[1], nullptr, 10);

  util::FileDescriptor fd(argv[2], O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd.IsSuccess() == false) {
    return EXIT_FAILURE;
  }
  if (!isatty(fd)) {
    return EXIT_FAILURE;
  }

  // +: decay operator
  if (util::InitializeSignalAction(
#ifdef PRIVATE_DEBUG
    +[](int32_t) -> void {
      // write(): Async-Signal-Safe
      (void)write(STDOUT_FILENO, "Ignore signal\n", 14);
    },
#else
    SIG_IGN,
#endif  // PRIVATE_DEBUG
    +[](int32_t) -> void {
      // write(), kill(): Async-Signal-Safe
#ifdef PRIVATE_DEBUG
      (void)write(STDOUT_FILENO, "Disconnect\n", 11);
#endif  // PRIVATE_DEBUG
      // When signal is caught, select() is not always waiting.
      g_should_continue = 0;
    }) == status_t::kFailure) return EXIT_FAILURE;

  auto ret = mainLoop(fd, args);
  if (ret == status_t::kFailure) return EXIT_FAILURE;

  return EXIT_SUCCESS;
}

