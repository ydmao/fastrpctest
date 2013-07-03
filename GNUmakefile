DEPS := .deps
DEPCFLAGS = -MD -MF $(DEPS)/$*.d
OBJDIR = ./obj
SRCDIR = ./src
PROTOGENDIR = ./proto
CXXFLAGS = -fPIC -Wno-pmf-conversions -g -std=gnu++0x -I./src \
           -I$(PROTOGENDIR) -I. -fno-omit-frame-pointer -O2 -I./fastrpc/src

PROTOSRCDIR = ./proto
FASTRPC = ./fastrpc/obj/libfastrpc.so

LIBS = -lboost_program_options-mt -lboost_thread-mt \
	-lboost_system-mt -lboost_serialization-mt \
	-lev -lprotobuf -ldl -liberty -ltcmalloc

all: $(PROTOGENDIR)/bench.pb.h \
     $(PROTOGENDIR)/bench.pb.cc \
     $(OBJDIR)/server \
     $(OBJDIR)/client

COMMON_OBJS := $(wildcard $(SRCDIR)/common/*.cc)
COMMON_OBJS := $(subst .cc,.o,$(notdir $(COMMON_OBJS))) 
COMMON_OBJS := $(addprefix $(OBJDIR)/,$(COMMON_OBJS)) $(PROTOGENDIR)/bench.pb.o

$(OBJDIR)/server: $(OBJDIR)/server.o $(COMMON_OBJS) $(FASTRPC)
	g++ $^ -L$(OBJDIR) -Wl,-R $(OBJDIR) $(LIBS) -o $@

$(OBJDIR)/client: $(OBJDIR)/client.o $(COMMON_OBJS) $(FASTRPC)
	g++ $^ -L$(OBJDIR) -Wl,-R $(OBJDIR) $(LIBS) -o $@

$(FASTRPC): fastrpc-update

fastrpc-update:
	if ! test -L ./fastrpc/src/proto; then (ln -s $(PWD)/proto fastrpc/src/proto); fi
	cd fastrpc && make

$(OBJDIR)/%.o: $(SRCDIR)/common/%.cc
	mkdir -p $(DEPS) $(OBJDIR)
	g++ $(CXXFLAGS) -c $(DEPCFLAGS) $< -o $@

$(OBJDIR)/%.o: $(SRCDIR)/app/%.cc
	mkdir -p $(DEPS) $(OBJDIR)
	g++ $(CXXFLAGS) -c $(DEPCFLAGS) $< -o $@

$(PROTOGENDIR)/bench.pb.cc $(PROTOGENDIR)/bench.pb.h: $(PROTOSRCDIR)/bench.proto
	mkdir -p $(DEPS) $(OBJDIR)
	protoc --cpp_out=$(PROTOGENDIR) -I$(PROTOSRCDIR) $<

DEPFILES := $(wildcard $(DEPS)/*.d)
ifneq ($(DEPFILES),)
-include $(DEPFILES)
endif

.PRECIOUS: $(OBJDIR)/*.o

clean:
	rm $(DEPS) $(OBJDIR) -rf
	cd fastrpc && make clean

