CFLAGS ?= -O2 -g -Wall -Wextra -Wno-format-truncation
LIBMXL ?= /opt/libmxl
SRCS = main.c util.c flow.c redundancy.c audio.c wait.c views.c lazy.c folios.c numa.c move.c

harness: $(SRCS) harness.h ../kernel/mxlfs_uapi.h
	$(CC) $(CFLAGS) -pthread -I$(LIBMXL)/include -o $@ $(SRCS) -L$(LIBMXL)/lib -Wl,-rpath,$(LIBMXL)/lib -lmxl

clean:
	rm -f harness
