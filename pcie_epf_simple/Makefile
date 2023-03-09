ifneq ($(KERNELRELEASE),)
# If this is defined, the makefile has been invoked from the kernel build
obj-m  := pcie_epf_simple.o

else
# The makefile has been called from the command line
KDIR=/home/reds/build_test_patches/cache/sources/linux-mainline/linux-6.0.y
ARCH=arm64
CROSS_COMPILE=/home/reds/build_test_patches/cache/toolchain/gcc-arm-8.3-2019.03-x86_64-aarch64-linux-gnu/bin/aarch64-linux-gnu-

all:
	$(MAKE) -C $(KDIR) M=$$PWD ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	rm -f *.o *.ko *.mod.c *.mod Module.symvers modules.order

.PHONY : all clean

endif