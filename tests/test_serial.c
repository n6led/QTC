#include "test.h"
#include "qtc/serial.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int64_t millis(void) {
    struct timespec t;
    ASSERT_EQ_INT(clock_gettime(CLOCK_MONOTONIC, &t), 0);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(void) {
    int fd[2];
    ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, fd), 0);
    ASSERT_EQ_INT(fcntl(fd[0], F_SETFL, O_NONBLOCK), 0);
    qtc_serial s = {.fd = fd[0]};
    uint8_t buf[4096] = {0}, command[] = {1};
    ASSERT_EQ_INT(qtc_serial_read(&s, buf, sizeof(buf)), -1);
    ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    ASSERT_EQ_INT(qtc_serial_send(&s, command, sizeof(command)), 0);
    ASSERT_EQ_INT(read(fd[1], buf, sizeof(buf)), 4);
    ASSERT_TRUE(memcmp(buf, "<\x01\x00\x01", 4) == 0);

    /* A stalled device must not trap the core in an unbounded write loop. */
    while (write(fd[0], buf, sizeof(buf)) > 0) {}
    ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    int64_t start = millis();
    ASSERT_EQ_INT(qtc_serial_send(&s, command, sizeof(command)), -1);
    ASSERT_EQ_INT(errno, ETIMEDOUT);
    ASSERT_TRUE(millis() - start < 2000);
    while (recv(fd[1], buf, sizeof(buf), MSG_DONTWAIT) > 0) {}
    ASSERT_EQ_INT(qtc_serial_send(&s, command, sizeof(command)), 0);
    ASSERT_EQ_INT(read(fd[1], buf, sizeof(buf)), 4);
    close(fd[1]);
    ASSERT_EQ_INT(qtc_serial_read(&s, buf, sizeof(buf)), 0);
    close(fd[0]);
    ASSERT_EQ_INT(qtc_serial_send(&s, command, sizeof(command)), -1);
    ASSERT_EQ_INT(errno, EBADF);
    s.fd = -1;
    qtc_serial_close(&s);
    puts("serial I/O tests passed");
    return 0;
}
