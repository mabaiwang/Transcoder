# Encode Studio

基于 C++ 和 Qt Widgets 的单窗口转码工具。

## 已实现功能

- URL 编码 / 解码
- HTML 实体编码 / 解码
- SQL_EN 编码 / 解码（`0x + UTF-16LE Hex`，与示例图一致）
- Hex 编码 / 解码
- Binary 编码 / 解码
- Asc（UTF-8 字节十进制）编码 / 解码
- Unicode Escape 编码 / 解码
- Base64 编码 / 解码
- MD5 32 位
- MD5 16 位
- SHA1
- SHA256

## 交互特点

- 所有编辑框默认一行高度，可通过“编辑框行数”统一调节
- 顶部输入一次，所有结果实时刷新
- 每张可逆卡片都支持单独粘贴内容后点击“还原”
- 每张卡片都支持一键复制
- 暗黑 / 浅色一键切换（默认浅色）
- 所有功能集中在一个窗口内

## 构建方式

### Qt Creator

直接打开 `D:\encode_change\CMakeLists.txt`，并确保 **编译器与 Qt 套件匹配**：

- `MSVC` 编译器配 `Qt msvcxxxx_64`
- `MinGW` 编译器配 `Qt mingw_64`

### CMake（MSVC + Qt msvc 套件）

```powershell
cmake -S D:\encode_change -B D:\encode_change\build_msvc -G "Visual Studio 16 2019" -A x64 -D CMAKE_PREFIX_PATH=C:\Qt\6.10.2\msvc2019_64
cmake --build D:\encode_change\build_msvc --config Release
```

### CMake（MinGW + Qt mingw_64 套件）

```powershell
cmake -S D:\encode_change -B D:\encode_change\build_mingw -G "MinGW Makefiles" -D CMAKE_C_COMPILER=C:\Qt\Tools\mingw1310_64\bin\gcc.exe -D CMAKE_CXX_COMPILER=C:\Qt\Tools\mingw1310_64\bin\g++.exe -D CMAKE_PREFIX_PATH=C:\Qt\6.10.2\mingw_64
cmake --build D:\encode_change\build_mingw --config Release
```

生成的可执行文件名称为 `EncodeStudio`。

## 绿色便携版（推荐 MinGW 套件）

以下命令会生成一个可直接拷走运行的目录，不依赖安装器：

```powershell
cmake -S D:\encode_change -B D:\encode_change\build_mingw -G "MinGW Makefiles" -D CMAKE_C_COMPILER=C:\Qt\Tools\mingw1310_64\bin\gcc.exe -D CMAKE_CXX_COMPILER=C:\Qt\Tools\mingw1310_64\bin\g++.exe -D CMAKE_PREFIX_PATH=C:\Qt\6.10.2\mingw_64
cmake --build D:\encode_change\build_mingw --config Release

Remove-Item -Recurse -Force D:\encode_change\portable -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path D:\encode_change\portable | Out-Null
Copy-Item D:\encode_change\build_mingw\EncodeStudio.exe D:\encode_change\portable\
Copy-Item D:\encode_change\assets\icon.ico D:\encode_change\portable\

C:\Qt\6.10.2\mingw_64\bin\windeployqt.exe --release --compiler-runtime D:\encode_change\portable\EncodeStudio.exe
```

便携目录为 `D:\encode_change\portable`，整目录打包即可分发。

## 说明

当前工程使用纯代码构建界面，没有依赖 `.ui` 文件，方便后续继续扩展更多编码格式或换肤样式。

如果你看到 `Detected MinGW Qt with MSVC generator`，说明是 `Visual Studio 生成器 + mingw_64 Qt` 混用，请改用上面任一正确组合。
