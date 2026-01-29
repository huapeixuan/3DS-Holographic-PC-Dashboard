#---------------------------------------------------------------------------------
# 3DS Holographic Monitor - Makefile
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
#---------------------------------------------------------------------------------
# 目标设置
#---------------------------------------------------------------------------------
TARGET		:=	holographic-monitor
BUILD		:=	build
SOURCES		:=	source
INCLUDES	:=	include
GRAPHICS	:=	gfx
ROMFS		:=	romfs

# Force RomFS flag if standard rules miss it
# Use deferred expansion (=) so OUTPUT is resolved later
_3DSXFLAGS = --smdh=$(OUTPUT).smdh --romfs=$(TOPDIR)/$(ROMFS)

include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# 代码生成选项
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-ffunction-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lcitro2d -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
# 库相关路径
#---------------------------------------------------------------------------------
LIBDIRS	:= $(CTRULIB)

#---------------------------------------------------------------------------------
# 自动文件收集
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(BUILD)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.pica)))

export LD	:=	$(CXX)

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o) $(PICAFILES:.pica=.shbin.o)

export OFILES	:=	$(OFILES_SOURCES)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf *.map

#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# 主构建规则
#---------------------------------------------------------------------------------
.PHONY: all

all:	$(OUTPUT).3dsx

$(OUTPUT).3dsx:	$(OUTPUT).elf $(OUTPUT).smdh

$(OUTPUT).elf:	$(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
# PICA Compiler Rules
#---------------------------------------------------------------------------------
%.shbin.o : %.shbin
	@echo $(notdir $<)
	@bin2s -a 32 $< | $(AS) -o $@

%.shbin : %.pica
	@echo $(notdir $<)
	@picasso -o $@ $<

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
