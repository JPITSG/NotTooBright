# NotTooBright Makefile
# Cross-compiles the Windows executable from Linux with MinGW-w64.
#
# Build host requirements:
#   - MinGW-w64 (x86_64-w64-mingw32-gcc and -windres) on the PATH, or a
#     CROSS prefix pointing at them (see below)
#   - Node.js and npm for the configuration UI in assets/
#   - ImageMagick (optional, only for `make icon`)
#
# Machine-specific settings never go in this file. Put them in an untracked
# local.mk next to it, for example:
#   CROSS = /opt/mingw-w64/bin/x86_64-w64-mingw32-
# or pass them on the command line: make CROSS=/path/to/x86_64-w64-mingw32-

-include local.mk

CROSS ?= x86_64-w64-mingw32-
CC = $(CROSS)gcc
WINDRES = $(CROSS)windres
NPM ?= npm

RELEASE_DIR = releases
TARGET = $(RELEASE_DIR)/NotTooBright.exe
SOURCES = NotTooBright.c
RESOURCES = resource.rc
MANIFEST = NotTooBright.manifest
OBJ = main.o resource.o

FRONTEND_DIR = assets
FRONTEND_HTML = $(FRONTEND_DIR)/dist/index.html
FRONTEND_SOURCES = $(shell find $(FRONTEND_DIR)/src -type f) \
	$(FRONTEND_DIR)/index.html $(FRONTEND_DIR)/package.json \
	$(wildcard $(FRONTEND_DIR)/package-lock.json) $(FRONTEND_DIR)/vite.config.ts \
	$(FRONTEND_DIR)/tsconfig.json $(FRONTEND_DIR)/postcss.config.js \
	$(FRONTEND_DIR)/tailwind.config.ts

CFLAGS ?= -O2
CFLAGS += -mwindows -Wall -Wextra -I.
# -static keeps the result a single self-contained executable with no
# dependency on the compiler's runtime DLLs (libgcc, winpthread).
# --no-insert-timestamp makes the link reproducible: rebuilding unchanged
# sources yields a byte-identical executable, so the tracked release binary
# only changes when the program does.
LDFLAGS += -mwindows -static -Wl,--no-insert-timestamp
LIBS = -luser32 -lgdi32 -lshell32 -lshlwapi -ladvapi32 -lole32 -luuid -ldxva2

.PHONY: all frontend icon clean clean-all

all: $(TARGET)

$(TARGET): $(OBJ) | $(RELEASE_DIR)
	@echo "Linking $@..."
	$(CC) -o $@ $(OBJ) $(LDFLAGS) $(LIBS)
	@rm -f $(OBJ)
	@echo "Build complete: $@"

$(RELEASE_DIR):
	@mkdir -p $(RELEASE_DIR)

main.o: $(SOURCES) resource.h version.h
	@echo "Compiling $<..."
	$(CC) -c $< -o $@ $(CFLAGS)

resource.o: $(RESOURCES) resource.h version.h $(MANIFEST) \
		$(FRONTEND_DIR)/icon.ico $(FRONTEND_HTML) $(FRONTEND_DIR)/WebView2Loader.dll
	@echo "Compiling resources..."
	$(WINDRES) $< -o $@

# The configuration UI compiles to a single HTML file that resource.rc embeds.
$(FRONTEND_HTML): $(FRONTEND_SOURCES)
	@echo "Building configuration UI..."
	cd $(FRONTEND_DIR) && $(NPM) install && $(NPM) run build

frontend: $(FRONTEND_HTML)

# Regenerate the multi-resolution icon from assets/icon.svg (needs ImageMagick).
icon:
	convert -background none -density 384 $(FRONTEND_DIR)/icon.svg \
		-define icon:auto-resize=256,48,32,24,16 $(FRONTEND_DIR)/icon.ico

clean:
	rm -f $(OBJ)
	rm -rf $(FRONTEND_DIR)/dist

clean-all: clean
	rm -rf $(FRONTEND_DIR)/node_modules $(RELEASE_DIR)
