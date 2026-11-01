PLUGIN   := noshare-cover
prefix   ?= /usr/local
CARGO    ?= cargo

# Cargo's target directory: `target` here, or $(CARGO_TARGET_DIR) if set. hyprpm sets it
# outside its throwaway clone (hyprpm.toml): the Rust core doesn't use Hyprland's headers,
# so a rebuild after a Hyprland update reuses every compiled dependency.
TARGET   := $(or $(CARGO_TARGET_DIR),target)
RUST_LIB := $(TARGET)/release/libnoshare_cover.a
RUST_SRC := Cargo.toml Cargo.lock build.rs $(shell find src -name '*.rs')
# VA-API helper: a separate .so with libva/libgbm, embedded into the plugin (see
# src/media/video/decode/vaapi.rs). NSC_VAAPI=0 builds without VA-API (then libva
# isn't needed).
# All build requirements are checked up front and missing ones are listed,
# instead of a silent "failed to build" in hyprpm.
NSC_VAAPI ?= 1
have = $(shell command -v $(1) >/dev/null 2>&1 && echo y)
havepc = $(shell pkg-config --exists $(1) 2>/dev/null && echo y)
MISSING :=
ifneq ($(shell $(CARGO) --version >/dev/null 2>&1 && echo y),y)
MISSING += cargo(pacman:rust)
endif
ifneq ($(call have,pkg-config),y)
MISSING += pkg-config(pacman:pkgconf)
endif
ifneq ($(call have,nasm),y)
MISSING += nasm(pacman:nasm)
endif
ifneq ($(call havepc,hyprland),y)
MISSING += hyprland-headers(hyprpm update / pacman:hyprland)
endif
ifeq ($(NSC_VAAPI),1)
ifneq ($(call havepc,libva libva-drm),y)
MISSING += libva(pacman:libva)
endif
ifneq ($(call havepc,gbm),y)
MISSING += gbm(pacman:mesa)
endif
endif
ifneq ($(strip $(MISSING)),)
$(error noshare-cover: missing build dependencies: $(strip $(MISSING)). Arch: sudo pacman -S --needed rust pkgconf nasm libva mesa. Without VA-API: make NSC_VAAPI=0)
endif
HELPER   := $(TARGET)/release/libnoshare_cover_vaapi.so
HELPER_SRC := vaapi-helper/Cargo.toml $(shell find vaapi-helper/src vendor -name '*.rs')

# No ffmpeg/cairo/libjpeg/giflib: all media handling lives in the Rust core.
PKGS     := hyprland pixman-1 libdrm wayland-server egl hyprutils hyprgraphics aquamarine hyprlang
CXXFLAGS := -std=c++26 -shared -fPIC -fno-gnu-unique -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
# the version Hyprland shows in `hyprctl plugin list` comes from Cargo.toml, so it can't drift
VERSION  := $(shell sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)
CXXFLAGS += -DNSC_VERSION='"$(VERSION)"'
INCLUDES := $(shell pkg-config --cflags $(PKGS)) -Iinclude
# Rust std on Linux: pthread, dl, m. VA-API/dav1d/openh264 come with the decoders.
LIBS     := -lpthread -ldl -lm
# The whole Rust archive is hidden (--exclude-libs): its symbols aren't exported
# and don't clash with other plugins. The plugin itself links like any C++ plugin:
# Hyprland globals (inline variables like g_pHyprRenderer) must bind to the copy
# in the Hyprland binary, so a version script with `local: *` is not an option.
LDFLAGS  := -Wl,--exclude-libs,ALL -Wl,--gc-sections

all: lib$(PLUGIN).so

ifeq ($(NSC_VAAPI),1)
$(HELPER): $(HELPER_SRC) Cargo.lock
	$(CARGO) build --release --locked -p noshare-cover-vaapi

$(RUST_LIB): $(RUST_SRC) $(HELPER)
	NSC_VAAPI_HELPER=$(abspath $(HELPER)) $(CARGO) build --release --locked -p noshare-cover
else
$(RUST_LIB): $(RUST_SRC)
	$(CARGO) build --release --locked -p noshare-cover --no-default-features --features nvdec,cpu-av1,cpu-h264,cpu-vpx
endif

lib$(PLUGIN).so: shim/plugin.cpp include/noshare_cover.h include/noshare_cover_api.h Cargo.toml $(RUST_LIB)
	$(CXX) $(CXXFLAGS) $(INCLUDES) shim/plugin.cpp $(RUST_LIB) -o $@ $(LDFLAGS) $(LIBS)

test:
	$(CARGO) test --locked

install: all
	install -Dm755 lib$(PLUGIN).so $(prefix)/lib/lib$(PLUGIN).so
	ln -sfn lib$(PLUGIN).so $(prefix)/lib/$(PLUGIN).so

# local copy; Nix never calls this target
local: all
	install -Dm755 lib$(PLUGIN).so $(HOME)/.config/hypr/plugins/$(PLUGIN).so

clean:
	rm -f lib$(PLUGIN).so
	$(CARGO) clean

.PHONY: all test install local clean
