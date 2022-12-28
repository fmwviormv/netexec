.POSIX:

all: netexec

clean:
	rm -f netexec netexec.o netexec.d

CFLAGS+= -std=c99 -pedantic -Wall -Wextra -Werror

.PHONY: all clean
