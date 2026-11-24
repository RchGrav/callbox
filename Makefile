CC ?= cc
CPPFLAGS ?=
EXTRA_CPPFLAGS ?=
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
STATIC_LDFLAGS ?=
else
STATIC_LDFLAGS ?= -static
endif
TEST_LDFLAGS ?=
VERSION_BASE ?= $(shell bash .github/scripts/resolve_version.sh --base 2>/dev/null || printf dev)
VERSION ?= $(shell if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then git describe --tags --exact-match --dirty 2>/dev/null || printf '%s-%s%s\n' "$(VERSION_BASE)" "$$(git rev-parse --short HEAD)" "$$(git diff --quiet 2>/dev/null || echo -dirty)"; else printf '%s\n' "$(VERSION_BASE)"; fi)
VERSION_CPPFLAG := -DHOLD_VERSION=\"$(VERSION)\"

# Every translation unit. wildcard is portable in GNU make; one glob per layer
# directory (GNU make has no portable recursive **). main.c sits directly
# under src/; the layers live in src/<layer>/.
SRCS := $(wildcard src/*.c) \
        $(wildcard src/core/*.c) \
        $(wildcard src/platform/*.c) \
        $(wildcard src/store/*.c) \
        $(wildcard src/term/*.c) \
        $(wildcard src/console/*.c) \
        $(wildcard src/access/*.c) \
        $(wildcard src/runtime/*.c) \
        $(wildcard src/screen/*.c) \
        $(wildcard src/cli/*.c)

# Vendored libvterm (third_party/libvterm, MIT): built with its own flags so the
# project's warning set (and CI's -Werror) applies to Backmux's code only, while
# optimization and sanitizer flags still carry over.
VTERM_SRCS := $(wildcard third_party/libvterm/src/*.c)
VTERM_CFLAGS := $(filter-out -Werror -Wall -Wextra -Wpedantic -std=%,$(CFLAGS)) -std=c99 -w

# Separate object trees per build "personality" so the test objects (built with
# -DHOLD_TESTING and a different HOLD_BOOT_ID_PATH) can never be linked
# into a release binary, and vice versa.
VTERM_OBJS := $(patsubst third_party/libvterm/src/%.c,obj/vterm/%.o,$(VTERM_SRCS))
# The help pages are plain text in src/cli/help (readable on GitHub); a
# generated C file compiles them in, so backmux reads no files to print help.
HELP_TXT := $(sort $(wildcard src/cli/help/*.txt src/cli/help/*/*.txt))
HELP_GEN := gen/help_text.c

OBJS      := $(patsubst src/%.c,obj/%.o,$(SRCS)) obj/gen/help_text.o $(VTERM_OBJS)
TEST_OBJS := $(patsubst src/%.c,obj-test/%.o,$(SRCS)) obj-test/gen/help_text.o $(VTERM_OBJS)

INCLUDES := -Iinclude -Ithird_party/libvterm/include
ALL_CPPFLAGS := $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(INCLUDES) $(VERSION_CPPFLAG)
TEST_CPPFLAGS := -DHOLD_TESTING -DHOLD_BOOT_ID_PATH='"/tmp/hold_test_boot_id"'
# Each object records the headers it read (obj/x.d), so editing a header
# (font_data.h, console.h, ...) rebuilds what uses it; objects also depend on
# this Makefile, so changed build rules (or objects from before .d files)
# rebuild once.
DEPFLAGS := -MMD -MP

all: backmux

obj/vterm/%.o: third_party/libvterm/src/%.c Makefile
	@mkdir -p $(dir $@)
	$(CC) -Ithird_party/libvterm/include $(VTERM_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(HELP_GEN): $(HELP_TXT) scripts/gen_help.sh
	@mkdir -p $(dir $@)
	sh scripts/gen_help.sh src/cli/help $(HELP_TXT) > $@.tmp && mv $@.tmp $@

obj/gen/help_text.o: $(HELP_GEN) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

obj-test/gen/help_text.o: $(HELP_GEN) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CPPFLAGS) $(TEST_CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

obj/%.o: src/%.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

# A static link uses its own build of paths.c that never references getpwuid
# (glibc cannot load NSS modules into a static binary, and warns).
STATIC_OBJS := $(if $(strip $(STATIC_LDFLAGS)),$(filter-out obj/platform/paths.o,$(OBJS)) obj/platform/paths-static.o,$(OBJS))

obj/platform/paths-static.o: src/platform/paths.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CPPFLAGS) -DHOLD_STATIC_LINK $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

backmux: $(STATIC_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(STATIC_LDFLAGS) -o $@ $(STATIC_OBJS)

backmux-dynamic: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o backmux-dynamic $(OBJS)

obj-test/%.o: src/%.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CPPFLAGS) $(TEST_CPPFLAGS) $(DEPFLAGS) $(CFLAGS) -c -o $@ $<

# The suite's binary is backmux-test, never ./backmux: a -DHOLD_TESTING build
# honors test-only environment (HOLD_TEST_SYSTEM_STATE_DIR, failure
# injection) and is linked dynamically, so it must not be left where the
# release binary goes - nor be taken for it by a later `make`, which would
# call the test build up to date.
backmux-test: $(TEST_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(TEST_LDFLAGS) -o backmux-test $(TEST_OBJS)

test: backmux-test
	@HOLD_BIN=./backmux-test bash tests/test_backmux.sh
	@$(MAKE) screen-test
	@bash tests/test_version_makefile.sh
	@bash tests/test_release_installer.sh
	@$(MAKE) test-040

screen-test: tests/screen_test.c src/screen/screen.c src/console/compose.c src/console/overlay.h src/term/line.c $(wildcard include/backmux/*.h) $(VTERM_OBJS)
	$(CC) $(ALL_CPPFLAGS) $(TEST_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o screen-test tests/screen_test.c src/screen/screen.c src/console/compose.c src/term/line.c $(VTERM_OBJS)
	@./screen-test

# The test build: only it honors HOLD_TEST_SYSTEM_STATE_DIR, so run as root
# (the root CI lane) a release build wrote into the machine's real system
# store. It used to pass by reusing a test-flag backmux-dynamic the suite's
# build test left in the tree; that test now builds in a copy.
test-040: backmux-test
	@HOLD_BIN=./backmux-test bash tests/test_040_dockerish.sh

print-version:
	@printf '%s\n' '$(VERSION)'

check: test

# Full local CI mirror (one command, same rigor as GitHub CI): static + dynamic
# -Werror builds, the regression suite, ASan/UBSan, cppcheck, and
# the layer-dependency lint. Skips (visibly) only what the local toolchain lacks.
ci:
	@bash scripts/ci.sh

# Layer dependency-direction lint, shared verbatim with the GitHub CI job.
lint:
	@bash scripts/lint_layers.sh

-include $(wildcard obj/*.d obj/*/*.d obj-test/*.d obj-test/*/*.d)

clean:
	rm -f backmux backmux-dynamic backmux-test screen-test
	rm -rf obj obj-test gen

.PHONY: all clean test test-040 check ci lint screen-test print-version
