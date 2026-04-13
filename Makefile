# 编译器
CC = gcc

# 编译选项
CFLAGS = -Wall -Wextra -g

# 源文件和目标文件
SRC = ppi_client.c
TARGET = build/ppi_client

# 默认目标
all: $(TARGET)

# 链接
$(TARGET): $(SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $<

# 清理
clean:
	rm -rf build/*

# 运行
run: $(TARGET)
	./$(TARGET)

# 声明伪目标
.PHONY: all clean run