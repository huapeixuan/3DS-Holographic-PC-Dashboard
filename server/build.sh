#!/bin/bash

# 3D 全息仪表盘 - 服务端一键打包脚本
# 此脚本将编译 Rust 服务器和 macOS 硬件监控工具，并输出到 dist 目录

set -e

# 颜色定义
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}🚀 开始打包流程...${NC}"

# 1. 创建输出目录
echo -e "${BLUE}📁 创建输出目录 dist/...${NC}"
mkdir -p dist

# 2. 编译 Rust 服务器
echo -e "${BLUE}🦀 正在编译 Rust 服务器 (Release 模式)...${NC}"
cargo build --release

# 3. 编译 macOS 硬件监控工具 (temp_sensor)
echo -e "${BLUE}🍎 正在编译 macOS 硬件监控工具...${NC}"
# 查找 temp_sensor.m
TEMP_SENSOR_SRC="temp-sensor/temp_sensor.m"
if [ ! -f "$TEMP_SENSOR_SRC" ]; then
    echo "❌ 错误: 未找到 $TEMP_SENSOR_SRC"
    exit 1
fi

# 使用 clang++ 编译，链接必要框架
clang++ -O3 -Wall \
    -framework IOKit \
    -framework CoreFoundation \
    -framework Foundation \
    -lobjc \
    "$TEMP_SENSOR_SRC" -o dist/temp_sensor

# 4. 整理产物
echo -e "${BLUE}📦 正在整理产物...${NC}"
cp target/release/holographic-monitor dist/

# 5. 设置权限
chmod +x dist/holographic-monitor
chmod +x dist/temp_sensor

echo -e "${GREEN}✅ 打包完成！${NC}"
echo -e "产物目录: ${BLUE}$(pwd)/dist${NC}"
echo -e "包含内容:"
ls -lh dist/

echo -e "\n${BLUE}💡 使用提示:${NC}"
echo -e "1. 运行服务器: ./dist/holographic-monitor"
echo -e "2. 注意: 获取温度和控制风扇需要 sudo 权限 (由服务器自动调用，建议初次运行手动测试: sudo ./dist/temp_sensor -j)"
