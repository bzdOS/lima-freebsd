# SPDX-License-Identifier: BSD-2-Clause
#
# hal/lima/Makefile — FreeBSD kmod build for Lima (Mali-400 DRM driver)
#
# CROSS-BUILD ON THE LINUX HOST (preferred; no FreeBSD VM involved)
#
# This used to say a FreeBSD dev VM was required. It is not -- verified
# 2026-08-19, producing a working lima.ko that rendered the project's first frame.
#
#   git clone --depth 1 -b drm_v6.6.25_13 \
#       https://github.com/freebsd/drm-kmod.git /opt/bzdos/drm-kmod-src
#   (cd /opt/bzdos/drm-kmod-src && \
#    for p in ../bsdOS/hal/lima/patches/drm-kmod/*.patch; do patch -p1 -i $p; done)
#
#    NOTE THE drm-kmod/ IN THAT GLOB. patches/ is split by TARGET TREE --
#    patches/drm-kmod/ and patches/freebsd-src/ -- and globbing patches/*.patch
#    would feed a freebsd-src patch to drm-kmod, which is exactly the mistake
#    patches/README.md warns against. Until 2026-08-20 three patches also existed
#    as byte-identical copies at the top level, so the wrong glob appeared to
#    work while silently missing drm-kmod-nonpci-busid.patch; the duplicates are
#    gone.
#
#   MAKEOBJDIRPREFIX=/opt/bzdos/fbsd-obj bmake \
#       -m /opt/bzdos/freebsd-src-earlyboot-wt/share/mk \
#       MACHINE=arm64 MACHINE_ARCH=aarch64 \
#       SYSDIR=/opt/bzdos/freebsd-src-earlyboot-wt/sys \
#       DRM_KMOD_SRC=/opt/bzdos/drm-kmod-src \
#       CC="clang --target=aarch64-unknown-freebsd15.1" LD=ld.lld \
#       COMPILER_TYPE=clang COMPILER_VERSION=210100 \
#       XARGS_J=-I OBJCOPY=llvm-objcopy NM=llvm-nm
#
# Four non-obvious bits, each of which stops the build dead:
#   - MACHINE/MACHINE_ARCH, NOT TARGET/TARGET_ARCH. The latter only work in the
#     top-level buildkernel wrapper; a standalone kmod build reads MACHINE and
#     otherwise goes looking for sys/x86_64/include.
#   - -m <freebsd-src>/share/mk, or bsd.kmod.mk and bsd.compiler.mk are missing
#     and COMPILER_TYPE never gets set.
#   - XARGS_J=-I. kmod.mk's symbol step uses xargs -J, which is BSD-only; -I is
#     GNU xargs' closest equivalent.
#   - the patches under patches/ are mandatory, not optional: dma_buf_mmap() is
#     otherwise undeclared and drm_gem_shmem_helper.c will not compile.
#
# Then push the module md5-verified -- a silently corrupt push has happened here,
# with the right size and the wrong content, so verify rather than trust.
#
# kldunload/kldload NOW WORKS (2026-08-21). This used to say "REBOOT THE GUEST
# rather than kldunload/kldload -- reloading leaves drm's sysctls behind and
# breaks eglInitialize", and that was true: drm_sysctl_init() put the SHARED
# hw.dri node into the per-device sysctl context, so sysctl_ctx_free() failed
# with EBUSY and removed nothing while cleanup freed the struct those nodes
# pointed into. Mesa reads hw.dri.<N>.busid, hence the broken eglInitialize; an
# unprivileged `sysctl -a` panicked the kernel outright. Fixed by
# patches/drm-kmod/drm-kmod-dri-sysctl-lifecycle.patch -- verified over 5
# load/GL/sysctl -a/unload cycles, 0 leaked nodes and GL working every time.
# A reboot is no longer required; a reload is fine.
#
# ON A FREEBSD HOST (the old route, kept for reference):
#   pkg install drm-66-kmod
#   cd /usr/ports/graphics/drm-66-kmod && make fetch extract
#   cd .../hal/lima && su -m root -c "make"
#   kldload drm.ko && kldload ./lima.ko && dmesg | grep lima
#
# Target: FreeBSD 15.1 aarch64, Banana Pi M64 (Allwinner A64, Mali-400 MP2)
#
# This Makefile is arch-neutral: nothing here names amd64 or aarch64. It used to
# hardcode KOBJ_DIR=/usr/obj/.../amd64.amd64/sys/BSDOS-SQUIRREL-amd64, which both
# pinned the build to amd64 and required a completed kernel build to exist. Both
# problems have the same fix: the six generated headers this driver needs are
# listed in SRCS below, and sys/conf/kmod.mk generates them into the module
# objdir from ${SYSDIR} alone. That is how drm-kmod's own drm/Makefile does it.
# To cross-build for another arch, just set MACHINE/MACHINE_ARCH (or use
# `make buildenv`); see hal/lima/README-arm64.md for a worked aarch64 example.

