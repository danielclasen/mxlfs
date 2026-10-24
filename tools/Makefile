CFLAGS ?= -O2 -g -Wall -Wextra -Wno-format-truncation
LIBMXL ?= /opt/libmxl

harness: harness.c ../kernel/mxlfs_uapi.h
	$(CC) $(CFLAGS) -I$(LIBMXL)/include -o $@ $< -L$(LIBMXL)/lib -Wl,-rpath,$(LIBMXL)/lib -lmxl

clean:
	rm -f harness
