#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# GRAPHICS is a list of directories containing graphics files
# GFXBUILD is the directory where converted graphics files will be placed
#   If set to $(BUILD), it will statically link in the converted
#   files as if they were data files.
#
# NO_SMDH: if set to anything, no SMDH file is generated.
# ROMFS is the directory which contains the RomFS, relative to the Makefile (Optional)
# APP_TITLE is the name of the app stored in the SMDH file (Optional)
# APP_DESCRIPTION is the description of the app stored in the SMDH file (Optional)
# APP_AUTHOR is the author of the app stored in the SMDH file (Optional)
# ICON is the filename of the icon (.png), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.png
#     - icon.png
#     - <libctru folder>/default_icon.png
#---------------------------------------------------------------------------------
TARGET		:=	3DSLink
BUILD		:=	build
SOURCES		:=	source ../Sources/azl ../Sources/enet
# The composer of the Download Play child (dlpc/, plain C99, no libctru: DlpLink/docs/COMPOSITORE.md) and the guide "How to get boot9.bin" (guide/, citro2d: it
# draws with source/gfx.hpp, whose folder is on the include path below; its pictures are the .t3s files of guide/gfx, linked in as data). Both folders are
# built whole: the installer child builds the guide from the same folder (DlpLink/child/Makefile).
SOURCES		+=	dlpc guide
# data/child_model.bin is the model of the child, the program of the installer without its RomFS: devkitPro's bin2o links it in as child_model_bin and
# child_model_bin_size. DlpLink/tools/build_all.sh makes it (make_child_model.sh: a seed child of the same installer, no app needed, cut by make_model.py) BEFORE
# the app and puts it here, so after a build the file is the model of that build (check_chain.sh proves the app carries exactly it). A tree that was never built has
# a placeholder that is not a model (dlpc_model_version() says 0 and the page says that this build has no installer), so that the build always has one.
# data/AzaharLink.3gx is the plugin, the other resource: the package is ONE (the app carries its plugin and puts it on the SD card by itself; the Download Play child is composed
# from it). bin2o links it in as AzaharLink_3gx and AzaharLink_3gx_size (plugin_resource.cpp). build_all.sh builds the plugin BEFORE the app and copies it here, and
# check_chain.sh proves that the app's ELF, .3dsx and CIA carry exactly that file (size and SHA-256) and that its header says the version of VERSION.
DATA		:=	data
INCLUDES	:=	../Includes source
GRAPHICS	:=	guide/gfx

# The title delivery module, copied into titleshare/ (the original files plus the patches of titleshare/PATCHES.md, which
# Tests/titleshare_vendor_check.sh proves: titleshare/MANIFEST.md5 is the copy, titleshare/ORIGINALS.md5 and vendor.patch are the way back to the
# original). The copy holds all of the original's common/ and source/. What the module does (the catalog, the check of a title, the sender, the
# receiver, the installer, the log) is built as it is; its own screens (source/main.c, text on a console) are NOT built: 3DS Link draws the Titles
# Delivery screens itself and reaches the module through titleshare_host/delivery.[ch], the place that includes the module's headers for
# the delivery screens. The list is explicit (no wildcard). Their objects are called ts_<name>.o: the module's log.c would otherwise be log.o, like the
# plugin core's log.cpp. They are plain C with their own include paths.
# TS_HOST is ours: delivery (the bridge), hostcfg and logrot (what the app tells the module: folders, key file, log switch; the names and
# the rotation of its log).
TS_COMMON	:=	aes catcache cia_front ciastream contentpath flow ncchname pace proto room runcache sched sdcrypt seed sha256 store win
TS_SOURCE	:=	amsink catalog ctx log power receiver sender
TS_HOST		:=	delivery hostcfg logrot builtin_keys
TS_OFILES	:=	$(addprefix ts_,$(addsuffix .o,$(TS_COMMON) $(TS_SOURCE) $(TS_HOST)))

# The Download Play host of 3DS Link Delivery (dlph/, a copy of DlpLink/host; Tests/dlph_vendor_check.sh proves it against dlph/MANIFEST.md5). Plain C
# with the same flags as the module's files. Their objects are dlph.o and dlph_wire.o: no file of source/ has either name.
DLPH		:=	dlph dlph_wire
DLPH_OFILES	:=	$(addsuffix .o,$(DLPH))
GFXBUILD	:=	$(BUILD)
#ROMFS		:=	romfs
#GFXBUILD	:=	$(ROMFS)/gfx

APP_TITLE		:= 3DS Link
# The version of 3DS Link is ONE number, in the file VERSION of AzaharLink/ (Tests/set_version.py): the home menu's description says it.
APP_VERSION	:= $(shell cat $(TOPDIR)/../VERSION)
APP_DESCRIPTION	:= Connect, share, play together · v$(APP_VERSION)
APP_AUTHOR		:= Powered by Gionko

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-ffunction-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__ -Wno-psabi $(EXTRA_CFLAGS)

