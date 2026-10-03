# 审核意见逐项修复对照（2026-10-03）

设计修订已完成；原设计没有库实现，因此“修复”指协议、骨架和验证规格修订，不能解读为实际线程池已验收。

## 1. 七项必须修复的问题

| 审核项 | 修复 | 落位 | 辅助验证 |
|---|---|---|---|
| R1 批量与连续 pop 重复认领 | 单元素 steal；池层重复单项，保留双侧 SC fence | 主设计 §5/§7.1、ADR-001 | thief 暂停、owner 连续五次 pop 后恢复，守恒通过 |
| R2 expected 改写导致 bottom 多推 | 用稳定 b+1 恢复 | 主设计 §7.1.2 | 强制 thief 赢最后项 CAS，owner 失败后为空，通过 |
| R3 overflow 容量证明错误 | 删除旧半批搬运/暂存；本地满只转投当前新任务 | 主设计 §4/§7.1.3、ADR-001 | 1024 满本地+16满全局，旧项不丢，新项内联，通过 |
| R4 失败分区交换跳项 | 删除分区循环及 drain_spill_overflow，保留单项调度降级 | 主设计 §4/§8.4 | 同上；所有旧指针完整取出；实际池 pending 待实现验证 |
| R5 共享旧字覆盖 | 每 worker 独立 Idle/Waiting/Signaled；不写共享旧 epoch | 主设计 §7.3、ADR-002 | 通知后 B 注销不能覆盖 A 状态，generic 阻塞返回，通过 |
| R6 关闭与注册 TOCTOU | 注册后 stopping 检查，之后由 epoch/Signaled 关闭握手 | 主设计 §5.3/§9、ADR-002 | 最终通知先于注册、或通知发生在复查后，两种路径通过 |
| R7 CallerRuns 自等待 | 独立 ExecutionFrame 链；本池所有执行上下文控制 guard | 主设计 §6.4/§8.3/§9 | 嵌套 A→B 仍能检测外层 A，logic_error/RAII 恢复通过；完整 CallerRuns 接入待实现 |

此处选择删除旧 overflow 协议，而不是仅扩大缓冲或修正交换循环。原因：旧协议增加 deque 与队列/唤醒/执行层耦合；单项满队列转投已能维持固定容量和 CallerRuns 逃生口，证明与实现都更简单。代价是失去一次迁移旧半批的潜在同步摊薄收益，后续必须由 benchmark 证明再立项。

## 2. 其他审核意见

| 意见 | 处置 |
|---|---|
| drain 返回保证过强 | 语义锚定成功观察 pending=0 的时刻；并发 submit 后返回瞬间不保证空 |
| hot path 成本口径矛盾 | 列出 gate、pending、submitted、epoch 和停车扫描；撤回零共享/只读比较说法 |
| worker scaling 受 producer/CallerRuns 污染 | worker_completed/inline_completed 分开；固定资源的独立 worker scaling、producer scaling、端到端路径 |
| MPMC Task** 与 slot 作用域 | Queue<T> 存 T；Task* 实例化；slot 声明在循环外；公开模板与原语编译通过 |
| callable value category | concept/invoke_result 都用 decay_t<F>&；检查从 F&& 构造及 noexcept 析构 |
| zero hardware hint/容量溢出 | 回退一个 worker；构造前异常校验、容量取整及实际 Slot byte size 检查 |
| constructor partial start | 先构造全部 victim/WaitSlot，再创建线程；失败 stopping→notify_all→join→重抛 |
| std::thread 栈参数不可能注入 | Linux pthread_create(attr) 后端与 generic std::thread 明确分离 |
| discarded future 未定义 | 明确 broken_promise，辅助 packaged_task 结果检查通过 |
| MPMC strict FIFO oracle 不匹配 | 采用 unavailable 弱 try 契约；producer/consumer hole 各有用例；正式弱历史 oracle 待实现 |
| deque 职责耦合 | 去掉对全局队列/EventCount/统计/overflow 的引用，策略归池层 |
| 瞬时 invariant 错误 | 只在受控稳定边界检查；pop 的 bottom<top 中间态合法 |
| generic 被当作 futex 证明 | 两后端测试仅为补充，Linux 原生 syscall 窗口独立验收 |
| seed replay 不复现调度 | 增加暂停点/释放顺序轨迹要求 |
| 索引/epoch 回绕假设 | 索引不回绕、上限前 release fail-fast；睡眠 word 不截断 epoch |
| 状态误标为已验证 | ADR 统一“已修订，待实现/模型检查”，不虚构实测结果 |

## 3. 已执行的有限验证

运行：

~~~sh
python3 design/validation/check_design.py
python3 design/validation/check_design.py --sanitize
git diff --check
~~~

前两项使用 C++20 Clang，直接提取主设计代码段，添加测试专用暂停点和私有状态观察；没有复制另一套 deque/MPMC 算法。公开 ThreadPool API 做模板实例化编译，不链接虚构库；停车 fixture 使用主设计的 block_on_eventcount 函数体及 generic 每槽 condition_variable 后端。

两种构建均通过 15 项：公开 API 模板编译、R1/R2、R3/R4 满队列降级、bulk 容量钳制、R5、R6 两个窗口、R7 嵌套执行帧、producer/consumer hole、Options、callable/discard future、12,000 项多生产多消费守恒、generic 并发通知/关闭。ASan+UBSan 无报告；diff whitespace 检查通过。

辅助脚本采用独立进程超时，防回归挂死长期占用。它仍只覆盖有限输入/交错，不能证明整个 C++ 内存模型协议。

## 4. 待真实实现阶段验证

库级 submit/run_one/CallerRuns/丢弃/pending/统计/构造回滚的实际接入；Linux per-slot futex 的 ABI、EAGAIN/EINTR 与 syscall 窗口；弱内存有界模型检查及 ARM64；TSan 正式用例、长时 soak；所有性能验收与新停车扫描代价。

这些是设计中明确的实现门禁，不是留待猜测的语义空白。当前修订不声称不存在其他缺陷，也不声称高性能目标已经达到。
