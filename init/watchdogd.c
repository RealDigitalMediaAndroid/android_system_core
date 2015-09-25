#include <stdio.h>  // printf
#include <stdlib.h> // getenv
#include <unistd.h> // sleep
#include <signal.h> // sigaction
#include <string.h> // strrchr, strerror
#include <sys/types.h> // mkfifo, open
#include <sys/stat.h> // mkfifo, open, fchmod
#include <errno.h> // errno
#include <fcntl.h> // O_RDONLY
#include <sys/select.h> // select
#include <sys/time.h> // gettimeofday
#include <linux/watchdog.h>
#include <stdarg.h> // vprintf

#ifdef ANDROID
#    include "log.h"
#    include "util.h"
#endif

/****************************
 * common
 ****************************/

#ifndef ANDROID
int INFO(const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  int ret = vprintf(format, ap);
  va_end(ap);
  return ret;
}

int ERROR(const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  int ret = vfprintf(stderr, format, ap);
  va_end(ap);
  return ret;
}
#endif

struct timeval *currentTime() {
  static struct timeval tv;
  if (gettimeofday(&tv, NULL))
    ERROR("Failed to get current time!\n");
  return &tv;
}

struct timeval timeInNSeconds(int n) {
  struct timeval tv;
  struct timeval res;
  timerclear(&tv);
  tv.tv_sec = n;
  struct timeval *ct = currentTime();
  timeradd(ct, &tv, &res);
  return res;
}

struct timeval timeLeft(struct timeval *tv) {
  struct timeval timeLeft;
  struct timeval *ct = currentTime();
  timersub(tv, ct, &timeLeft);
  return timeLeft;
}

int isExpired(struct timeval *expiration) {
  struct timeval *ct = currentTime();
  return timercmp(expiration, ct, <);
}

// Get lesser of two timeouts in time remaining. Only return a positive value.
struct timeval getLesserTimeout(long timeout, struct timeval expiration) {
  struct timeval tv;
  timerclear(&tv);
  tv.tv_sec = timeout;
  if (isExpired(&expiration))
    return tv;
  struct timeval timeTillExpiration = timeLeft(&expiration);
  return timercmp(&tv, &timeTillExpiration, <) ? tv : timeTillExpiration;
}



/****************************
 * control fifo
 ****************************/

#ifdef ANDROID
#    define FIFO_NAME "/dev/pet"
int chown_main(int argc, char **argv);
#else
#    define FIFO_NAME "pet"
#endif

int createAndOpenPetForReading() {
  int fd;

  unlink(FIFO_NAME);
  if (mkfifo(FIFO_NAME, 0620)) {
    ERROR("Failed to create named pipe, %s: %s\n", FIFO_NAME, strerror(errno));
    return -1;
  }

  fd = open(FIFO_NAME, O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    ERROR("Failed to open named pipe, %s, for reading: %s\n", FIFO_NAME,
          strerror(errno));
    return fd;
  }

  if (fchmod(fd, 0220))
    ERROR("Failed to make named pipe, %s,  write-only: %s\n", FIFO_NAME,
          strerror(errno));

#ifdef ANDROID
  char *owner = "root.system";
  char *argv[] = { "watchdogd", owner, FIFO_NAME };
  int argc = 3;
  if (chown_main(argc, argv))
    ERROR("Failed to set owner of named pipe, %s, to %s: %s\n", FIFO_NAME,
          owner, strerror(errno));
#endif

  return fd;
}

long getLastNumberInBuffer(char *buf, size_t len) {
  long num = -1;
  char *end = buf + len;
  buf[len] = '\0';

  while (buf < end) {
    char *p = strchr(buf, '\n');
    if (p == NULL)
      p = buf + len - 1;
    *p = '\0';
    char *endptr;
    long temp = strtol(buf, &endptr, 10);
    if (endptr == p)
      num = temp;
    buf = p + 1;
  }

  if (num != -1)
    INFO("getLastNumberInBuffer = %ld\n", num);
  return num;
}

int isFifoReadyToRead(int fd, struct timeval timeout) {
  int nfds = fd + 1;
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(fd, &readfds);
  // TODO: Can we catch errors with fd here?
  nfds = select(nfds, &readfds, NULL, NULL, &timeout);
  if (nfds > 0)
    return 1;

  if (nfds < 0)
    ERROR("select(pet fifo): %s\n", strerror(errno));

  return 0;
}