KMOD=	lima

SRCS=	lima_drv.c \
	lima_device.c \
	lima_mmu.c \
	lima_gp.c \
	lima_pp.c \
	lima_sched.c \
	lima_gem.c \
	lima_vm.c \
	lima_ctx.c \
	lima_pmu.c \
	lima_l2_cache.c \
	lima_dlbu.c \
	lima_bcast.c \
	lima_ccu_debug.c

# FreeBSD's own DRM GEM SHMEM helper. drm-kmod does not ship one (no header, no
# .c, no mention of "shmem" in its drm/Makefile), which is why every
# render-only SoC DRM driver links but cannot kldload on FreeBSD: the
# drm_gem_shmem_* symbols resolve nowhere. drm/drm_gem_shmem_helper.c is a real
# implementation of that API on top of FreeBSD OBJT_SWAP vm_objects; it is
# driver-agnostic and belongs in drm.ko, so it is a separate knob here. Set
# LIMA_PROVIDE_GEM_SHMEM=no once drm-kmod (or a bsdOS drm.ko) exports it, to
# avoid two definitions of the same symbols.
LIMA_PROVIDE_GEM_SHMEM?=	yes
.if ${LIMA_PROVIDE_GEM_SHMEM} == "yes"
# Found through .PATH rather than named as drm/drm_gem_shmem_helper.c: bmake
# would then try to write drm/drm_gem_shmem_helper.o into a subdirectory of the
# objdir that kmod.mk never creates.
.PATH:	${.CURDIR}/drm
SRCS+=	drm_gem_shmem_helper.c
.endif

# Second symbol drm-kmod defines nowhere: drm_timeout_abs_to_jiffies(). Its
# header (include/drm/drm_utils.h) is shipped but drivers/gpu/drm/drm_utils.c
# does not exist in the repository, so every driver using the absolute-timeout
# wait ioctl pattern references a symbol no module defines. Same knob rationale
# as LIMA_PROVIDE_GEM_SHMEM above.
LIMA_PROVIDE_DRM_UTILS?=	yes
.if ${LIMA_PROVIDE_DRM_UTILS} == "yes"
.PATH:	${.CURDIR}/drm
SRCS+=	drm_utils_freebsd.c
.endif

# FreeBSD's newbus/FDT <-> LinuxKPI platform_device bridge. linuxkpi's
# linux/platform_device.h is a stub whose platform_driver_register() returns
# -ENXIO unconditionally, so module_platform_driver() makes module init fail and
# the driver's probe routine is unreachable dead code. linux/platform_device.{h,c}
# and linux/interrupt.h here are a real implementation: newbus probe/attach over
# an FDT node, MMIO + IRQ resources, busdma tags, and an interrupt path that
# does not go through linuxkpi's PCI-only lkpi_pci_find_irq_dev().
#
# Like the GEM SHMEM helper this is driver-agnostic and belongs in linuxkpi
# rather than in a driver; set LIMA_PROVIDE_PLATFORM_DEVICE=no once FreeBSD's
# own linuxkpi grows a working platform bus.
LIMA_PROVIDE_PLATFORM_DEVICE?=	yes
.if ${LIMA_PROVIDE_PLATFORM_DEVICE} == "yes"
.PATH:	${.CURDIR}/linux
SRCS+=	platform_device.c
.endif

# Generated kernel headers. Listing them in SRCS makes sys/conf/kmod.mk build
# them into the module objdir from ${SYSDIR}, so no pre-existing kernel obj tree
# (and no arch-specific KOBJ_DIR) is required. This exact set is what the driver
# actually pulls in — verified against the .depend files of a completed build.
SRCS+=	device_if.h \
	bus_if.h \
	pci_if.h \
	vnode_if.h

