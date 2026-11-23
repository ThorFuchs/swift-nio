// macOS kqueue early-EOF diagnostic. NOT part of SwiftNIO or a shipping patch.
// Public interfaces only: socket, kqueue, EVFILT_EXCEPT, NOTE_LOWAT, EV_CLEAR.
// This program records observations; it does not reinterpret spurious events as passes.
#define _DARWIN_C_SOURCE 1
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { POLL_EVENT_CAP = 4096, FILL_BYTE_CAP = 8 * 1024 * 1024 };
typedef struct {
    int sender;
    int receiver;
} SocketPair;

typedef struct {
    int total;
    int non_eof;
    int eof;
    int error;
    int capped;
    intptr_t first_non_eof_data;
    unsigned short first_non_eof_flags;
} Events;

static void fail(const char *what) {
    fprintf(stderr, "FATAL %s: %s (errno %d)\n", what, strerror(errno), errno);
    exit(2);
}

static int64_t millis(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) fail("clock_gettime");
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(int n) {
    struct timespec t = {.tv_sec = n / 1000, .tv_nsec = (n % 1000) * 1000000L};
    (void)nanosleep(&t, NULL);
}

static void nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) fail("fcntl O_NONBLOCK");
}

static void tune(int receiver, int sender) {
    int rcv = 8192;
    int snd = 16384;
    if (setsockopt(receiver, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv)) < 0) fail("SO_RCVBUF");
    if (setsockopt(sender, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd)) < 0) fail("SO_SNDBUF");
    nonblocking(receiver);
    nonblocking(sender);
}

static int receive_capacity(int receiver) {
    int rcv = 0;
    socklen_t length = sizeof(rcv);
    if (getsockopt(receiver, SOL_SOCKET, SO_RCVBUF, &rcv, &length) < 0) fail("get SO_RCVBUF");
    return rcv;
}

static int queued_bytes(int receiver) {
    int queued = 0;
    if (ioctl(receiver, FIONREAD, &queued) < 0) fail("FIONREAD");
    return queued;
}

static SocketPair connect_tcp(int family) {
    SocketPair p = {.sender = -1, .receiver = -1};
    int listener = socket(family, SOCK_STREAM, 0);
    if (listener < 0) fail("socket listener");

    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    socklen_t len;
    if (family == AF_INET) {
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = 0};
        a.sin_len = (uint8_t)sizeof(a);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        memcpy(&addr, &a, sizeof(a));
        len = sizeof(a);
    } else {
        struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = 0};
        a.sin6_len = (uint8_t)sizeof(a);
        a.sin6_addr = in6addr_loopback;
        memcpy(&addr, &a, sizeof(a));
        len = sizeof(a);
        int only_v6 = 1;
        if (setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, &only_v6, sizeof(only_v6)) < 0)
            fail("IPV6_V6ONLY");
    }
    if (bind(listener, (struct sockaddr *)&addr, len) < 0) fail("bind loopback");
    if (listen(listener, 1) < 0) fail("listen");
    if (getsockname(listener, (struct sockaddr *)&addr, &len) < 0) fail("getsockname");

    p.sender = socket(family, SOCK_STREAM, 0);
    if (p.sender < 0) fail("socket client");
    // Connect in blocking mode before enabling nonblocking I/O.
    if (connect(p.sender, (struct sockaddr *)&addr, len) < 0) fail("connect loopback");
    p.receiver = accept(listener, NULL, NULL);
    if (p.receiver < 0) fail("accept");
    if (close(listener) < 0) fail("close listener");
    tune(p.receiver, p.sender);
    return p;
}

static SocketPair connect_uds(void) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) fail("socketpair AF_UNIX");
    SocketPair p = {.sender = fds[0], .receiver = fds[1]};
    tune(p.receiver, p.sender);
    return p;
}

