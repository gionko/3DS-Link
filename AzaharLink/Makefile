.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR 		?= 	$(CURDIR)
include $(DEVKITARM)/3ds_rules

CTRPFLIB	?=	$(DEVKITPRO)/libctrpf

TARGET		:= 	AzaharLink
PLGINFO 	:= 	CTRPluginFramework.plgInfo

BUILD		:= 	Build
INCLUDES	:= 	Includes
SOURCES 	:= 	Sources Sources/azl Sources/ctr Sources/enet

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH		:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS		:=	$(ARCH) -Os -mword-relocations \
				-fomit-frame-pointer -ffunction-sections -fno-strict-aliasing

CFLAGS		+=	$(INCLUDE) -D__3DS__ -DAZL_USE_CTRPF -Wall -Wno-psabi

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS		:=	$(ARCH)
# --wrap=srvGetServiceHandle lets ctr/soc.cpp hand libctru's socInit() a soc:U session even when the
# game's exheader does not allow it (see soc.cpp). --wrap=free lets ctr/teardown.cpp stop
# CTRPluginFramework's exit sequence from freeing memory that worker threads still use.
# --wrap=...GetTextSize (Process::GetTextSize()) lets ctr/text_search_guard.cpp keep the framework's
# startup search for the GSP call inside the game's code (Kirby Triple Deluxe / Planet Robobot).
LDFLAGS		:= -T $(TOPDIR)/3gx.ld $(ARCH) -Os -Wl,--gc-sections,--strip-discarded,--strip-debug \
				-Wl,--wrap=srvGetServiceHandle -Wl,--wrap=free \
				-Wl,--wrap=_ZN18CTRPluginFramework7Process11GetTextSizeEv

LIBS		:= -lctrpf -lctru
LIBDIRS		:= 	$(CTRPFLIB) $(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES			:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES			:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD 		:= 	$(CXX)
export OFILES	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I $(CURDIR)/$(dir) ) \
					$(foreach dir,$(LIBDIRS),-I $(dir)/include) \
					-I $(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L $(dir)/lib)

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ... 
	@rm -fr $(BUILD) $(OUTPUT).3gx $(OUTPUT).elf

re: clean all

#---------------------------------------------------------------------------------

else

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
# The ELF must be relinked whenever an object, the linker script or this Makefile changes, and the
# 3gx repacked when the ELF or the plugin info changes: without these lines make kept an old .elf
# and shipped a stale plugin (it cost two hardware test rounds).
$(OUTPUT).3gx : $(OUTPUT).elf $(TOPDIR)/$(PLGINFO)
$(OUTPUT).elf : $(OFILES) $(TOPDIR)/3gx.ld $(TOPDIR)/Makefile

# The build stamp of the log header (Sources/ctr/build_id.cpp) is made again whenever any other object was: the stamp says when the code
# last changed. (The first C-stick build had a new main.cpp over a plugin.cpp of two days before, and its log said "1.0.3 (built Oct 23)".)
build_id.o : $(filter-out build_id.o,$(OFILES))

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	:	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
.PRECIOUS: %.elf
%.3gx: %.elf
#---------------------------------------------------------------------------------
	@echo creating $(notdir $@)
	@3gxtool -s $(word 1, $^) $(TOPDIR)/$(PLGINFO) $@

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
