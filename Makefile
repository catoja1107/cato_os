# Cato OS build. Run inside WSL Ubuntu (see README.MD).

CC    := clang
LD    := lld-link
BUILD := build
DISK  := $(BUILD)/cato.img
EFI   := $(BUILD)/boot/BOOTX64.EFI

BOOT_SRCS := $(wildcard boot/*.c)
BOOT_OBJS := $(BOOT_SRCS:%.c=$(BUILD)/%.o)
GUI_SRCS  := $(wildcard gui/*.c)
GUI_OBJS  := $(GUI_SRCS:%.c=$(BUILD)/%.o)
FS_SRCS   := $(wildcard fs/*.c)
FS_OBJS   := $(FS_SRCS:%.c=$(BUILD)/%.o)
NET_SRCS  := $(wildcard net/*.c)
NET_OBJS  := $(NET_SRCS:%.c=$(BUILD)/%.o)
WEB_SRCS  := $(wildcard web/*.c)
WEB_OBJS  := $(WEB_SRCS:%.c=$(BUILD)/%.o)
IMAGE_SRCS := web/image.c web/inflate.c web/png.c web/jpeg.c web/gif.c

# BearSSL 0.6 (third_party/bearssl/README.cato.md). It is built only for the EFI
# image. Host unit tests link tests/unit/tls_stub.c instead.
BEARSSL      := third_party/bearssl
BEARSSL_SRCS := $(sort $(shell find $(BEARSSL)/src -name '*.c'))
BEARSSL_OBJS := $(BEARSSL_SRCS:%.c=$(BUILD)/%.o)
NET_TEST_SRCS := $(filter-out net/tls.c net/tls_anchors.c,$(NET_SRCS)) tests/unit/tls_stub.c

INCLUDES  := -Iboot -Igui -Ifs -Inet -Iweb -isystem $(BEARSSL)/inc
TEST_CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
	-fno-sanitize-recover=undefined $(INCLUDES)

SPLASH_PNG := assets/splash.png
SPLASH_C   := $(BUILD)/generated/splash_data.c
SPLASH_PGM := $(BUILD)/generated/splash.pgm
SPLASH_OBJ := $(BUILD)/generated/splash_data.o

# UEFI x86_64 uses the Microsoft x64 calling convention and PE32+ images.
# -mno-red-zone:       firmware interrupts may run on our stack.
# -mgeneral-regs-only: the loader must not depend on SSE state.
# -mno-stack-arg-probe: there is no __chkstk runtime in UEFI.
BOOT_CFLAGS := --target=x86_64-unknown-windows -std=c11 -O2 -ffreestanding \
	-fno-stack-protector -fno-strict-aliasing -mno-red-zone \
	-mno-stack-arg-probe -mgeneral-regs-only -Wall -Wextra -Werror -MMD -MP \
	$(INCLUDES) -isystem third_party/bearssl-shim

BOOT_LDFLAGS := -subsystem:efi_application -entry:efi_main -nodefaultlib \
	-machine:x64 -opt:ref

# BearSSL is configured for a freestanding build: no OS time or random sources
# (the loader supplies both), and no SSE or AES-NI code paths.
BEARSSL_CFLAGS := --target=x86_64-unknown-windows -std=c99 -O2 -ffreestanding \
	-fno-stack-protector -mno-red-zone -mno-stack-arg-probe -mgeneral-regs-only \
	-ffunction-sections -fdata-sections -w \
	-Ithird_party/bearssl-shim -I$(BEARSSL)/inc -I$(BEARSSL)/src \
	-DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 -DBR_USE_URANDOM=0 \
	-DBR_USE_GETENTROPY=0 -DBR_USE_WIN32_RAND=0 -DBR_RDRAND=0 \
	-DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_POWER8=0 -DBR_INT128=0 -DBR_UMUL128=0 \
	-DBR_64=1 -DBR_LE_UNALIGNED=1 -DBR_MSC=0 -DBR_amd64=0 -DBR_i386=0

# WSL can lose unflushed writes to the Windows drive, leaving zero-filled outputs
# with fresh timestamps that make would never rebuild. This deletes build/ first
# if any output is damaged, so the same make command rebuilds from scratch.
ifneq ($(filter-out clean,$(or $(MAKECMDGOALS),all)),)
$(shell python3 scripts/check_build.py >&2)
ifneq ($(.SHELLSTATUS),0)
$(error scripts/check_build.py failed)
endif
endif

.PHONY: all run debug test unit smoke usb anchors test-image clean

all: $(DISK)

$(BUILD)/boot/%.o: boot/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(BUILD)/gui/%.o: gui/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(BUILD)/fs/%.o: fs/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(BUILD)/net/%.o: net/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(BUILD)/web/%.o: web/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(BUILD)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(BEARSSL_CFLAGS) -c $< -o $@

# Regenerates the HTTPS trust anchors from the system's CA bundle.
anchors:
	python3 scripts/gen_anchors.py /etc/ssl/certs/ca-certificates.crt net/tls_anchors.c

$(SPLASH_C): $(SPLASH_PNG) scripts/mksplash.py
	@mkdir -p $(dir $@)
	python3 scripts/mksplash.py $(SPLASH_PNG) $(SPLASH_C) $(SPLASH_PGM)

$(SPLASH_OBJ): $(SPLASH_C)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

EFI_OBJS := $(BOOT_OBJS) $(GUI_OBJS) $(FS_OBJS) $(NET_OBJS) $(WEB_OBJS) $(BEARSSL_OBJS) $(SPLASH_OBJ)

$(EFI): $(EFI_OBJS)
	$(LD) $(BOOT_LDFLAGS) -out:$@ $^

$(DISK): $(EFI) scripts/mkimage.sh
	sh scripts/mkimage.sh $(EFI) $@

# The smoke test's image trusts only a throwaway test CA that tests/smoke.py
# generates. It is built separately and never replaces the real image.
TEST_ANCHORS_C   := $(BUILD)/test/tls_anchors_test.c
TEST_ANCHORS_OBJ := $(BUILD)/test/tls_anchors_test.o
TEST_EFI         := $(BUILD)/test/BOOTX64.EFI
TEST_DISK        := $(BUILD)/test/cato-test.img

$(TEST_ANCHORS_OBJ): $(TEST_ANCHORS_C)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(TEST_EFI): $(filter-out $(BUILD)/net/tls_anchors.o,$(EFI_OBJS)) $(TEST_ANCHORS_OBJ)
	$(LD) $(BOOT_LDFLAGS) -out:$@ $^

$(TEST_DISK): $(TEST_EFI) scripts/mkimage.sh
	sh scripts/mkimage.sh $(TEST_EFI) $@

test-image: $(TEST_DISK)

# Boot in a QEMU window with OVMF.
run: $(DISK)
	sh scripts/qemu.sh run $(DISK)

# Boot paused with a GDB stub on :1234 and an interrupt log in build/qemu.log.
debug: $(DISK)
	sh scripts/qemu.sh debug $(DISK)

test: unit smoke

unit:
	@mkdir -p $(BUILD)/test
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
		-Iboot tests/unit/test_boot_logic.c -o $(BUILD)/test/test_boot_logic
	$(BUILD)/test/test_boot_logic
	$(CC) $(TEST_CFLAGS) tests/unit/test_gui.c $(GUI_SRCS) $(FS_SRCS) $(NET_TEST_SRCS) \
		$(WEB_SRCS) -o $(BUILD)/test/test_gui
	$(BUILD)/test/test_gui
	$(CC) $(TEST_CFLAGS) tests/unit/test_fs.c $(FS_SRCS) -o $(BUILD)/test/test_fs
	$(BUILD)/test/test_fs
	$(CC) $(TEST_CFLAGS) tests/unit/test_net.c $(NET_TEST_SRCS) $(WEB_SRCS) \
		-o $(BUILD)/test/test_net
	$(BUILD)/test/test_net
	python3 tests/gen_test_images.py $(BUILD)/test/images
	$(CC) $(TEST_CFLAGS) tests/unit/test_image.c $(IMAGE_SRCS) -o $(BUILD)/test/test_image
	$(BUILD)/test/test_image $(BUILD)/test/images

smoke: $(DISK)
	python3 tests/smoke.py

# Builds an image FILE for real hardware. It never writes to a device.
usb: $(EFI)
	@test -n "$(IMG)" || { echo "usage: make usb IMG=build/cato-usb.img"; exit 2; }
	@sh scripts/mkimage.sh $(EFI) "$(IMG)"

clean:
	rm -rf $(BUILD)

-include $(BOOT_OBJS:.o=.d) $(GUI_OBJS:.o=.d) $(FS_OBJS:.o=.d) $(NET_OBJS:.o=.d) $(WEB_OBJS:.o=.d)
