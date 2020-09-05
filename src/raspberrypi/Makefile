.DEFAULT_GOAL: all

ARCH ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-

CC  = $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++

DEBUG ?= 0
ifeq ($(DEBUG), 1)
	# Debug CFLAGS
	CFLAGS   += -DDISK_LOG -O0 -g -Wall -DDEBUG
	CXXFLAGS   += -DDISK_LOG -O0 -g -Wall -DDEBUG
	BUILD_TYPE = Debug
else
	# Release CFLAGS
	CFLAGS += -O3 -Wall -Werror
	CXXFLAGS += -O3 -Wall -Werror
	BUILD_TYPE = Release
endif
CFLAGS += -iquote . -MD -MP 
CXXFLAGS += -std=c++14 -iquote . -MD -MP 

# If its not specified, build for STANDARD configuration
CONNECT_TYPE ?= STANDARD

ifdef CONNECT_TYPE
CFLAGS   += -DCONNECT_TYPE_$(CONNECT_TYPE)
CXXFLAGS += -DCONNECT_TYPE_$(CONNECT_TYPE)
endif

RASCSI = rascsi
RASCTL = rasctl
RASDUMP = rasdump
SASIDUMP = sasidump
SCSIMON = scsimon

PREFIX ?= /usr/local
INSTALL_BINDIR = $(PREFIX)/bin
INSTALL_MANDIR  = $(PREFIX)/share/man/man1
DOC_DIR = ../../doc

OBJDIR := ./obj/$(shell echo $(CONNECT_TYPE) | tr '[:upper:]' '[:lower:]')
BINDIR := ./bin/$(shell echo $(CONNECT_TYPE) | tr '[:upper:]' '[:lower:]')

#BIN_ALL = $(RASCSI) $(RASCTL) $(RASDUMP) $(SASIDUMP) $(SCSIMON)
# Temporarily remove the RASDUMP and RASDUMP tools, since they're not needed
# for my specific use case. If you need them - add them back in!
BIN_ALL = $(BINDIR)/$(RASCSI) $(BINDIR)/$(RASCTL)


SRC_RASCSI = \
	rascsi.cpp \
	scsi.cpp \
	gpiobus.cpp \
	filepath.cpp \
	fileio.cpp\
#	os.cpp
#	rasctl_command.cpp
#	rascsi_mgr.cpp
#	command_thread.cpp
SRC_RASCSI += $(shell find ./controllers -name '*.cpp')
SRC_RASCSI += $(shell find ./devices -name '*.cpp')

SRC_RASCTL = \
	rasctl.cpp
#	rasctl_command.cpp

SRC_RASDUMP = \
	rasdump.cpp \
	scsi.cpp \
	gpiobus.cpp \
	filepath.cpp \
	fileio.cpp

SRC_SASIDUMP = \
	sasidump.cpp \
	scsi.cpp \
	gpiobus.cpp \
	filepath.cpp \
	fileio.cpp

vpath %.h ./ ./controllers ./devices
vpath %.cpp ./ ./controllers ./devices
vpath %.o ./$(OBJDIR)
vpath ./$(BINDIR)


OBJ_RASCSI := $(addprefix $(OBJDIR)/,$(notdir $(SRC_RASCSI:%.cpp=%.o)))
OBJ_RASCTL := $(addprefix $(OBJDIR)/,$(notdir $(SRC_RASCTL:%.cpp=%.o)))
OBJ_RASDUMP := $(addprefix $(OBJDIR)/,$(notdir $(SRC_RASDUMP:%.cpp=%.o)))
OBJ_SASIDUMP := $(addprefix $(OBJDIR)/,$(notdir $(SRC_SASIDUMP:%.cpp=%.o)))
OBJ_SCSIMON  := $(addprefix $(OBJDIR)/,$(notdir $(SRC_SCSIMON:%.cpp=%.o)))
#OBJ_ALL := $(OBJ_RASCSI) $(OBJ_RASCTL) $(OBJ_RASDUMP) $(OBJ_SASIDUMP) $(OBJ_SCSIMON)
OBJ_ALL := $(OBJ_RASCSI) $(OBJ_RASCTL) $(OBJ_RASDUMP) $(OBJ_SASIDUMP)


# The following will include all of the auto-generated dependency files (*.d)
# if they exist. This will trigger a rebuild of a source file if a header changes
ALL_DEPS := $(patsubst %.o,%.d,$(OBJ_RASCSI) $(OBJ_RASCTL))
-include $(ALL_DEPS)

$(OBJDIR) $(BINDIR):
	echo Creating directory $@
	mkdir -p $@

$(OBJDIR)/%.o: %.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

.DEFAULT_GOAL := all
.PHONY: all ALL docs
all: $(BIN_ALL) docs
ALL: all

docs: $(DOC_DIR)/rascsi_man_page.txt $(DOC_DIR)/rasctl_man_page.txt

$(BINDIR)/$(RASCSI): $(OBJ_RASCSI) | $(BINDIR)
	$(CXX) -o $@ $(OBJ_RASCSI) -lpthread

$(BINDIR)/$(RASCTL): $(OBJ_RASCTL) $(BINDIR)
	$(CXX) -o $@ $(OBJ_RASCTL)

$(RASDUMP): $(OBJ_RASDUMP) $(BINDIR)
	$(CXX) -o $@ $(OBJ_RASDUMP)

$(SASIDUMP): $(OBJ_SASIDUMP) $(BINDIR)
	$(CXX) -o $@ $(OBJ_SASIDUMP)

$(SCSIMON): $(OBJ_SCSIMON) $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ_SCSIMON) -lpthread

clean:
	rm -rf $(OBJDIR) $(BINDIR)

run:
	sudo ./$(RASCSI) -ID1 /home/pi/HARDDISK2.hda -ID6 /home/pi/marathon.iso

install: $(INSTALL_MANDIR)/rascsi.1 $(INSTALL_MANDIR)/rasctl.1
	sudo cp $(BINDIR)/$(RASCTL) $(DESTDIR)$(INSTALL_BINDIR)
	sudo cp $(BINDIR)/$(RASCSI) $(DESTDIR)$(INSTALL_BINDIR)

$(INSTALL_MANDIR)/%.1 : $(DOC_DIR)/%.1
	sudo cp $< $@

$(DOC_DIR)/%_man_page.txt : $(DOC_DIR)/%.1
	@echo "!!   ------ THIS FILE IS AUTO_GENERATED! DO NOT MANUALLY UPDATE!!!" > $@
	@echo "!!   ------ The native file is $(notdir $<). Re-run 'make docs' after updating\n\n" >> $@
	man -l $< | col -bx >> $@

.PHONY: Debug
Debug: all
