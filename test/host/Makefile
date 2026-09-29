CC     ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O2 -g
MAIN   := ../../main
SRCS   := test_motion.c $(MAIN)/motion.c $(MAIN)/overlay.c
DEPS   := $(SRCS) $(MAIN)/motion.h $(MAIN)/overlay.h

.PHONY: all test asan clean

all: test

build/test_motion: $(DEPS)
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(MAIN) -o $@ $(SRCS)

build/test_motion_asan: $(DEPS)
	@mkdir -p build
	$(CC) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
		-fno-omit-frame-pointer -I$(MAIN) -o $@ $(SRCS)

test: build/test_motion
	./build/test_motion

asan: build/test_motion_asan
	./build/test_motion_asan

clean:
	rm -rf build
