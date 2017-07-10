obj-m += oscache.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	rm *.order *.o *.ko *.symvers *.mod.c
