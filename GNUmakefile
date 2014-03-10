DEPS := .deps
DEPCFLAGS = -MD -MF $(DEPS)/$*.d
OBJDIR = ./obj
SRCDIR = ./src
CXXFLAGS = -fPIC -Wno-pmf-conversions -g -std=gnu++0x -I./src \
           -I. -fno-omit-frame-pointer -O2 -I./fastrpc/src

LIBS = -lboost_program_options -lev -lprotobuf -ldl -lpthread -libverbs

FASTRPC = ./fastrpc/obj/libfastrpc.so

# Make sure that FASTRPC is the first target!
all: $(FASTRPC) \
     $(OBJDIR)/server \
     $(OBJDIR)/client \
     $(OBJDIR)/ibclient \
     $(OBJDIR)/ibserver \
     $(OBJDIR)/ib_bw_server \
     $(OBJDIR)/ib_bw_client \
     $(OBJDIR)/sync_server \
     $(OBJDIR)/sync_client

$(FASTRPC): fastrpc-update

fastrpc-update:
	if ! test -L ./fastrpc/src/proto; then (ln -s $(PWD)/proto fastrpc/src/proto); fi
	cd fastrpc && PROTO=bench.proto make

COMMON_OBJS := $(wildcard $(SRCDIR)/common/*.cc)
COMMON_OBJS := $(subst .cc,.o,$(notdir $(COMMON_OBJS))) 
COMMON_OBJS := $(addprefix $(OBJDIR)/,$(COMMON_OBJS))

ib%: ib%.o $(FASTRPC)
	g++ $^ -L$(OBJDIR) -Wl,-R $(OBJDIR) -libverbs -lev -o $@

%: %.o $(COMMON_OBJS) $(FASTRPC)
	g++ $^ -L$(OBJDIR) -Wl,-R $(OBJDIR) $(LIBS) -o $@

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
	cd fastrpc && make clean

