# ADR-003：全局 MPMC——载荷、reservation hole 与弱 try 契约

- 状态：**已修订，待实现验证（v2.0，2026-10-03）**。
- 上位约束：[magpie设计方案.md](magpie设计方案.md) §7.2、§8、§10。
- 修订：纠正 Task** 载荷/slot 作用域、满空与线性化表述、索引溢出和 oracle。旧版“已接受的系统级 lock-free FIFO”结论撤回。

## 1. 问题与约束

MPMCQueue<T> 存储 T，池实例化 MPMCQueue<Task*>。enqueue(T)、dequeue(T&) 和 dequeue_bulk(T*) 与此一致；不再给 T 多加一级指针。

每个槽包含 atomic<size_t> seq 和普通 T data。T 要求默认构造、拷贝构造/赋值均不抛且 trivially copyable；Task* 满足。只有取得相应 position 的线程才能访问 data；seq acquire/release 保护其生命周期。认领之后没有可抛操作、用户回调或分配。

容量在外部规范化，基础结构最低 2、池层最低 16；分配 byte size 与取整校验。初版只支持 64 位目标。索引不回绕，pos+capacity 前 release 有效的上限检查避免 unsigned 回绕/有符号差 UB。

## 2. 协议与骨架

代码唯一源为主设计 §7.2.1。Slot* 在重试循环外声明，break 后仍可访问。

| 阶段 | 条件/操作 |
|---|---|
| 初始 | seq[i]=i |
| 入队可写 | seq==pos，CAS enqueue_pos 取得位置 |
| 数据发布 | 写 data 后 seq.store(pos+1,release) |
| 出队可读 | seq==pos+1，CAS dequeue_pos 取得位置 |
| 槽释放 | 读 data 后 seq.store(pos+capacity,release) |

先检查令牌再 CAS；返回 false 不携带认领。size_t 索引不回绕，因此用相等/大小比较，不作两个 intptr_t 的可能溢出减法。seq 大于预期时重读 position，可无限竞争重试；cpu_relax 不代表具有时间上限。

## 3. try 失败含义与 progress

enqueue false：当前选中的物理槽还不可复用；可能是容量用尽，也可能 consumer 已认领但尚未释放槽。不能推出“已发布未消费任务数≥capacity”。

dequeue false：当前头部未发布；producer p 暂停而 p+1 已成功发布时，后继任务可存在。不能推出全局集合为空。

成功位置的 CAS 给出认领位置顺序，seq release 给出数据/槽可访问时刻。这不是严格线性化 FIFO try_empty/try_full 契约；不指定 seq release 为与严格 FIFO 等价的全局线性化点，也不宣称成功调用的返回顺序就是队列顺序。

**Reservation hole 历史**：A 认领 p 后暂停，B 发布 p+1 并返回，C 随后 dequeue false；这个历史在严格 FIFO 顺序模型中不合法，但在允许 unavailable 结果的弱 try 规格中合法。测试不能拿严格 std::queue 的“空则 false”模型判断该返回。

至少一个已认领线程暂停就可能让有效数据流停止，即使其他 try 调用不断失败返回；不把这些失败当作严格队列的 lock-free progress 证明。契约只承诺无队列 mutex、失败不消耗位置；不承诺单调用有界完成或严格 FIFO lock-free progress。池级活性依赖过门提交/消费者继续调度、完成发布/释放。头部生产者恢复发布后的通知负责恢复工作获取。

## 4. bulk 与权衡

dequeue_bulk 是最多 limit 次单元素 dequeue，每项独立 CAS，首个失败即停止。limit 在池层钳制 local_capacity，以免向容量 16 的本地 deque 推入 32 项。

初版不做段 CAS 抢占。循环单元素有可解释的共享成本；在所有外部任务都经过全局队列的负载下，bulk 并不减少“每项全局 dequeue CAS”，只减少调度决策/本地执行切换。是否需要专用 bulk 必须由数据裁决。

BULK_LIMIT 扫描 {1,4,16,32,64,128}，每个配置都钳制接收容量，观测吞吐、队列滞留、p99、stolen、缓存争用。不能用 enqueue_pos−dequeue_pos 当作精确“已发布深度”：它包含认领未发布，也不包含已认领未释放的占用槽。

## 5. 所有权、拒绝与通知

discard_oldest 等同一次 dequeue，成功后池层销毁 dropped、discarded++、dec_pending；队列不删除 Task，不保存 future/异常语义。

DiscardOldest 只删除全局当前可取得头项，不承诺全池最老提交项；头部未发布时可以失败。删除旧项后新项仍可能 enqueue 失败，最多 DISCARD_RETRY_LIMIT 次，否则 Abort。

丢弃 packaged_task 的 future 以 broken_promise 就绪。正常执行/丢弃均先完成 Task 销毁再 pending--。

成功的新任务发布后池层无条件 notify_one。队列不判 waiters、不维护近似深度，也不承担本地溢出半批转移。通知成本/闸门语义由 ADR-002 定义。

## 6. 验证

| 测试 | 覆盖 |
|---|---|
| SingleThreadFifo / WrapAround | 普通容量绕圈（物理复用，非整数回绕） |
| MultiProdMultiCons | 所有已接受 id 恰消费一次 |
| ProducerClaimStall | 认领后发布前暂停，后继发布且当前 try_dequeue false，恢复后完整消费 |
| ConsumerClaimStall | 认领后释放前暂停，producer 可暂时不能复用，恢复后进度恢复 |
| RelaxedQueueHistory | 弱 try 历史模型，单独校验成功 position 顺序/守恒 |
| MpmcIndexLimit | 上限前 fail-fast，不产生整数回绕/有符号 UB |
| DiscardUnderLoad / DiscardedFutureBrokenPromise | 丢弃/执行竞争、future 及计数 |
| BulkReceiveCapacityBound | 16 本地容量×32 bulk 限额不越界 |

测试缝全集由 test-plan §4.5 管理：producer position CAS 后/seq 发布前、consumer position CAS 后/seq 释放前。release 剔除测试钩子。

小状态历史检查必须允许 unavailable/spurious failure，成功任务仍只能消费一次；不能以宽松 false 规则掩盖成功 id 错误。标准 FIFO oracle 仅用于单线程无暂停场景。

## 7. 性能与演进

默认紧凑槽、头尾分行，W2.9 比较每槽一行。没有“256KB 必然超出 L2”的统一硬件断言。

全局 producer/consumer 争用、gate/pending/submitted/epoch 均须采样；专用 bulk、分片或严格 FIFO 队列替换为独立 ADR，不能在局部优化时静默改变当前 API 恢复语义。