static int register_except(int fd, bool edge) {
    int kq = kqueue();
    if (kq < 0) fail("kqueue");
    struct kevent change;
    unsigned short flags = (unsigned short)(EV_ADD | EV_ENABLE | (edge ? EV_CLEAR : 0));
    EV_SET(&change, (uintptr_t)fd, EVFILT_EXCEPT, flags, NOTE_LOWAT, INTPTR_MAX, NULL);
    if (kevent(kq, &change, 1, NULL, 0, NULL) < 0) fail("kevent EVFILT_EXCEPT register");
    return kq;
}

static void poll_events(int kq, int window_ms, Events *events) {
    int64_t end = millis() + window_ms;
    while (millis() < end && events->total < POLL_EVENT_CAP) {
        struct kevent results[16];
        struct timespec wait = {.tv_sec = 0, .tv_nsec = 2000000L};
        int got = kevent(kq, NULL, 0, results, 16, &wait);
        if (got < 0) {
            if (errno == EINTR) continue;
            fail("kevent poll");
        }
        for (int i = 0; i < got; i++) {
            struct kevent *e = &results[i];
            if (e->flags & EV_ERROR) events->error++;
            if (e->filter != EVFILT_EXCEPT) continue;
            events->total++;
            if (e->flags & EV_EOF) {
                events->eof++;
            } else {
                if (events->non_eof == 0) {
                    events->first_non_eof_data = e->data;
                    events->first_non_eof_flags = e->flags;
                }
                events->non_eof++;
            }
        }
    }
    if (events->total >= POLL_EVENT_CAP) events->capped = 1;
}

static size_t fill_receiver(SocketPair p, int capacity) {
    unsigned char bytes[8192];
    memset(bytes, 'X', sizeof(bytes));
    size_t sent = 0;
    int64_t deadline = millis() + 1200;
    // Report BOTH actual FIONREAD and socket capacity: merely sending enough
    // bytes does not prove the remote receive buffer is full.
    while (millis() < deadline && sent < FILL_BYTE_CAP) {
        size_t request = sent == 0 ? sizeof(bytes) : 512;
        ssize_t n = send(p.sender, bytes, request, 0);
        if (n > 0) {
            sent += (size_t)n;
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            sleep_ms(2);
        } else if (n == -1 && errno == EINTR) {
            continue;
        } else {
            fail("nonblocking fill send");
        }
        if (queued_bytes(p.receiver) >= capacity) {
            sleep_ms(10);
            break;
        }
    }
    return sent;
}

static void send_small(SocketPair p) {
    char bytes[64];
    memset(bytes, 'S', sizeof(bytes));
    size_t done = 0;
    while (done < sizeof(bytes)) {
        ssize_t n = send(p.sender, bytes + done, sizeof(bytes) - done, 0);
        if (n > 0) done += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else fail("send 64 bytes");
    }
}

static void drain_after_close(int receiver, int kq, Events *events,
                              size_t *drained, bool *saw_recv_eof, bool *saw_reset) {
    int64_t deadline = millis() + 1800;
    char bytes[16384];
    while (millis() < deadline && !*saw_recv_eof && !*saw_reset) {
        for (;;) {
            ssize_t n = recv(receiver, bytes, sizeof(bytes), 0);
            if (n > 0) {
                *drained += (size_t)n;
                continue;
            }
            if (n == 0) *saw_recv_eof = true;
            else if (errno == ECONNRESET) *saw_reset = true;
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                fail("drain recv");
            break;
        }
        poll_events(kq, 6, events);
        if (!*saw_recv_eof && !*saw_reset) sleep_ms(2);
    }
    poll_events(kq, 60, events);
}

