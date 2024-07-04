################################################################
# Copyright (c) 2024, Intel Corp All rights reserved.
# ATUL RAUT atul.raut@intel.com
# GPUDirect RDMA
################################################################

IDIR = .
CC = gcc
ODIR = obj

# Location of the CUDA Toolkit
CUDA_PATH ?= /usr/local/cuda

CUDAFLAGS = -I/usr/local/cuda-12.5/targets/x86_64-linux/include
CUDAFLAGS += -I/usr/local/cuda/include
PRE_CFLAGS1 = -I$(IDIR) $(CUDAFLAGS) -g -DHAVE_CUDA
LIBS = -Wall -lrdmacm -libverbs -lmlx5 -lcuda
NVCC          := $(CUDA_PATH)/bin/nvcc

my_path := /opt/rdma-core/build
CFLAGS  := -Wall -Werror -g -O0 -I${my_path}/include/

#Uncomment next line to enable debug prints
CFLAGS  += -DDEBUG_ENABLE
LD      := gcc
LDLIBS  := ${LDLIBS} -L${my_path}/lib/ -Wl,-rpath=${my_path}/lib/ -lrdmacm -libverbs -lpthread -lz


ifeq ($(PRINT_LAT),1)
  CFLAGS = $(PRE_CFLAGS1) -DPRINT_LATENCY
else
  CFLAGS = $(PRE_CFLAGS1)
endif

OEXE_CLT = client
OEXE_SRV = server

DEPS = common.h
DEPS += hugepage.h

OBJS = common.o
OBJS += client.o
OBJS += hugepage.o
OBJS += gpu_mem_util.o

$(ODIR)/%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

all : make_odir $(OEXE_CLT) $(OEXE_SRV)

make_odir: $(ODIR)/

$(OEXE_CLT) : $(patsubst %,$(ODIR)/%,$(OBJS)) $(ODIR)/common.o
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS) ${LDLIBS}

$(ODIR)/:
	mkdir -p $@

.PHONY: clean

clean :
	rm -f $(OEXE_CLT) $(OEXE_SRV) $(ODIR)/*.o *~ core.* $(IDIR)/*~
