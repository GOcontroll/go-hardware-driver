GO_BASE   ?= /home/GOcontroll/GOcontroll-CodeBase
CC        := aarch64-linux-gnu-gcc

OBJ_DIR   := build
BIN       := $(OBJ_DIR)/go-hardware-driver

CPPFLAGS  := -Iinclude \
             -I$(GO_BASE)/code -I$(GO_BASE)/code/modules \
             -I$(GO_BASE)/lib/JSON-C -I$(GO_BASE)/lib/IIO \
             -DGOCONTROLL_LINUX -D_GNU_SOURCE -MMD -MP

CFLAGS_STRICT := -Wall -Wextra -O2 -std=c11
CFLAGS_RELAX  := -Wall -O2 -std=c11

LDFLAGS   := -L$(GO_BASE)/lib/JSON-C -L$(GO_BASE)/lib/IIO
LDLIBS    := -Wl,-Bstatic -ljson-c -Wl,-Bdynamic -liio -lrt -lpthread -lm

OWN_SRCS  := src/main.c src/config.c src/registry.c src/shm.c \
             src/modules/input_6ch.c \
             src/modules/output_6ch.c \
             src/modules/input_10ch.c \
             src/modules/input_4_20ma.c \
             src/modules/output_10ch.c \
             src/modules/bridge_2ch.c

GO_SRCS   := $(GO_BASE)/code/GO_board.c \
             $(GO_BASE)/code/GO_communication_modules.c \
             $(GO_BASE)/code/modules/GO_module_input.c \
             $(GO_BASE)/code/modules/GO_module_input_420ma.c \
             $(GO_BASE)/code/modules/GO_module_output.c \
             $(GO_BASE)/code/modules/GO_module_bridge.c \
             $(GO_BASE)/code/print.c \
             $(GO_BASE)/code/GO_xcp.c \
             $(GO_BASE)/code/XcpStack.c \
             $(GO_BASE)/code/GO_memory.c \
             $(GO_BASE)/code/GO_fault.c \
             $(GO_BASE)/code/GO_controller_info.c

OWN_OBJS  := $(patsubst src/%.c,$(OBJ_DIR)/own/%.o,$(OWN_SRCS))
GO_OBJS   := $(patsubst $(GO_BASE)/%.c,$(OBJ_DIR)/vendor/%.o,$(GO_SRCS))
ALL_OBJS  := $(OWN_OBJS) $(GO_OBJS)
DEPS      := $(ALL_OBJS:.o=.d)

.PHONY: all clean
all: $(BIN)

$(BIN): $(ALL_OBJS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(OBJ_DIR)/own/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS_STRICT) -c $< -o $@

$(OBJ_DIR)/vendor/%.o: $(GO_BASE)/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS_RELAX) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR)

-include $(DEPS)