# -fdata-sections (C++ only: TitleShare's C files are built as its own project builds them): a static buffer that nothing uses is not
# loaded. What the app loads is taken from the heap that TitleShare plans its windows of blocks with (the loader gives the rest).
CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17 -fdata-sections

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lcitro2d -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(CTRULIB)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES	:=	$(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

#---------------------------------------------------------------------------------
ifeq ($(GFXBUILD),$(BUILD))
#---------------------------------------------------------------------------------
export T3XFILES :=  $(GFXFILES:.t3s=.t3x)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
export ROMFS_T3XFILES	:=	$(patsubst %.t3s, $(GFXBUILD)/%.t3x, $(GFXFILES))
export T3XHFILES		:=	$(patsubst %.t3s, $(BUILD)/%.h, $(GFXFILES))
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES)) \
			$(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o) \
			$(addsuffix .o,$(T3XFILES))

export OFILES := $(OFILES_BIN) $(OFILES_SOURCES) $(TS_OFILES) $(DLPH_OFILES)

export HFILES	:=	$(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h) \
			$(addsuffix .h,$(subst .,_,$(BINFILES))) \
			$(GFXFILES:.t3s=.h)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean

#---------------------------------------------------------------------------------
all: $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

ifneq ($(GFXBUILD),$(BUILD))
$(GFXBUILD):
	@mkdir -p $@
endif

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(GFXBUILD)

#---------------------------------------------------------------------------------
$(GFXBUILD)/%.t3x	$(BUILD)/%.h	:	%.t3s
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@tex3ds -i $< -H $(BUILD)/$*.h -d $(DEPSDIR)/$*.d -o $(GFXBUILD)/$*.t3x

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

# the strings of the SMDH carry the version: a new VERSION makes a new SMDH (without this line make says "up to date" and the old numbers stay in it)
$(OUTPUT).smdh	:	$(TOPDIR)/../VERSION

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

# TitleShare's files: one rule for each of its two folders (they include each other's headers, so both are on the include path).
TS_INCLUDE	:=	-I$(TOPDIR)/titleshare/common -I$(TOPDIR)/titleshare/source -I$(TOPDIR)/titleshare_host

$(addprefix ts_,$(addsuffix .o,$(TS_COMMON))) : ts_%.o : $(TOPDIR)/titleshare/common/%.c
	@echo $(notdir $<)
	$(SILENTCMD)$(CC) -MMD -MP -MF $(DEPSDIR)/ts_$*.d $(CFLAGS) $(TS_INCLUDE) -c $< -o $@ $(ERROR_FILTER)

$(addprefix ts_,$(addsuffix .o,$(TS_SOURCE))) : ts_%.o : $(TOPDIR)/titleshare/source/%.c
	@echo $(notdir $<)
	$(SILENTCMD)$(CC) -MMD -MP -MF $(DEPSDIR)/ts_$*.d $(CFLAGS) $(TS_INCLUDE) -c $< -o $@ $(ERROR_FILTER)

# The same flags as the other module files (the module's own build uses them too).
$(addprefix ts_,$(addsuffix .o,$(TS_HOST))) : ts_%.o : $(TOPDIR)/titleshare_host/%.c
	@echo $(notdir $<)
	$(SILENTCMD)$(CC) -MMD -MP -MF $(DEPSDIR)/ts_$*.d $(CFLAGS) $(TS_INCLUDE) -c $< -o $@ $(ERROR_FILTER)

$(DLPH_OFILES) : %.o : $(TOPDIR)/dlph/%.c
	@echo $(notdir $<)
	$(SILENTCMD)$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(CFLAGS) -c $< -o $@ $(ERROR_FILTER)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
# The plugin (data/AzaharLink.3gx) is data too: its symbol is AzaharLink_3gx
#---------------------------------------------------------------------------------
%.3gx.o	%_3gx.h :	%.3gx
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
# The pictures (GFXBUILD is BUILD: they are linked in as data, so they are made here, from the .t3s files of GRAPHICS, next to the objects)
#---------------------------------------------------------------------------------
%.t3x	%.h	:	%.t3s
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@tex3ds -i $< -H $*.h -d $*.d -o $*.t3x

#---------------------------------------------------------------------------------
.PRECIOUS	:	%.t3x %.shbin
#---------------------------------------------------------------------------------
%.t3x.o	%_t3x.h :	%.t3x
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

#---------------------------------------------------------------------------------
%.shbin.o %_shbin.h : %.shbin
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
