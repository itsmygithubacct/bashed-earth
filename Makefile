CC      ?= cc
KITTY_FRAMEBUFFER_DIR ?= third_party/kitty-framebuffer
SOFT_RASTER_DIR ?= third_party/soft-raster
PCM_MIXER_DIR ?= third_party/pcm-mixer
override CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	-I$(KITTY_FRAMEBUFFER_DIR)/include \
	-I$(SOFT_RASTER_DIR)/include \
	-I$(PCM_MIXER_DIR)/include
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS   = -lz -lm -lpthread

SRC = src/main.c src/game.c src/terrain.c src/render.c src/term.c src/config.c src/sound.c
VENDOR_OBJ = src/vendor_kitty_framebuffer.o src/vendor_pcm_mixer.o \
	src/vendor_pcm_wav.o src/vendor_soft_raster.o
OBJ = $(SRC:.c=.o) $(VENDOR_OBJ)
BIN = bashed-earth

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/bashed_earth.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/render.o: src/font8x16.h $(SOFT_RASTER_DIR)/include/soft_raster.h
src/sound.o: $(PCM_MIXER_DIR)/include/pcm_mixer.h
src/term.o: $(KITTY_FRAMEBUFFER_DIR)/include/kitty_framebuffer.h

src/vendor_kitty_framebuffer.o: $(KITTY_FRAMEBUFFER_DIR)/src/kitty_framebuffer.c \
	$(KITTY_FRAMEBUFFER_DIR)/src/kitty_framebuffer_internal.h \
	$(KITTY_FRAMEBUFFER_DIR)/include/kitty_framebuffer.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/vendor_pcm_mixer.o: $(PCM_MIXER_DIR)/src/pcm_mixer.c \
	$(PCM_MIXER_DIR)/include/pcm_mixer.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/vendor_pcm_wav.o: $(PCM_MIXER_DIR)/src/pcm_wav.c \
	$(PCM_MIXER_DIR)/include/pcm_mixer.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

src/vendor_soft_raster.o: $(SOFT_RASTER_DIR)/src/soft_raster.c \
	$(SOFT_RASTER_DIR)/include/soft_raster.h \
	$(SOFT_RASTER_DIR)/src/font8x16.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: $(BIN)
	./$(BIN) --selftest 1337 3
	./$(BIN) --selftest 42 2
	./$(BIN) --selftest 7 2

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all test clean
