ARCH ?= x86_64
TARGET := robu_kernel
BUILD_DIR := build
SRC_DIR := src
ARCH_DIR := arch/$(ARCH)
TRACE ?= 0
BUILD_SYS ?= clang

ifeq ($(BUILD_SYS),clang)
  CC := clang
  AS := clang -x assembler-with-cpp
  LD := ld.lld

  STRIP := $(shell command -v llvm-strip 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-strip)
else ifeq ($(BUILD_SYS),gcc)
  CC := $(ARCH)-elf-gcc
  AS := $(ARCH)-elf-as
  LD := $(ARCH)-elf-ld
  STRIP := $(ARCH)-elf-strip
else
  $(error Unknown BUILD_SYS '$(BUILD_SYS)' -- expected 'clang' or 'gcc')
endif

CFLAGS := -ffreestanding -O2 -g -Wall -Wextra -Iinclude -I$(ARCH_DIR)/include
CFLAGS += -fno-pic -fno-pie -fno-stack-protector
CFLAGS += -fno-omit-frame-pointer
CFLAGS += -DROBU_TRACE=$(TRACE)

ifeq ($(BUILD_SYS),clang)
	CFLAGS += --target=$(ARCH)-elf
endif

LDFLAGS += -nostdlib -T $(ARCH_DIR)/linker.ld -z noexecstack

ifeq ($(ARCH),x86_64)
	CFLAGS += -mno-red-zone -mno-mmx -mno-sse -mno-sse2
endif
GEN_DIR := $(BUILD_DIR)/generated
CFLAGS += -I$(GEN_DIR)

