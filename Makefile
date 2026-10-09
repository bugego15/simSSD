# ---------------------------------------------------------------------
# SSD firmware simulator
# 目标：
#   make        构建模拟器
#   make asan   带 ASan/UBSan 构建（调试与回归测试用）
#   make test   运行单元测试
#   make clean  清理
# ---------------------------------------------------------------------

# make 内置 CC 默认为 cc，用 ?= 无法覆盖，这里显式探测 origin
ifeq ($(origin CC),default)
CC := gcc
endif

STD     := -std=c99
WARN    := -Wall -Wextra -Werror
OPT     ?= -O2

CFLAGS  := $(STD) $(WARN) $(OPT) -g
CFLAGS  += -MMD -MP
INC     := -Iinclude
LDLIBS  := -lm

BUILD   := build

SRC_CORE := \
	src/core/assert.c \
	src/core/bitmap.c \
	src/core/clock.c \
	src/core/config.c \
	src/core/eventq.c \
	src/core/log.c \
	src/core/mempool.c \
	src/core/rng.c \
	src/core/stats.c

SRC_MEDIA := \
	src/media/geometry.c \
	src/media/nand.c

SRC_FTL := \
	src/ftl/ftl.c \
	src/ftl/gc.c \
	src/ftl/wl.c

SRC_SIM  := src/main.c
SRC_TEST := tests/test_main.c tests/test_smoke.c tests/test_media.c \
	tests/test_ftl.c tests/test_ftl_s3.c

OBJ_LIB  := $(SRC_CORE:%.c=$(BUILD)/%.o) $(SRC_MEDIA:%.c=$(BUILD)/%.o) $(SRC_FTL:%.c=$(BUILD)/%.o)
OBJ_SIM  := $(SRC_SIM:%.c=$(BUILD)/%.o)
OBJ_TEST := $(SRC_TEST:%.c=$(BUILD)/%.o)

TARGET  := $(BUILD)/ssd-sim
TESTBIN := $(BUILD)/ssd-test

.PHONY: all asan test clean

all: $(TARGET)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

$(TARGET): $(OBJ_LIB) $(OBJ_SIM)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(TESTBIN): $(OBJ_LIB) $(OBJ_TEST)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

asan: CFLAGS := -std=c99 -Wall -Wextra -Werror -g -O1 \
                -fsanitize=address,undefined -fno-omit-frame-pointer
asan: LDLIBS := -lm
asan: clean $(TARGET)

test: $(TESTBIN)
	./$(TESTBIN)

clean:
	rm -rf $(BUILD)

-include $(OBJ_LIB:.o=.d) $(OBJ_SIM:.o=.d) $(OBJ_TEST:.o=.d)