struct timeval getNewPetExpiration(int *fd, int interval, int timeout,
                                   struct timeval expiration) {
  if (!isFifoReadyToRead(*fd, getLesserTimeout(interval, expiration)))
    return expiration;

  char buf[1024];
  int len = read(*fd, &buf, sizeof(buf));
  if (len <= 0) {
    if (len < 0)
      ERROR("Error reading named fifo, %s: %s\n", FIFO_NAME, strerror(errno));
    close(*fd);
    *fd = createAndOpenPetForReading();
    return expiration;
  }

  long newPetTimeout = getLastNumberInBuffer(buf, len);
  if (newPetTimeout >= 0)
    return timeInNSeconds(newPetTimeout);

  // if we read anything at all, treat it like a proxy pet
  return timeInNSeconds(timeout);
}

/****************************
 * watchdog
 ****************************/

#define DEV_NAME "/dev/watchdog"

int openWatchdogForWriting() {
  int fd = open(DEV_NAME, O_RDWR);
  if (fd < 0) {
    ERROR("watchdogd: Failed to open %s: %s\n", DEV_NAME, strerror(errno));
  }
  return fd;
}

int setWatchdogTimeout(int fd, int interval, int margin) {
  int timeout = interval + margin;
  int ret = ioctl(fd, WDIOC_SETTIMEOUT, &timeout);
  if (ret) {
    ERROR("watchdogd: Failed to set timeout to %d: %s\n", timeout,
          strerror(errno));
    ret = ioctl(fd, WDIOC_GETTIMEOUT, &timeout);
    if (ret) {
      ERROR("watchdogd: Failed to get timeout: %s\n", strerror(errno));
    } else {
      if (timeout > margin)
        interval = timeout - margin;
      else
        interval = 1;
      ERROR("watchdogd: Adjusted interval to timeout returned by driver: timeout %d, interval %d, margin %d\n",
            timeout, interval, margin);
    }
  }
  return interval;
}

void pet(int fd) {
  if (fd >= 0)
    write(fd, "", 1);
}

/****************************
 * arguments, setup, & main
 ****************************/

static int continueRunning = 1;

void handleArguments(int argc, char *argv[],
                     int *interval, int *margin,
                     struct timeval *petExpiration) {
  if (argc >= 2)
    *interval = atoi(argv[1]);

  if (argc >= 3)
    *margin = atoi(argv[2]);

  if (argc >= 4)
    *petExpiration = timeInNSeconds(atoi(argv[3]));
}

void sigIntHandler(int signum) {
  continueRunning = 0;
}

void installSignalHandlers() {
  struct sigaction act;
  act.sa_handler = sigIntHandler;
  sigaction(SIGINT, &act, NULL);
}

const char *programName(const char *argv0) {
  char *name = strrchr(argv0, '/');
  if (name)
    return name + 1;

  return argv0;
}

int testMode() {
  return NULL != getenv("WATCHDOGD_TEST_MODE");
}

#ifndef ANDROID
int main(int argc, char *argv[]) {
#else
  int watchdogd_main(int argc, char **argv) {
    open_devnull_stdio();
    //klog_set_level(7);
    klog_init();
#endif

    int isTestMode = testMode();
    int timeout;
    int interval = 10;
    int margin = 20;
    int petFd = -1;
    int watchdogFd = -1;
    struct timeval petExpiration = timeInNSeconds(30);

    handleArguments(argc, argv, &interval, &margin, &petExpiration);
    timeout = interval + margin;
    INFO("Starting %s with timeout: %d seconds\n", programName(argv[0]),
         timeout);

    installSignalHandlers();
    petFd = createAndOpenPetForReading();
    if (!isTestMode) {
      watchdogFd = openWatchdogForWriting();
      if (watchdogFd < 0)
        return 1;
      interval = setWatchdogTimeout(watchdogFd, interval, margin);
    }

    while (continueRunning) {
      if (!isExpired(&petExpiration)) {
        struct timeval left = timeLeft(&petExpiration);
        INFO("pet: %ld.%03ld seconds remaining\n", left.tv_sec,
             left.tv_usec / 1000);
        pet(watchdogFd);
      } else if (isTestMode) {
        break;
      }
      petExpiration = getNewPetExpiration(&petFd, interval, timeout,
                                          petExpiration);
    }

    close(watchdogFd);
    close(petFd);
    unlink(FIFO_NAME);
    INFO("watchdogd shutting down\n");

    return 0;
  }
