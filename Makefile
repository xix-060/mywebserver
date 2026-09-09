# Makefile — myweb v2 (epoll Reactor + 线程池)
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread -std=gnu11
TARGET  = myweb
SRCS    = src/webserver.c src/threadpool.c src/http_parser.c src/timer_wheel.c src/log.c

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: clean
