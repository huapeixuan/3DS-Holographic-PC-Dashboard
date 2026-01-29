# 3DS 开发环境配置指南

## 1. 安装 devkitPro

### macOS 安装步骤

```bash
# 1. 下载安装器
curl -L https://github.com/devkitPro/pacman/releases/latest/download/devkitpro-pacman-installer.pkg -o /tmp/devkitpro.pkg

# 2. 安装 (需要管理员权限)
sudo installer -pkg /tmp/devkitpro.pkg -target /

# 3. 配置环境变量 (添加到 ~/.zshrc)
echo 'export DEVKITPRO=/opt/devkitpro' >> ~/.zshrc
echo 'export DEVKITARM=${DEVKITPRO}/devkitARM' >> ~/.zshrc
echo 'export PATH=${DEVKITPRO}/tools/bin:$PATH' >> ~/.zshrc
source ~/.zshrc

# 4. 安装 3DS 开发工具
sudo dkp-pacman -S 3ds-dev
```

## 2. 验证安装

```bash
# 检查编译器
arm-none-eabi-gcc --version

# 检查 3DS 工具
3dsxtool --help
```

## 3. 编译本项目

```bash
cd 3ds
make
```

## 4. 在 3DS 上运行

1. 将生成的 `.3dsx` 文件复制到 SD 卡的 `/3ds/` 目录
2. 在 3DS 上打开 Homebrew Launcher
3. 选择 "holographic-monitor" 启动

## 常见问题

### Q: 编译报错找不到头文件
确保环境变量正确设置：
```bash
echo $DEVKITPRO  # 应输出 /opt/devkitpro
echo $DEVKITARM  # 应输出 /opt/devkitpro/devkitARM
```

### Q: 如何更新 devkitPro
```bash
sudo dkp-pacman -Syu
```
