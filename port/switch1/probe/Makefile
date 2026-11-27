# Console probe (NRO). Build with tools/switch1/build.sh port/switch1/probe
include $(DEVKITPRO)/libnx/switch_rules

TARGET   := probe
BUILD    := build
SOURCES  := source
APP_TITLE   := fnv probe
APP_AUTHOR  := fnvOunce
APP_VERSION := 0.1.0

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS   := -g -Wall -O2 -ffunction-sections $(ARCH) $(INCLUDE) -D__SWITCH__
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS     := -ldeko3d -lnx
LIBDIRS  := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
export LD       := $(CXX)
export OFILES   := $(patsubst %.cpp,%.o,$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp))))
export INCLUDE  := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
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
