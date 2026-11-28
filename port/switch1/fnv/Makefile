# The game host for Switch 1 (NRO): host_switch1.cpp, the recompiler runtime, the PAL kernel layer and the guest allocator
# with an empty function table (no game code). Build with tools/switch1/build.sh port/switch1/fnv
include $(DEVKITPRO)/libnx/switch_rules

TARGET   := fnv
BUILD    := build
RECOMP   := ../../recomp
APP_TITLE   := fnv1
APP_AUTHOR  := fnvOunce
APP_VERSION := 0.1.0
CPPFILES := host_switch1.cpp fnv_recomp_rt.cpp pal_kernel.cpp pal_fs_std.cpp guest_heap.cpp

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS   := -g -Wall -Wno-unused-function -O2 -ffunction-sections $(ARCH) $(INCLUDE) -D__SWITCH__ \
            -DFNV_GUEST_BASE_RUNTIME -fno-strict-aliasing -fwrapv
CXXFLAGS := $(CFLAGS) -std=gnu++17
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS     := -lnx
LIBDIRS  := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(CURDIR)/$(RECOMP)
export DEPSDIR  := $(CURDIR)/$(BUILD)
export LD       := $(CXX)
export OFILES   := $(CPPFILES:.cpp=.o)
export INCLUDE  := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(RECOMP) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export NROFLAGS := --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: all clean
all:
	@mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@rm -rf $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)
-include $(DEPSDIR)/*.d
endif