C_SRCS := $(wildcard $(SRC_DIR)/*/*.c) $(wildcard $(SRC_DIR)/*.c) $(wildcard $(ARCH_DIR)/src/*.c)
ASM_SRCS := $(wildcard $(ARCH_DIR)/src/*.S)

OBJS := $(C_SRCS:%.c=$(BUILD_DIR)/%.c.o) $(ASM_SRCS:%.S=$(BUILD_DIR)/%.S.o)

.PHONY: all _all clean mlibc _mlibc minibox run mlibc-hello iso _iso

all:
	./scripts/identify-os.sh _all

_all: $(BUILD_DIR)/$(TARGET) $(BUILD_DIR)/rootfs.tar

clean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR)/$(TARGET): $(OBJS)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/src/main.c.o:

$(BUILD_DIR)/%.S.o: %.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

APPS_BUILD_DIR := $(BUILD_DIR)/apps
APP_LDFLAGS := -nostdlib -static -z noexecstack

$(APPS_BUILD_DIR)/devfs/devfs.c.o: apps/devfs/devfs.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/devfs/devfs: $(APPS_BUILD_DIR)/devfs/devfs.c.o $(APP_COMMON_OBJ) apps/link/devfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/devfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/devfs/devfs.c.o $(APP_COMMON_OBJ)

$(APPS_BUILD_DIR)/ramfs/ramfs.c.o: apps/ramfs/ramfs.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/ramfs/ramfs: $(APPS_BUILD_DIR)/ramfs/ramfs.c.o $(APP_COMMON_OBJ) apps/link/ramfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/ramfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/ramfs/ramfs.c.o $(APP_COMMON_OBJ)
$(APPS_BUILD_DIR)/procfs/procfs.c.o: apps/procfs/procfs.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/procfs/procfs: $(APPS_BUILD_DIR)/procfs/procfs.c.o $(APP_COMMON_OBJ) apps/link/procfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/procfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/procfs/procfs.c.o $(APP_COMMON_OBJ)
$(APPS_BUILD_DIR)/sysfs/sysfs.c.o: apps/sysfs/sysfs.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/sysfs/sysfs: $(APPS_BUILD_DIR)/sysfs/sysfs.c.o $(APP_COMMON_OBJ) apps/link/sysfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/sysfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/sysfs/sysfs.c.o $(APP_COMMON_OBJ)

$(APPS_BUILD_DIR)/bootfs/bootfs.c.o: apps/bootfs/bootfs.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/bootfs/bootfs: $(APPS_BUILD_DIR)/bootfs/bootfs.c.o $(APP_COMMON_OBJ) apps/link/bootfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/bootfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/bootfs/bootfs.c.o $(APP_COMMON_OBJ)

$(APPS_BUILD_DIR)/diskfs/diskfs.c.o: apps/diskfs/diskfs.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/diskfs/diskfs: $(APPS_BUILD_DIR)/diskfs/diskfs.c.o $(APP_COMMON_OBJ) apps/link/diskfs.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/diskfs.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/diskfs/diskfs.c.o $(APP_COMMON_OBJ)

$(APPS_BUILD_DIR)/pager/pager.c.o: apps/pager/pager.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/pager/pager: $(APPS_BUILD_DIR)/pager/pager.c.o $(APP_COMMON_OBJ) apps/link/pager.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/pager.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/pager/pager.c.o $(APP_COMMON_OBJ)

$(APPS_BUILD_DIR)/powertools/reboot.c.o: apps/powertools/reboot.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/powertools/reboot: $(APPS_BUILD_DIR)/powertools/reboot.c.o apps/link/reboot.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/reboot.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/powertools/reboot.c.o

$(APPS_BUILD_DIR)/powertools/halt.c.o: apps/powertools/halt.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/powertools/halt: $(APPS_BUILD_DIR)/powertools/halt.c.o apps/link/halt.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/halt.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/powertools/halt.c.o

$(APPS_BUILD_DIR)/powertools/shutdown.c.o: apps/powertools/shutdown.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/powertools/shutdown: $(APPS_BUILD_DIR)/powertools/shutdown.c.o apps/link/shutdown.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/shutdown.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/powertools/shutdown.c.o

$(APPS_BUILD_DIR)/sigtest/sigtest.c.o: apps/sigtest/sigtest.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/sigtest/sigtest: $(APPS_BUILD_DIR)/sigtest/sigtest.c.o apps/link/sigtest.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/sigtest.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/sigtest/sigtest.c.o

$(APPS_BUILD_DIR)/consoletest/consoletest.c.o: apps/consoletest/consoletest.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/consoletest/consoletest: $(APPS_BUILD_DIR)/consoletest/consoletest.c.o apps/link/consoletest.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/consoletest.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/consoletest/consoletest.c.o

MLIBC_DIR := apps/mlibc
MLIBC_BUILD_DIR := $(BUILD_DIR)/mlibc
MLIBC_SYSROOT := $(abspath $(BUILD_DIR)/mlibc-sysroot)
MLIBC_CROSS := apps/mlibc-robu-cross.ini

MLIBC_TOOLCHAIN_PATH := /opt/homebrew/opt/llvm/bin:$(PATH)

mlibc:
	./scripts/identify-os.sh _mlibc

_mlibc:
	@if [ ! -f $(MLIBC_BUILD_DIR)/build.ninja ]; then \
	    PATH="$(MLIBC_TOOLCHAIN_PATH)" meson setup $(MLIBC_BUILD_DIR) $(MLIBC_DIR) --cross-file $(MLIBC_CROSS) \
	        -Dlibgcc_dependency=false -Ddefault_library=static --prefix=/usr; \
	fi
	PATH="$(MLIBC_TOOLCHAIN_PATH)" ninja -C $(MLIBC_BUILD_DIR)
	DESTDIR=$(MLIBC_SYSROOT) PATH="$(MLIBC_TOOLCHAIN_PATH)" ninja -C $(MLIBC_BUILD_DIR) install

MLIBC_APP_CFLAGS := --target=x86_64-linux-gnu -ffreestanding -fPIC -fno-stack-protector \
                     -mno-red-zone -D_GNU_SOURCE -Wall -Wextra \
                     -isystem $(MLIBC_SYSROOT)/usr/include

MLIBC_CRT_OBJS := $(MLIBC_SYSROOT)/usr/lib/crt1.o
MLIBC_LIBS := --start-group $(MLIBC_SYSROOT)/usr/lib/libc.a $(MLIBC_SYSROOT)/usr/lib/libm.a --end-group

CONFUSE_DIR := apps/confuse/src
CONFUSE_BUILD_DIR := $(APPS_BUILD_DIR)/confuse
CONFUSE_CFLAGS := $(MLIBC_APP_CFLAGS) -I$(CONFUSE_DIR) \
                   -DHAVE_UNISTD_H -DHAVE_SYS_STAT_H -DHAVE_STRING_H \
                   -DHAVE_STRDUP -DHAVE_STRNDUP -DHAVE_STRCASECMP -DHAVE_FMEMOPEN \
                   -DPACKAGE=\"libconfuse\" -DPACKAGE_VERSION=\"3.3\" -DPACKAGE_STRING=\"libconfuse-3.3\" \
                   -DCONFUSE_NO_FLOAT \
                   -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-sign-compare

$(CONFUSE_BUILD_DIR)/lexer.c: $(CONFUSE_DIR)/lexer.l
	@mkdir -p $(@D)
	flex -Pcfg_yy -o $@ $<

$(CONFUSE_BUILD_DIR)/lexer.c.o: $(CONFUSE_BUILD_DIR)/lexer.c
	@mkdir -p $(@D)
	$(CC) $(CONFUSE_CFLAGS) -Wno-unused-but-set-variable -c $< -o $@

$(CONFUSE_BUILD_DIR)/confuse.c.o: $(CONFUSE_DIR)/confuse.c
	@mkdir -p $(@D)
	$(CC) $(CONFUSE_CFLAGS) -c $< -o $@

$(CONFUSE_BUILD_DIR)/reallocarray.c.o: $(CONFUSE_DIR)/reallocarray.c
	@mkdir -p $(@D)
	$(CC) $(CONFUSE_CFLAGS) -c $< -o $@

CONFUSE_OBJS := $(CONFUSE_BUILD_DIR)/confuse.c.o $(CONFUSE_BUILD_DIR)/lexer.c.o $(CONFUSE_BUILD_DIR)/reallocarray.c.o

$(APPS_BUILD_DIR)/hello_initsys/main.c.o: apps/hello_initsys/main.c mlibc
	@mkdir -p $(@D)
	$(CC) $(MLIBC_APP_CFLAGS) -I$(CONFUSE_DIR) -c $< -o $@

$(APPS_BUILD_DIR)/hello_initsys/hello_initsys: $(APPS_BUILD_DIR)/hello_initsys/main.c.o $(CONFUSE_OBJS) apps/link/hello_initsys.ld
	ld.lld -nostdlib -static -T apps/link/hello_initsys.ld -e _start -o $@ \
	    $(MLIBC_CRT_OBJS) $(APPS_BUILD_DIR)/hello_initsys/main.c.o $(CONFUSE_OBJS) $(MLIBC_LIBS)
	$(STRIP) --strip-all $@

MINIBOX_SRCS := $(filter-out apps/minibox/src/init.c,$(wildcard apps/minibox/src/*.c)) \
                $(wildcard apps/minibox/libmb/*.c) apps/minibox/robu-stubs.c
MINIBOX_OBJS := $(patsubst apps/minibox/%.c,$(APPS_BUILD_DIR)/minibox/%.c.o,$(MINIBOX_SRCS))
MINIBOX_CFLAGS := $(MLIBC_APP_CFLAGS) -Iapps/minibox/include -Iapps/minibox/libmb \
                   -Iinclude -I$(ARCH_DIR)/include \
                   -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -Wno-unused-result \
                   -DVERSION=\"0.3.1\" -include apps/minibox/include/config.h

$(MINIBOX_OBJS): $(APPS_BUILD_DIR)/minibox/%.c.o: apps/minibox/%.c mlibc
	@mkdir -p $(@D)
	$(CC) $(MINIBOX_CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/minibox/minibox: $(MINIBOX_OBJS) apps/link/minibox.ld
	ld.lld -nostdlib -static -T apps/link/minibox.ld -e _start -o $@ \
	    $(MLIBC_CRT_OBJS) $(MINIBOX_OBJS) $(MLIBC_LIBS)
	$(STRIP) --strip-all $@

minibox: $(APPS_BUILD_DIR)/minibox/minibox

SH_CFLAGS := $(MLIBC_APP_CFLAGS) -Wno-unused-parameter -Wno-sign-compare

$(APPS_BUILD_DIR)/sh/minibox-shell.c.o: apps/minibox/minibox-shell/minibox-shell.c mlibc
	@mkdir -p $(@D)
	$(CC) $(SH_CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/sh/sh: $(APPS_BUILD_DIR)/sh/minibox-shell.c.o apps/link/sh.ld
	ld.lld -nostdlib -static -T apps/link/sh.ld -e _start -o $@ \
	    $(MLIBC_CRT_OBJS) $(APPS_BUILD_DIR)/sh/minibox-shell.c.o $(MLIBC_LIBS)
	$(STRIP) --strip-all $@

sh: $(APPS_BUILD_DIR)/sh/sh

$(APPS_BUILD_DIR)/mlibc-hello/hello.c.o: apps/mlibc-hello/hello.c mlibc
	@mkdir -p $(@D)
	$(CC) $(MLIBC_APP_CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/mlibc-hello/hello: $(APPS_BUILD_DIR)/mlibc-hello/hello.c.o apps/link/mlibchello.ld
	ld.lld -nostdlib -static -T apps/link/mlibchello.ld -e _start -o $@ \
	    $(MLIBC_CRT_OBJS) $(APPS_BUILD_DIR)/mlibc-hello/hello.c.o $(MLIBC_LIBS)
	$(STRIP) --strip-all $@

mlibc-hello: $(APPS_BUILD_DIR)/mlibc-hello/hello

AR := $(shell command -v llvm-ar 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-ar)

READLINE_DIR := apps/readline
READLINE_BUILD_DIR := $(APPS_BUILD_DIR)/readline
READLINE_CFLAGS := $(MLIBC_APP_CFLAGS) -I$(READLINE_DIR) -DHAVE_CONFIG_H \
                    -DRL_LIBRARY_VERSION=\"8.3\" \
                    -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
                    -Wno-unused-but-set-variable -Wno-sign-compare -Wno-implicit-fallthrough

READLINE_SRCS := readline.c funmap.c keymaps.c vi_mode.c parens.c rltty.c \
                  complete.c bind.c isearch.c display.c signals.c emacs_keymap.c \
                  vi_keymap.c util.c kill.c undo.c macro.c input.c \
                  callback.c terminal.c xmalloc.c xfree.c \
                  history.c histsearch.c histexpand.c histfile.c nls.c search.c \
                  shell.c savestring.c tilde.c text.c misc.c compat.c \
                  mbutil.c gettimeofday.c colors.c parse-colors.c \
                  robu-termcap.c

READLINE_OBJS := $(patsubst %.c,$(READLINE_BUILD_DIR)/%.c.o,$(READLINE_SRCS))

$(READLINE_BUILD_DIR)/%.c.o: $(READLINE_DIR)/%.c mlibc
	@mkdir -p $(@D)
	@[ -L $(READLINE_DIR)/readline ] || ln -s . $(READLINE_DIR)/readline
	$(CC) $(READLINE_CFLAGS) -c $< -o $@

$(READLINE_BUILD_DIR)/libreadline.a: $(READLINE_OBJS)
	$(AR) rcs $@ $(READLINE_OBJS)

readline: $(READLINE_BUILD_DIR)/libreadline.a

BASH_DIR := apps/bash
BASH_BUILD_DIR := $(APPS_BUILD_DIR)/bash

$(BASH_DIR)/config.h: $(BASH_DIR)/robu.cache mlibc readline
	cd $(BASH_DIR) && CC="clang --target=x86_64-linux-gnu" \
	    CC_FOR_BUILD=clang \
	    CFLAGS="-ffreestanding -fPIC -fno-stack-protector -mno-red-zone -D_GNU_SOURCE -nostdinc -isystem $$(clang --print-resource-dir)/include -isystem $(MLIBC_SYSROOT)/usr/include" \
	    CPPFLAGS="-I$(abspath $(READLINE_DIR))" \
	    ./configure --host=x86_64-linux-gnu --build=x86_64-pc-linux-gnu \
	        --cache-file=$(abspath $(BASH_DIR)/robu.cache) \
	        --without-bash-malloc --enable-readline --enable-job-control \
	        --disable-nls --disable-rpath --without-libiconv-prefix --without-libintl-prefix

bash-configure: $(BASH_DIR)/config.h

bash-build: $(BASH_DIR)/config.h
	touch $(BASH_DIR)/configure.ac $(BASH_DIR)/aclocal.m4 $(BASH_DIR)/config.h.in
	touch $(BASH_DIR)/configure
	$(MAKE) -C $(BASH_DIR)

$(APPS_BUILD_DIR)/readlinetest/readlinetest.c.o: apps/readlinetest/readlinetest.c mlibc
	@mkdir -p $(@D)
	@[ -L $(READLINE_DIR)/readline ] || ln -s . $(READLINE_DIR)/readline
	$(CC) $(MLIBC_APP_CFLAGS) -I$(READLINE_DIR) -c $< -o $@

$(APPS_BUILD_DIR)/readlinetest/readlinetest: $(APPS_BUILD_DIR)/readlinetest/readlinetest.c.o $(READLINE_BUILD_DIR)/libreadline.a apps/link/readlinetest.ld
	ld.lld -nostdlib -static -T apps/link/readlinetest.ld -e _start -o $@ \
	    $(MLIBC_CRT_OBJS) $(APPS_BUILD_DIR)/readlinetest/readlinetest.c.o $(READLINE_BUILD_DIR)/libreadline.a $(MLIBC_LIBS)
	$(STRIP) --strip-all $@

readlinetest: $(APPS_BUILD_DIR)/readlinetest/readlinetest

$(APPS_BUILD_DIR)/stub/stub.c.o: apps/stub/stub.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/stub/stub: $(APPS_BUILD_DIR)/stub/stub.c.o apps/link/stub.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/stub.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/stub/stub.c.o

ROOTFS_STAGE := $(BUILD_DIR)/rootfs-stage
ROOTFS_MINIBOX_ALIASES := ls cat touch tail cp
ROOTFS_STUB_NAMES := root_task file find mv
ROOTFS_MINIBOX_SYMLINKS := basename cal cat clear cmp cp cut date \
                            dirname echo env expand factor false fold \
                            free grep head hexdump hostname kill link \
                            ls mkdir mknod nohup od paste ps rm \
                            rmdir sleep sort sync tail touch tr true \
                            tty unexpand uniq unlink update uptime vmstat \
                            w wc whoami xxd yes

$(BUILD_DIR)/rootfs.tar: $(APPS_BUILD_DIR)/devfs/devfs $(APPS_BUILD_DIR)/ramfs/ramfs \
                         $(APPS_BUILD_DIR)/procfs/procfs $(APPS_BUILD_DIR)/sysfs/sysfs \
                         $(APPS_BUILD_DIR)/bootfs/bootfs $(APPS_BUILD_DIR)/diskfs/diskfs \
                         $(APPS_BUILD_DIR)/pager/pager \
                         $(APPS_BUILD_DIR)/powertools/reboot \
                         $(APPS_BUILD_DIR)/powertools/halt \
                         $(APPS_BUILD_DIR)/powertools/shutdown \
                         $(APPS_BUILD_DIR)/sigtest/sigtest \
                         $(APPS_BUILD_DIR)/consoletest/consoletest \
                         $(APPS_BUILD_DIR)/mlibc-hello/hello \
                         $(APPS_BUILD_DIR)/am/am \
                         $(APPS_BUILD_DIR)/top/top \
                         $(APPS_BUILD_DIR)/readlinetest/readlinetest \
                         $(APPS_BUILD_DIR)/hello_initsys/hello_initsys \
                         $(APPS_BUILD_DIR)/minibox/minibox \
                         $(APPS_BUILD_DIR)/sh/sh \
                         $(APPS_BUILD_DIR)/stub/stub \
                         apps/hello_initsys/rc.conf etc/passwd
	rm -rf $(ROOTFS_STAGE)
	mkdir -p $(ROOTFS_STAGE) $(ROOTFS_STAGE)/bin $(ROOTFS_STAGE)/sbin \
	         $(ROOTFS_STAGE)/etc $(ROOTFS_STAGE)/usr/bin $(ROOTFS_STAGE)/usr/sbin
	cp $(APPS_BUILD_DIR)/devfs/devfs $(ROOTFS_STAGE)/devfs
	cp $(APPS_BUILD_DIR)/ramfs/ramfs $(ROOTFS_STAGE)/ramfs
	cp $(APPS_BUILD_DIR)/procfs/procfs $(ROOTFS_STAGE)/procfs
	cp $(APPS_BUILD_DIR)/sysfs/sysfs $(ROOTFS_STAGE)/sysfs
	cp $(APPS_BUILD_DIR)/bootfs/bootfs $(ROOTFS_STAGE)/bootfs
	cp $(APPS_BUILD_DIR)/diskfs/diskfs $(ROOTFS_STAGE)/diskfs
	cp $(APPS_BUILD_DIR)/pager/pager $(ROOTFS_STAGE)/pager
	cp $(APPS_BUILD_DIR)/sigtest/sigtest $(ROOTFS_STAGE)/sigtest
	cp $(APPS_BUILD_DIR)/consoletest/consoletest $(ROOTFS_STAGE)/consoletest
	cp $(APPS_BUILD_DIR)/mlibc-hello/hello $(ROOTFS_STAGE)/mlibc-hello
	cp $(APPS_BUILD_DIR)/readlinetest/readlinetest $(ROOTFS_STAGE)/readlinetest
	cp $(APPS_BUILD_DIR)/powertools/reboot $(ROOTFS_STAGE)/sbin/reboot
	cp $(APPS_BUILD_DIR)/powertools/halt $(ROOTFS_STAGE)/sbin/halt
	cp $(APPS_BUILD_DIR)/powertools/shutdown $(ROOTFS_STAGE)/sbin/shutdown
	cp $(APPS_BUILD_DIR)/am/am $(ROOTFS_STAGE)/bin/am
	cp $(APPS_BUILD_DIR)/top/top $(ROOTFS_STAGE)/bin/top
	cp $(APPS_BUILD_DIR)/hello_initsys/hello_initsys $(ROOTFS_STAGE)/usr/sbin/hello_initsys
	cp apps/hello_initsys/rc.conf $(ROOTFS_STAGE)/etc/rc.conf
	cp etc/passwd $(ROOTFS_STAGE)/etc/passwd
	cp $(APPS_BUILD_DIR)/minibox/minibox $(ROOTFS_STAGE)/bin/minibox
	cp $(APPS_BUILD_DIR)/sh/sh $(ROOTFS_STAGE)/bin/sh
	cp $(APPS_BUILD_DIR)/stub/stub $(ROOTFS_STAGE)/bin/file
	cp $(APPS_BUILD_DIR)/stub/stub $(ROOTFS_STAGE)/bin/find
	cp $(APPS_BUILD_DIR)/stub/stub $(ROOTFS_STAGE)/bin/mv
	for n in $(ROOTFS_MINIBOX_ALIASES); do cp $(APPS_BUILD_DIR)/minibox/minibox $(ROOTFS_STAGE)/$$n; done
	for n in $(ROOTFS_STUB_NAMES); do cp $(APPS_BUILD_DIR)/stub/stub $(ROOTFS_STAGE)/$$n; done
	for n in $(ROOTFS_MINIBOX_SYMLINKS); do ln -sf bin/minibox $(ROOTFS_STAGE)/bin/$$n; done
	(cd $(ROOTFS_STAGE) && tar --format ustar -cf $(abspath $@) $$(ls))

QEMU ?= qemu-system-x86_64
QEMU_SMP ?= 2
QEMU_MEM ?= 256
QEMU_APPEND ?= root=root_task starter=hello_initsys

QEMU_DISK ?= $(BUILD_DIR)/diskfs.img

GRUB_DOCKER ?= docker
GRUB_DOCKER_IMAGE ?= ubuntu:24.04

$(BUILD_DIR)/robu_kernel.iso: $(BUILD_DIR)/$(TARGET) $(BUILD_DIR)/rootfs.tar iso/boot/grub/grub.cfg.in
	@mkdir -p $(BUILD_DIR)/iso_root/boot/grub
	cp $(BUILD_DIR)/$(TARGET) $(BUILD_DIR)/iso_root/boot/robu_kernel
	cp $(BUILD_DIR)/rootfs.tar $(BUILD_DIR)/iso_root/boot/rootfs.tar
	sed 's|@QEMU_APPEND@|$(QEMU_APPEND)|' iso/boot/grub/grub.cfg.in \
	    > $(BUILD_DIR)/iso_root/boot/grub/grub.cfg
	@if command -v grub-mkrescue >/dev/null 2>&1; then \
	    grub-mkrescue -o $(BUILD_DIR)/robu_kernel.iso $(BUILD_DIR)/iso_root; \
	else \
	    $(GRUB_DOCKER) run --rm --platform=linux/amd64 -v $(abspath $(BUILD_DIR)):/build $(GRUB_DOCKER_IMAGE) \
	        bash -c "apt-get update -qq && apt-get install -y -qq grub-pc-bin grub-common xorriso mtools >/dev/null 2>&1 && grub-mkrescue -o /build/robu_kernel.iso /build/iso_root"; \
	fi

iso:
	./scripts/identify-os.sh _iso

_iso: $(BUILD_DIR)/robu_kernel.iso

run: $(BUILD_DIR)/robu_kernel.iso
	@test -f $(QEMU_DISK) || truncate -s 16M $(QEMU_DISK)
	$(QEMU) -cdrom $(BUILD_DIR)/robu_kernel.iso \
	    -smp $(QEMU_SMP) -m $(QEMU_MEM) \
	    -nographic -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	    -drive file=$(QEMU_DISK),format=raw,if=none,id=blk0 \
	    -device virtio-blk-pci,drive=blk0

APP_BINS := $(APPS_BUILD_DIR)/procfs/procfs $(APPS_BUILD_DIR)/sysfs/sysfs \
            $(APPS_BUILD_DIR)/hello_initsys/hello_initsys $(APPS_BUILD_DIR)/minibox/minibox \


# --- TUI AM BUILD RULES (NATIVE FREESTANDING) ---
$(APPS_BUILD_DIR)/am/am.c.o: apps/am/am.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/am/am: $(APPS_BUILD_DIR)/am/am.c.o apps/link/am.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/am.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/am/am.c.o
	$(STRIP) --strip-all $@

# --- TUI TOP BUILD RULES (NATIVE FREESTANDING) ---
$(APPS_BUILD_DIR)/top/top.c.o: apps/top/top.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(APPS_BUILD_DIR)/top/top: $(APPS_BUILD_DIR)/top/top.c.o apps/link/top.ld
	$(LD) $(APP_LDFLAGS) -T apps/link/top.ld -e _start -o $@ \
	    $(APPS_BUILD_DIR)/top/top.c.o
	$(STRIP) --strip-all $@
