# 编译与运行
#   make               编译到 build/app.exe
#   make run           编译并启动 Web UI（默认 8080 端口）
#   make run PORT=8099 指定端口
#   make clean         删除 build/
#
# 依赖：MinGW/MSYS2 的 g++，Windows 下链接 ws2_32（WebUI 的本地 HTTP 服务）。

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Isrc
LDLIBS   ?= -lws2_32

# 有些精简环境里 make 的子进程拿不到 TMP/TEMP，MinGW g++ 会退回 C:\WINDOWS
# 创建临时文件而报 "Permission denied"，这里补一个项目内的临时目录
ifeq ($(strip $(TMP)),)
TMP := $(CURDIR)/build/tmp
NEED_TMP_DIR := build/tmp
endif
ifeq ($(strip $(TEMP)),)
TEMP := $(CURDIR)/build/tmp
NEED_TMP_DIR := build/tmp
endif
export TMP TEMP

TARGET := build/app.exe
SRCS   := src/main.cpp src/service/AuthService.cpp src/service/ProductService.cpp src/service/TradeService.cpp src/service/ReturnService.cpp src/service/SearchService.cpp src/service/MessageService.cpp src/service/TimeoutSweeper.cpp src/ui/WebUI.cpp
OBJS   := $(SRCS:src/%.cpp=build/obj/%.o)
DEPS   := $(OBJS:.o=.d)
PORT   ?= 8080

.PHONY: all run clean rebuild

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(OBJS) -o $@ $(LDLIBS)

# -MMD -MP 自动生成头文件依赖，改 .h 也会触发重编译
build/obj/%.o: src/%.cpp
	@mkdir -p $(dir $@) $(NEED_TMP_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

run: $(TARGET)
	./$(TARGET) $(PORT)

clean:
	rm -rf build

rebuild: clean all

-include $(DEPS)