static void one_case(const char *family, const char *scenario, bool clear, int iteration) {
    SocketPair p;
    if (strcmp(family, "ipv4") == 0) p = connect_tcp(AF_INET);
    else if (strcmp(family, "ipv6") == 0) p = connect_tcp(AF_INET6);
    else p = connect_uds();

    int capacity = receive_capacity(p.receiver);
    int kq = register_except(p.receiver, clear);
    size_t sent = 0;
    if (strcmp(scenario, "small") == 0) {
        send_small(p);
        sent = 64;
    } else if (strcmp(scenario, "full") == 0) {
        sent = fill_receiver(p, capacity);
    }
    int queued = queued_bytes(p.receiver);

    Events before = {0}, after = {0}, after_drain = {0};
    poll_events(kq, 180, &before);

    if (strcmp(scenario, "rst") == 0) {
        struct linger abortive = {.l_onoff = 1, .l_linger = 0};
        if (setsockopt(p.sender, SOL_SOCKET, SO_LINGER, &abortive, sizeof(abortive)) < 0)
            fail("SO_LINGER");
        if (close(p.sender) < 0) fail("close sender RST");
        p.sender = -1;
    } else {
        if (shutdown(p.sender, SHUT_WR) < 0) fail("shutdown sender FIN");
    }

    // This phase deliberately performs NO recv(), even when the queue is full.
    poll_events(kq, 350, &after);

    size_t drained = 0;
    bool recv_eof = false, recv_reset = false;
    drain_after_close(p.receiver, kq, &after_drain, &drained, &recv_eof, &recv_reset);
    const char *status = "OBSERVED";
    if (strcmp(scenario, "full") == 0 && queued < capacity) status = "FULL_NOT_CONFIRMED";
    else if (strcmp(scenario, "rst") != 0 &&
             after.eof == 0 && after_drain.eof == 0) status = "EXCEPT_EOF_NOT_SEEN";
    else if (before.non_eof > 0) status = "PRE_CLOSE_SPURIOUS";

    printf("%s\t%s\t%s\t%d\t%d\t%d\t%zu\t%d\t%d\t%d\t%d\t%u\t%ld\t%d\t%d\t%d\t%d\t%zu\t%d\t%d\t%s\n",
           clear ? "EV_CLEAR" : "LEVEL", family, scenario, iteration, capacity, queued, sent,
           before.non_eof, before.eof, before.capped, after.non_eof,
           (unsigned)before.first_non_eof_flags, (long)before.first_non_eof_data,
           after.eof, after_drain.non_eof, after_drain.eof, after_drain.capped, drained,
           recv_eof ? 1 : 0, recv_reset ? 1 : 0, status);
    fflush(stdout);

    if (close(kq) < 0) fail("close kqueue");
    if (close(p.receiver) < 0) fail("close receiver");
    if (p.sender >= 0 && close(p.sender) < 0) fail("close sender");
}

int main(int argc, char **argv) {
    if (argc > 2) {
        fprintf(stderr, "usage: %s [repetitions:1..12]\n", argv[0]);
        return 2;
    }
    int repetitions = 3;
    if (argc == 2) {
        char *end = NULL;
        long n = strtol(argv[1], &end, 10);
        if (*argv[1] == 0 || *end != 0 || n < 1 || n > 12) {
            fprintf(stderr, "repetitions must be an integer from 1 through 12\n");
            return 2;
        }
        repetitions = (int)n;
    }
    signal(SIGPIPE, SIG_IGN);
    puts("mode\tfamily\tscenario\titeration\trecv_capacity\tqueued_preclose\tbytes_sent\tpre_non_eof\tpre_eof\tpre_capped\tpost_non_eof\tfirst_spurious_flags\tfirst_spurious_data\tpost_eof\tdrain_non_eof\tdrain_eof\tdrain_capped\tdrained_bytes\trecv_eof\trecv_reset\tstatus");
    for (int rep = 1; rep <= repetitions; rep++) {
        for (int c = 0; c < 2; c++) {
            for (int f = 0; f < 3; f++) {
                const char *family = f == 0 ? "ipv4" : f == 1 ? "ipv6" : "uds";
                for (int s = 0; s < 4; s++) {
                    const char *scenario = s == 0 ? "empty" : s == 1 ? "small" : s == 2 ? "full" : "rst";
                    one_case(family, scenario, c != 0, rep);
                }
            }
        }
    }
    return 0;
}
