obj-m += syscall_trace.o

PWD := $(CURDIR)

all: module build-generators

module:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

build-generators:
	$(MAKE) -C generators

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	$(MAKE) -C generators clean