TARGET := libnorss.so

PKG_CONFIG ?= pkg-config

CFLAGS := -g -Werror -Wall -Wextra -O3

ifneq ($(MAKECMDGOALS),clean)
CFLAGS += $(shell $(PKG_CONFIG) --cflags libdpdk)
endif

$(TARGET): ixgbevf_disable_rss.c
	$(CC) $(CFLAGS) -fPIC -shared -Wl,--no-undefined -o $@ $< -ldl

clean:
	rm -f $(TARGET)
