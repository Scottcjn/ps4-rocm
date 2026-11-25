ROCM    ?= $(HOME)/rocm-gfx7
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11 -ffp-contract=off
CPPFLAGS = -I. -I$(ROCM)/include -I/usr/include/libdrm
LDLIBS   = libpm4bridge.a $(ROCM)/lib/libhsakmt.a -ldrm_amdgpu -ldrm -lnuma -lpthread -lrt -lm

# Device code: gfx701 (hawaii), no FLAT addressing, no 128-bit LDS ops.
CLC      = clang -x cl -cl-std=CL1.2 -target amdgcn-amd-amdhsa -mcpu=hawaii -nogpulib -O2 \
           -mcode-object-version=5 \
           -Xclang -target-feature -Xclang -flat-for-global \
           -Xclang -target-feature -Xclang -enable-ds128

TESTS   = t_noop t_errors t_fill t_vadd t_copybw t_matvec t_stress t_wide t_vrambw
KERNELS = noop fill vadd copybw matvec stress wide
BINS    = $(addprefix tests/,$(TESTS))
COS     = $(patsubst %,kernels/%.co,$(KERNELS))

all: libpm4bridge.a $(COS) kernels/bad/scratch.co $(BINS)

libpm4bridge.a: pm4bridge.o
	$(AR) rcs $@ $^

pm4bridge.o: pm4bridge.c pm4bridge.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

tests/%: tests/%.c tests/common.h pm4bridge.h libpm4bridge.a
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDLIBS)

# .s is only for the audit (llvm-objdump cannot disassemble gfx7)
kernels/%.co: kernels/%.cl kernels/audit.sh
	$(CLC) -S -o kernels/$*.s $<
	kernels/audit.sh kernels/$*.s
	$(CLC) -o $@ $<

# Negative test input: needs scratch, so it must NOT pass the audit and the
# loader must refuse it. Never dispatched.
kernels/bad/scratch.co: kernels/bad/scratch.cl
	$(CLC) -o $@ $<

# Every GPU test runs under `timeout` via run.sh; fill runs twice with
# different values. Logs go to logs/.
check: all
	rm -f logs/*.log
	./run.sh t_noop 30
	./run.sh t_errors 30
	./run.sh t_fill 60 0x11110000 0x77770000
	./run.sh t_fill 60 0xABCD0000 0x00420000
	./run.sh t_vadd 120
	./run.sh t_stress 120
	PM4_GPU_TS=1 PM4_ENV=PM4_GPU_TS=1 ./run.sh t_copybw 300
	PM4_GPU_TS=1 PM4_ENV=PM4_GPU_TS=1 ./run.sh t_matvec 120 64
	./run.sh t_wide 120 256 64
	@echo; echo "== summary"; grep -h "^RESULT" logs/*.log

# Vulkan version of the bandwidth test (needs glslc, libvulkan, patched Mesa to run)
vk: vk/vkbw vk/bw_copy.spv vk/bw_sum.spv vk/bw_fill.spv
vk/vkbw: vk/vkbw.c
	$(CC) -O2 -Wall -o $@ $< -lvulkan
vk/%.spv: vk/%.comp
	glslc -O $< -o $@

clean:
	rm -f pm4bridge.o libpm4bridge.a $(BINS) kernels/*.co kernels/*.s kernels/bad/*.co

.PHONY: all clean check vk
