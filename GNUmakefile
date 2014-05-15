DEPS := .deps
DEPCFLAGS = -MD -MF $(DEPS)/$*.d
OBJDIR = ./obj
SRCDIR = ./src
CXXFLAGS = -fPIC -Wno-pmf-conversions -g -std=gnu++0x -I./src \
           -I. -fno-omit-frame-pointer -O2 -I./fastrpc/src

LIBS = -lboost_program_options -lev -lprotobuf -ldl -lpthread -libverbs

all: $(OBJDIR)/server \
     $(OBJDIR)/client \
     $(OBJDIR)/ibclient \
     $(OBJDIR)/ibserver \
     $(OBJDIR)/ib_bw_server \
     $(OBJDIR)/ib_bw_client \
     $(OBJDIR)/sync_server \
     $(OBJDIR)/sync_client

COMMON_OBJS := $(wildcard $(SRCDIR)/common/*.cc)
COMMON_OBJS := $(subst .cc,.o,$(notdir $(OBJS)))
COMMON_OBJS := $(addprefix $(OBJDIR)/,$(OBJS))

# Define all objects that depends on fastrpc
DEPOBJS := $(wildcard $(SRCDIR)/app/*.cc)
DEPOBJS := $(subst .cc,.o,$(notdir $(DEPOBJS)))
DEPOBJS := $(addprefix $(OBJDIR)/,$(DEPOBJS)) $(COMMON_OBJS)
# The name of the Protocol Buffer file in the ./proto directory.
# Define before the subsequent "include" statement
PROTOFILE := bench.proto
# include the definition of FASTRPC and the rule to build FASTRPC
include fastrpc/Makefile.include

ib%: ib%.o
	g++ $^ -L$(OBJDIR) -Wl,-R $(OBJDIR) -libverbs -lev $(FASTRPC) -o $@

%: %.o $(OBJS)
	g++ $^ -L$(OBJDIR) -Wl,-R $(OBJDIR) $(LIBS) $(FASTRPC) -o $@

$(OBJDIR)/%.o: $(SRCDIR)/common/%.cc
	mkdir -p $(DEPS) $(OBJDIR)
	g++ $(CXXFLAGS) -c $(DEPCFLAGS) $< -o $@

$(OBJDIR)/%.o: $(SRCDIR)/app/%.cc
	mkdir -p $(DEPS) $(OBJDIR)
	g++ $(CXXFLAGS) -c $(DEPCFLAGS) $< -o $@

DEPFILES := $(wildcard $(DEPS)/*.d)
ifneq ($(DEPFILES),)
-include $(DEPFILES)
endif

.PRECIOUS: $(OBJDIR)/*.o

clean:
	rm $(DEPS) $(OBJDIR) -rf