# ofw_bus_if.h is only needed by the platform_device bridge (dev/ofw/ofw_bus.h).
.if ${LIMA_PROVIDE_PLATFORM_DEVICE} == "yes"
SRCS+=	ofw_bus_if.h
.endif

# linux/clk.h and linux/reset.h (real clk(9)/hwreset(9) bindings, replacing the
# no-op stubs that shipped until 2026-08-11 — see those headers for why that
# was a real hardware bug, not a cosmetic one) pull in <dev/clk/clk.h> and
# <dev/hwreset/hwreset.h> directly. Both are FDT consumer APIs: their
# OFW-based entry points (clk_get_by_ofw_name(), hwreset_get_by_ofw_name(),
# hwreset_array_get_ofw()) are compiled out unless FDT is defined, which
# normally comes from a kernel config's `options FDT` via opt_platform.h. This
# module has no KERNBUILDDIR (see the header comment above about not requiring
# a prebuilt kernel obj tree), so sys/conf/kmod.mk emits an *empty*
# opt_platform.h instead of one pulled from a real kernel build — the same
# situation sys/modules/neta and sys/modules/vnic are in, and they solve it
# the same way: force FDT directly via CFLAGS rather than relying on the
# generated (empty) header to define it. clk.h additionally needs
# clknode_if.h (KOBJ method dispatch table for clknode).
CFLAGS+=	-DFDT
SRCS+=	opt_platform.h \
	clknode_if.h

# Overridable so the same tree can build against a ports checkout, an extracted
# tarball, or a git worktree of drm-kmod.
DRM_KMOD_SRC?=	/usr/ports/graphics/drm-66-kmod/work/drm-kmod-drm_v6.6.25_13

.include <bsd.kmod.mk>

# Include order mirrors drm-kmod/drm/Makefile exactly
.include "${DRM_KMOD_SRC}/linuxkpi_version.mk"
.include "${DRM_KMOD_SRC}/compiler_flags.mk"

# Local Lima stubs must come FIRST to override empty linuxkpi headers
# (e.g. linux/clk.h, linux/reset.h, linux/regulator/consumer.h)
CFLAGS+=	-I${.CURDIR}
CFLAGS+=	-I${DRM_KMOD_SRC}/linuxkpi/gplv2/include
CFLAGS+=	-I${DRM_KMOD_SRC}/linuxkpi/bsd/include
CFLAGS+=	-I${SYSDIR}/compat/linuxkpi/common/include
CFLAGS+=	-I${SYSDIR}/compat/linuxkpi/dummy/include
CFLAGS+=	-I${DRM_KMOD_SRC}/include
CFLAGS+=	-I${DRM_KMOD_SRC}/include/drm
CFLAGS+=	-I${DRM_KMOD_SRC}/include/uapi
CFLAGS+=	-I${DRM_KMOD_SRC}/drivers/gpu/drm
CFLAGS+=	-I${DRM_KMOD_SRC}/drivers/gpu
CFLAGS+=	'-DKBUILD_MODNAME="${KMOD}"'
CFLAGS+=	'-DLINUXKPI_PARAM_PREFIX=lima_'
CFLAGS+=	-DDRM_SYSCTL_PARAM_PREFIX=_dri
# Lima uses void-pointer arithmetic in io macros (valid GNU C extension)
CFLAGS+=	-Wno-pointer-arith

# ── Userspace tests (no kernel required) ─────────────────────────────────
# Run on any host with a C compiler:
#   gmake test          (both suites)
#   gmake test-layout   (Lima VM/dump layout and arithmetic)
#   gmake test-shmem    (GEM SHMEM helper refcount/offset logic)
# test-shmem compiles the *shipped* drm/drm_gem_shmem_logic.h, not a copy of
# it, so a regression in the helper's arithmetic fails the test.
TEST_BIN=	tests/test_lima_math
TEST_SHMEM_BIN=	tests/test_shmem_logic

test: test-layout test-shmem

test-layout:
	cc -Wall -Wextra -o ${TEST_BIN} tests/test_lima_math.c && ./${TEST_BIN}

test-shmem:
	cc -Wall -Wextra -o ${TEST_SHMEM_BIN} tests/test_shmem_logic.c && \
	    ./${TEST_SHMEM_BIN}

clean-test:
	rm -f ${TEST_BIN} ${TEST_SHMEM_BIN}
