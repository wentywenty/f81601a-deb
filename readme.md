# f81601a-deb 构建说明

该目录用于将 `f81601a` 内核驱动编译并打包为 Debian 包，流程参考 `igh-deb`。

## 1. 配置环境
编辑 `env.sh`，确认以下变量：

- `KERNEL_COMPILER`: 交叉编译器前缀（例如 `aarch64-none-linux-gnu-`）
- `KERNEL_SRC`: 目标内核源码目录（必须已配置/可编译）
- `ARCH`: 目标架构（默认 `arm64`）
- `PACKAGE_NAME` / `DEB_VERSION`: 包名和版本

## 2. 执行构建

```bash
cd f81601a-deb
chmod +x build.sh env.sh debian/DEBIAN/postinst debian/DEBIAN/postrm debian/DEBIAN/preinst debian/DEBIAN/prerm
./build.sh
```

也可以用参数临时覆盖：

```bash
./build.sh <KERNEL_COMPILER> <KERNEL_SRC> <ARCH>
```

## 3. 输出结果

构建成功后会在 `output/` 目录生成：

- `*.deb`：可安装 Debian 包
- `_modules/`：临时模块安装目录
- `package/`：打包 staging 目录

## 4. 安装

在目标设备上执行：

```bash
sudo dpkg -i output/<your_deb_name>.deb
```

安装脚本会自动执行 `depmod`。
