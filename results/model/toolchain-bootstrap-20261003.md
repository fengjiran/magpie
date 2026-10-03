# M0 GenMC 工具链引导记录

## 授权与安装结果

- 用户通过系统 `require_escalated` 授权安装 LLVM 19 开发工具和获取固定 GenMC v0.17.0 官方源码。GenMC 源码及 build tree 均位于仓库忽略的 `build/tools/`；没有把 GenMC 加入 magpie 生产依赖，也没有安装 GenMC 可执行文件到系统目录。
- Homebrew 6.0.20 在 Intel macOS 给出 Tier 3/no-normal-bottles 警告；所需 bottle 最终从 GHCR fallback 成功获取。先尝试的 `mirrors.aliyun.com` portable-ruby URL 返回 HTTP 404，Homebrew 随后自动回退到 `ghcr.io` 并成功。
- `llvm@19` 安装版本为 19.1.7，prefix `/usr/local/opt/llvm@19`；`llvm-config --version` 与 Homebrew Clang 均报告 19.1.7。LLVM bottle 安装后占用约 1.9 GB。
- Intel 配置下 Homebrew 将 xz 5.8.4 从源码构建；其输出报告用时 1 分 43 秒。Homebrew log 时间戳显示 xz build log 11:53:09–11:54:49，本地 Cellar 中 LLVM keg 时间为 11:55:16。GenMC 安装命令在 11:52:07 时已观察为运行中，因此从该观测点至 LLVM 安装完成至少 3 分 09 秒；完整命令起止 wall-time 未由 yield-returning PTY 单独计时，不能报成精确总耗时。
- Homebrew 执行了默认 `brew cleanup`：删除了旧 xz 5.8.1、旧 zstd 1.5.7 以及缓存文件。该副作用来自获批安装命令，本记录不隐藏或回滚它。Homebrew 的 xz 构建日志保留在宿主机 `/Users/richard/Library/Logs/Homebrew/xz/`；当次镜像失败与回退信息按实际终端输出记录在上文。

## 固定 GenMC 构建

源码命令和身份：

```text
git clone --depth 1 --branch v0.17.0 https://github.com/MPI-SWS/genmc.git build/tools/genmc-v0.17.0
tag: v0.17.0
commit: 29b03a66402c4453fc77901ef3be90bb55707cd4
```

使用 Homebrew LLVM 19.1.7、AppleClang-family clang/clang++、Release 配置、默认 `BUILD_LLI=ON` 构建真实 LLVM 前端：

```text
cmake -S build/tools/genmc-v0.17.0 -B build/tools/genmc-v0.17.0-build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/local/opt/llvm@19 -DCMAKE_C_COMPILER=/usr/local/opt/llvm@19/bin/clang -DCMAKE_CXX_COMPILER=/usr/local/opt/llvm@19/bin/clang++ -DBUILD_TESTS=OFF
cmake --build build/tools/genmc-v0.17.0-build --parallel 2
build/tools/genmc-v0.17.0-build/bin/genmc --version
```

CMake 成功发现 LLVM 19.1.7、SDK 内 libffi/LibEdit/zlib 与 Threads；构建到 100% 并产出 `bin/genmc`。上游源码/LLVM headers 有非致命 warning，包括 C++23 `aligned_union_t` deprecation、missing `override`、`sprintf` deprecation 和一处 `undefined-bool-conversion`；本记录保留这些观察，没有改动 GenMC 源码。

实际模型能力结果、每条命令输出、退出码、完整 trace 和 DOT graph 位于 [通过结果目录](genmc-20261003T041133.472502Z/run.json)。第一次 runner 失败记录位于 [初次判据结果](genmc-20261003T041011.380988Z/run.json)；原因是把文档示例中的 `Assertion violation:` 当作必须的独立诊断，而 v0.17.0 当前实现对本例在 `Error: Safety violation!` 后输出 exact assert trace 和源行。按 pinned source 校准判据后，negative control 与正例均通过。
