KDIR ?= /lib/modules/$(shell uname -r)/build
CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Werror -pthread
.PHONY: all module client clean vm
all: module client
module:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel modules
client:
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Iinclude tools/kqctl.cpp -o build/kqctl
static-client:
	$(MAKE) client CXXFLAGS='$(CXXFLAGS) -static'
vm:
	bash scripts/run-vm.sh
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/kernel clean
	rm -rf build
