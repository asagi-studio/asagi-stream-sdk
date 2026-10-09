# Asagi Sample — the app template (Windows x64, MinGW-w64 + Python 3)
#
#   make                       builds sample.dll and packs AsagiSample-<version>.asagiapp into $(OUT)
#   make SDK=... COMMON=...    when this folder was copied somewhere else: where asagi_app.h and
#                              Json.hpp are (the sdk/ and common/ folders of the SDK)
#
# The app folder that gets packed ($(OUT)/<id>/) holds manifest.json, the DLL and web/.
SDK     ?= ..
COMMON  ?= ../../common
CXX     := x86_64-w64-mingw32-g++-posix
BUILD   ?= build
OUT     ?= build
ID      := sample
VERSION := $(shell python3 -c "import json;print(json.load(open('manifest.json',encoding='utf-8'))['version'])")
APPDIR  := $(OUT)/$(ID)
PACKAGE := $(OUT)/AsagiSample-$(VERSION).asagiapp

CXXFLAGS := -std=gnu++17 -O2 -DNDEBUG -Wall -Wextra -Wno-unused-parameter -I$(SDK) -I$(COMMON) \
            -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -ffunction-sections -fdata-sections
# one self-contained DLL: no MinGW runtime DLLs to ship next to it
LDFLAGS  := -shared -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -s

WEBSRC := $(wildcard web/*)
WEBOUT := $(WEBSRC:web/%=$(APPDIR)/web/%)

all: $(PACKAGE)

$(PACKAGE): $(APPDIR)/$(ID).dll $(APPDIR)/manifest.json $(WEBOUT) $(SDK)/tools/pack_app.py
	python3 $(SDK)/tools/pack_app.py $(APPDIR) -o $@

$(BUILD)/app.o: app.cpp $(SDK)/asagi_app.h $(COMMON)/Json.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(APPDIR)/$(ID).dll: $(BUILD)/app.o | $(APPDIR)
	$(CXX) $< $(LDFLAGS) -o $@

$(APPDIR)/manifest.json: manifest.json | $(APPDIR)
	cp $< $@

$(APPDIR)/web/%: web/% | $(APPDIR)
	mkdir -p $(dir $@)
	cp $< $@

$(BUILD) $(APPDIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD)/app.o $(APPDIR) $(OUT)/AsagiSample-*.asagiapp

.PHONY: all clean
