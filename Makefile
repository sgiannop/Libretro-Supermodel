# --- Global Variables ---
DEBUG ?= 0
PSS_STYLE ?= 1
EXTERNAL_ZLIB ?= 0
HAVE_GRIFFIN ?= 1
STATIC_LINKING ?= 0
CORE_DIR := .
TARGET_NAME := supermodel

# --- Platform Detection ---
ifeq ($(platform),)
    platform = unix
    ifeq ($(shell uname -a),)
        platform = win
    else ifneq ($(findstring MINGW,$(shell uname -a)),)
        platform = win
    else ifneq ($(findstring Darwin,$(shell uname -a)),)
        platform = osx
    else ifneq ($(findstring win,$(shell uname -a)),)
        platform = win
    endif
endif

# --- Include Common Sources FIRST ---
# This populates SOURCES_C, SOURCES_CXX and base INCFLAGS
include Makefile.common

# --- Include Platform Specifics SECOND ---
# This allows Windows/Unix/OSX files to APPEND to INCFLAGS and set CC/CXX/OBJOUT
ifneq (,$(findstring msvc,$(platform)))
    include Makefile.windows
else ifneq (,$(findstring win,$(platform)))
    include Makefile.windows
else ifneq (,$(findstring osx,$(platform)))
    include Makefile.osx
else
    include Makefile.unix
endif

# --- Safety Defaults (In case platform files miss them) ---
OBJOUT  ?= -o
LINKOUT ?= -o

# --- Flag Assembly ---
GIT_VERSION := " $(shell git rev-parse --short HEAD || echo unknown)"
ifneq ($(GIT_VERSION)," unknown")
    DEFINES += -DGIT_VERSION=\"$(GIT_VERSION)\"
endif

OBJECTS := $(SOURCES_C:.c=.o) $(SOURCES_CXX:.cpp=.o)

# Combine defines from common file and platform settings
# Use += to ensure we don't wipe out GLEW_STATIC or other Windows flags
DEFINES += $(COREDEFINES) $(PLATFORM_DEFINES) -DSUPERMODEL_OSD_LIBRETRO

ifeq ($(STATIC_LINKING),1)
    DEFINES += -DSTATIC_LINKING
endif

ifeq ($(platform), sncps3)
    WARNING_DEFINES =
else ifneq (,$(findstring msvc,$(platform)))
    WARNING_DEFINES =
    LIBM :=
else
    WARNING_DEFINES = -Wno-write-strings
endif

CFLAGS   += $(fpic) $(WARNING_DEFINES) $(DEFINES)
CXXFLAGS += $(fpic) $(WARNING_DEFINES) $(DEFINES)
LDFLAGS  += $(LIBM)

# --- Build Rules ---
all: $(TARGET)

$(TARGET): $(OBJECTS)
ifeq ($(STATIC_LINKING),1)
ifneq (,$(findstring msvc,$(platform)))
	$(LD) $(LINKOUT)$@ $(OBJECTS)
else
	$(AR) rcs $@ $(OBJECTS)
endif
else
	$(LD) $(LINKOUT)$@ $(SHARED) $(OBJECTS) $(LDFLAGS) $(LIBS)
endif

# THE FIX: Note the absence of space between $(OBJOUT) and $@
%.o: %.cpp
	$(CXX) -c $(OBJOUT)$@ $< $(CXXFLAGS) $(INCFLAGS)

%.o: %.c
	$(CC) -c $(OBJOUT)$@ $< $(CFLAGS) $(INCFLAGS)

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: clean