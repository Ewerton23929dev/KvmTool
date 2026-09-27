SOURCE_DIR := src
INCLUDE_DIR := include

CFLAG := -O1 -I$(INCLUDE_DIR) -Wall -Wextra -pthread
LDFLAGS := -pthread

OBJS := \
 $(SOURCE_DIR)/vcpu/kvm_vcpu.c \
 $(SOURCE_DIR)/vcpu/kvm_thread.c \
 $(SOURCE_DIR)/kvm_machine.c \
 $(SOURCE_DIR)/kvm_device.c \
 $(SOURCE_DIR)/kvm_capability.c \
 $(SOURCE_DIR)/x86/kvmx86_modes.c \
 $(SOURCE_DIR)/x86/kvmx86_registres.c \
 $(SOURCE_DIR)/main.c


all:
	gcc -g $(CFLAG) $(OBJS) -o main $(LDFLAGS)
clean:
	rm -rf main