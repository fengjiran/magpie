# M5 EventCount 候选

M5 使用 MPMC + Chase-Lev 调度、每 worker 独立 WaitSlot 和三级退避。默认配置仍为 mutex/global/CV；EventCount 是 opt-in。generic 在 macOS 验证；Linux futex 原生门禁已于 2026-10-11 在 Linux x86_64（WSL2）完成验收，M5 标记完成。状态与证据见 [M5 milestone](milestones/M5.md)。

```sh
cmake --preset release-eventcount
cmake --build --preset release-eventcount --parallel 2
python3 scripts/run_ctest.py --build-dir build/release-eventcount --mode fast
python3 scripts/run_ctest.py --build-dir build/release-eventcount --mode stress
```

`MAGPIE_PARKING_BACKEND=eventcount` 要求 `MAGPIE_ENABLE_WORK_STEALING=ON` 和 `MAGPIE_QUEUE_BACKEND=mpmc`。`MAGPIE_EVENTCOUNT_BACKEND=auto|generic|futex` 的 auto 在 Linux 选择 futex，其余平台选择 generic；非 Linux 显式 futex 会配置失败。保留独立的 `tsan-eventcount`、`asan-ubsan-eventcount`、`bench-eventcount` presets。Linux TSan 应显式使用 `-DCMAKE_CXX_COMPILER=clang++`，不通过屏蔽 GCC fence 诊断制造通过。

## 获取、注册与执行

先探测 local/global/随机 victim，再执行至多 64 次 cpu_relax、8 次 yield；退避只探测 local/global。仍无任务时 prepare epoch、注册本 worker、重新检查 stopping、完整扫描全部 victim、复查 epoch，最后进入 backend wait。所有路径配平注册，注销前不运行用户代码或发起迁移通知。

每个成功领取的非空批次保留一个独占执行项，只把其余项发布到本地。这个执行项不再被其他 worker 取走，避免有限任务在 worker 间反复迁移而没有执行。剩余项发布完成后先注销、再通知、再执行；单项批次没有剩余发布，不额外发迁移通知。原有容量钳制、单项 CAS、CallerRuns 与 capture 生命周期契约继续适用。

epoch 是不回绕的 uint64_t。每槽 word 为 Idle/Waiting/Signaled，不是 epoch 的低位。通知先推进 epoch，再标记 Waiting→Signaled，最后 wake。notify_one 在无注册者时只省扫描，仍更新 epoch；notify_all 扫描所有槽。上限和注册不配平在 Release 也 fail-fast。

generic 用每槽 mutex/cv 配对 predicate wait 与 CAS/notify；Linux 用四字节独立状态字和 FUTEX_WAIT_PRIVATE/FUTEX_WAKE_PRIVATE。EAGAIN、EINTR 和 spurious 返回后注销并重新探测；其他 syscall 错误 fail-fast。generic 的成功不能替代真实 futex 窗口验证。

## 关闭与统计

停止发布后不再重新停车，worker 在 stopping→gate→pending 裁决之前继续探测/退避。长任务尚未完成时其他 worker 可能持续消耗 CPU；这与 CV 候选的关闭期停车不同。修改关闭期停车需要另行证明最终完成通知和晚注册握手，不能仅省掉停止重查。析构 join 后 EventCount 要求注册数为零；单次 drain 不保证所有 worker 已退出或所有统计稳定。

| 字段 | 含义 |
|---|---|
| epoch | 通知入口的推进次数；无注册者也推进 |
| registered_waiters | 尚未注销的注册数量，不等于内核睡眠线程数 |
| notify_scan_slots | 通知实际探测槽数 |
| wakes | EventCount 成功标记后的 backend wake 调用数；CV 模式为 notify 请求数 |
| wake_threads | Linux wake syscall 的返回值累计；generic 为不可用 |
| wait_calls / wait_returns | 进入/返回 backend 的次数，包含立即返回，不等于实际睡眠次数 |
| wait_syscalls / wait_eagain / wait_eintr | 仅 Linux 提供实际 syscall 计数与返回分类 |
| post_wake_hit / empty | backend 返回后首次任务探测结果；包含 spurious/中断返回 |
| pre_sleep_scan_hit / empty | 注册后的最后探测结果；不代表最终一定进入内核 |

统计是逐原子读取的近似快照。后台已退出时可能没有下一次 post_wake 探测，因此不能要求该分项之和始终等于 wait_returns。

## Linux 原生验收入口

在 Linux 上运行上述 release-eventcount（auto=futex），并运行独立 TSan/ASan+UBSan 配置。fast 自动包含同一原语的 generic/futex suite、真实 EAGAIN/EINTR/spurious 用例及池级关闭/迁移回归。另以独立 build 目录设置 `MAGPIE_EVENTCOUNT_BACKEND=generic` 运行 pool control；配置中保留 MPMC、work stealing 和 eventcount。

归档源码 revision/dirty hash、编译器和 libc/内核/架构信息、CMakeCache、实际命令、退出码、CTest 日志；完成 fast/stress、部分构造失败与全部 join 后注册归零检查。CI 配置存在不算原生验收通过。

2026-10-11 该入口已在 Linux x86_64（WSL2，GCC 14/Clang 18）执行完毕：四配置 fast/stress 全过，归档 `results/linux-m5-20261011/`（本机）。ARM64、固定物理 Linux 性能与 M6 六项门槛仍分别记录。
