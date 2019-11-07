-include config.mk

bin = lsc
src = filevercmp.c grid.c id.c ls_colors.c lsc.c

CFLAGS ?= -O2 -pipe -Wall -Wextra -pedantic -g

CPPFLAGS += -MMD -MP -D_XOPEN_SOURCE=700
CFLAGS += -std=c99

$(bin): $(src:.c=.o)

clean:
	rm -f $(bin) *.o *.d

-include $(src:.c=.d)

.PHONY: clean
