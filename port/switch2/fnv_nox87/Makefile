#---------------------------------------------------------------------------------
# fnv.nro: the whole-game recompiler for the Switch 2. The lifted shards (out/recomp/recomp_NN.cpp, tools/recomp.py),
# the portable runtime and platform layer, host_posix.cpp in its __SWITCH__ form, host_window_switch2.cpp and the
# Vulkan renderer, linked with vkd3d-shader (cross-built, see SCOPE.md) and libounce/OunceVK.
#
# Build in the toolchain container (shards are large: GCC needs ~1-2 GB per job, so keep -j modest):
#   tools/switch2/build.sh port/switch2/fnv make -j6
# The game files are not inside the NRO: put fnv.nro next to FalloutNV.exe and Data/ in sdmc:/switch/fnv/.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libounce/switch2_rules

APP_TITLE	:=	Fallout NV (old build)
APP_AUTHOR	:=	fnv-recomp
APP_VERSION	:=	0.1
TARGET		:=	fnv_nox87
BUILD		:=	build
INCLUDES	:=

REPO	:=	$(abspath $(TOPDIR)/../../..)
RECOMP	:=	$(REPO)/out/recomp_nox87
PORTSRC	:=	$(REPO)/port/recomp
VKD3D	:=	$(REPO)/local/dl/vkd3d/switch2/vkd3d-1.19

SOURCES	:=	$(RECOMP) $(PORTSRC)
SHARDS	:=	$(notdir $(wildcard $(RECOMP)/recomp_[0-9][0-9].cpp))
PORT	:=	fnv_recomp_rt.cpp fnv_intrinsics.cpp pal_kernel.cpp native_math.cpp pal_fs_std.cpp pal_com.cpp pal_user.cpp \
			pal_input.cpp pal_start.cpp pal_dsound.cpp pal_d3d9.cpp pal_d3dx.cpp pal_dshow.cpp pal_bink.cpp \
			guest_heap.cpp host_posix.cpp host_window_switch2.cpp renderer_vk.cpp

OPT		?=	-O2
ARCH	:=	$(SWITCH2_CPU) -mtp=soft -fPIE
CFLAGS	:=	$(OPT) -ffunction-sections -fdata-sections $(ARCH) -w \
			-fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks \
			-DFNV_GUEST_BASE_RUNTIME -DFNV_HAVE_VORBIS -DFNV_HAVE_MPG123 -DFNV_HAVE_FFMPEG
CFLAGS	+=	$(INCLUDE) -D__SWITCH__ -I$(PORTSRC) -I$(VKD3D)/include
# hand-written NATIVE_FUNCS only when tools/recomp.py ran with --native (it leaves this marker); a switch needs
# `rm build/native_math.o` (make does not see the flag change)
ifneq ($(wildcard $(RECOMP)/native_math.on),)
CFLAGS	+=	-DFNV_NATIVE_MATH
endif
CXXFLAGS	:= $(CFLAGS) -std=gnu++17
ASFLAGS	:=	$(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,--gc-sections -Wl,-Map,$(notdir $*.map)
LIBS	:=	$(VKD3D)/.libs/libvkd3d-shader.a -lvulkan -lvorbisfile -lvorbis -logg -lmpg123 -lavformat -lavcodec -lswresample -lavutil -ldav1d -lbz2 -lz -lnx
LIBDIRS	:=	$(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export VPATH	:=	$(SOURCES)
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
export LD	:=	$(CXX)
export OFILES	:=	$(SHARDS:.cpp=.o) $(PORT:.cpp=.o)
export INCLUDE	:=	$(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export APP_ICON	:=	$(LIBNX)/default_icon.jpg
export NROFLAGS	+=	--icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: $(BUILD) clean all
all: $(BUILD)
$(BUILD):
	@[ -n "$(SHARDS)" ] || { echo "no lifted shards in $(RECOMP): run tools/recomp.py first"; exit 1; }
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
DEPENDS	:=	$(OFILES:.o=.d)
all	:	$(OUTPUT).nro
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf	:	$(OFILES)
-include $(DEPENDS)
endif
