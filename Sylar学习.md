# Sylar 学习

## 日志系统

日志分 `Logger`，`Appender`, `Formatter` 三个组件, 依赖链是：

```cpp
Logger::log → appender->log → formatter->format
```

- `Logger`：调度中心（决定是否记录 + 分发）
- `Appender`：输出目的地（文件 / stdout / 远程）
- `Formatter`：格式化（字符串拼接规则）

`log event` 从产生，通过 Logger 调度中心决定发送的方向，是发给 `FileLogAppender` `StdoutLogAppender` `LogserverAppender`

使用 `LogEventWrap` 的原因是每次发送日志构建一个临时短命对象，目的是配合 `#define` 支持流式写日志如：

```cpp
getSS() << "hello" << x;
```

表达式结束后，对象自动析构，析构时触发真正的 log 逻辑：

```cpp
LogEventWrap::~LogEventWrap() {
    m_event->getLogger()->log(m_event->getLevel(), m_event);
}
```

### Format Pattern DSL Parser

定义日志格式化输出时使用这种模式：

```cpp
Logger::Logger(const std::string& name) : m_name(name), m_level(LogLevel::DEBUG) {
    m_formatter = std::make_shared<LogFormatter>(
        "%d{%Y-%m-%d %H:%M:%S}%T%t%T%N%T%F%T[%p]%T[%c]%T%f:%l%T%m%n");
}
```

为了实现支持这种语法，本质上就是定义一个 mini DSL +解析器，实现逻辑不需要复杂的编译原理过程, 具体逻辑在 `log.cc` 的 `void LogFormatter::init() ` 中：

首先遍历字符串，解析结果放进 `std::vector<std::tuple<std::string, std::string, int>> vec;` 中，后续定义一个 `static std::map<std::string, std::function<FormatItem::ptr(...)>> s_format_items` 把每个 pattern 中定义的字符串对应一个 `std::function对象` 的这样一个映射表，例如：

```cpp
"d" → DateTimeFormatItem
"m" → MessageFormatItem
```

最终得到 `vector<FormatItem>` 存储在 m_item 中，这里面每个 Item 就对应一个输出格式

多线程日志这里使用 `SpinLock`，对于日志这种 **高频短命任务**，恰当适合自旋锁是能很好避免线程调度开销的

## 锁设计

第一层是 OS 线程层，比较常规，例如：

```cpp
Mutex / RWMutex / Semaphore
```

第二层是一些 **用户态** 中为高性能设计的锁：

```cpp
Spinlock / CASLock / RWSpinlock
```

- `SpinLock` 是基于 `pthread_spinlock_t` 的一层封装，特点是如果持锁线程不释放会一直自选，拿 CPU busy wait 换 latency

- CASLock 是原子锁基于 atomic 库，代码比较简单：

  ```cpp
  /**
   * @brief 上锁
   */
  // 尝试把m_mutex 从0设置到1
  // 如果修改成功，则拿到锁
  // 否则一直自旋
  void lock() {
      while (std::atomic_flag_test_and_set_explicit(&m_mutex, std::memory_order_acquire));
  }
  
  /**
   * @brief 解锁
   */
  void unlock() {
      std::atomic_flag_clear_explicit(&m_mutex, std::memory_order_release);
  }
  private:
  /// 原子状态
  volatile std::atomic_flag m_mutex;
  ```

  - `std::memory_order_acquire`：使用 atomic 时，需要考虑到 CPU 和编译器会做乱序执行和指令重排，这个参数的作用是让 CPU 插入 `load barrier（读屏障）` 禁止读操作被提前到 lock 之前
  - `std::memory_order_acquire`: 同理，禁止写操作被移动到 unlock 之后，插入 `store barrier（写屏障）`
  - 任何语言使用 `atomic` 或者 `CASLock` 的适用场景是：
    - 单一状态（一个变量 / 一个指针 / 一个状态机）
    - 操作是“原子转换”（比如计数器，状态变量, flag 切换 true/false）
    - 不考虑多步业务逻辑先后顺序性（单一读写操作原子而不是过程原子化）
    - 极高频读（读多写少）

## 用户线程模型

- `thread_local` 定义变量全局可访问，但全局不共享（每个线程复制一份），这样做的目的是代码中可以直接使用变量名，但实际上线程间不共享这个变量数据，比如定义 `ThreadName`, `ThreadID`
- `pthread_join` 是阻塞，等待线程完成返回数据再继续, `detech` 是分离线程的生命周期，线程任务完成后自行被操作系统回收

## Stream 流

主要解决网络通信中 `write` 和 `recv` 行为和代码预期不一致，导致协议解析问题（粘包）

- `readFixSize` 和 `writeFixSize` 在 while 中循环读写，目的是防止出现缓冲区不足或网络问题导致一次 `write/read` 收不到预期 bufferSize 的情况
- `Stream` 是一个虚基类，提供接口比如 `read` `write` 需要具体实例实现，而 `readFixSize` 和 `writeFixSize` 则是带实现的虚函数，因为逻辑通用

## Fiber(有栈协程设计)

sylar 的作者使用 `epoll+boost fcontext+thread` 的方式模拟协程模型（不是 `io_uring`）

实现或调用这个协程模型时要注意，这套协程从协程本身到调度器都是跑在 OS 线程内，所以一定要注意不能造成卡死否则会导致整个调度器阻塞

### 主线程协程化

在创建任何子协程之前，当前运行的普通线程必须先给自己披上一件“协程的外衣”，这样它才能参与后续的切换。

1. **`Fiber::GetThis()`**：这是获取当前协程的接口。如果 `t_fiber` 为空，说明当前线程还没初始化过协程，它会立刻 `NewFiber()` 创建一个主协程，并赋值给 `t_threadFiber`。
2. **`Fiber::Fiber()` (无参构造函数)**：注意看这个构造函数，它极其简单。它没有分配栈空间（`m_stacksize`），也没有设置回调函数（`m_cb`）。因为它就是主线程本身，它已经在运行了，有自己的系统栈，所以只需要记录一下状态（`m_state = EXEC`）并把自己设为 `t_fiber` 即可

### 内存分配器（MallocStackAllocator/MMapStackAllocator）

`MallocStackAllocator` 就是基于 `malloc` 的一层简单封装，后者则是封装 `mmap` 和 `munmap`

在 `NewFiber` 函数中，我们可以看到：

```cpp
Fiber* NewFiber(std::function<void()> cb, size_t stacksize, bool use_caller) {
    stacksize = stacksize ? stacksize : g_fiber_stack_size->getValue();
    // Fiber* p = (Fiber*)malloc(sizeof(Fiber) + stacksize);
    // Fiber* p = (Fiber*)s_fiber_pool.alloc(sizeof(Fiber) + stacksize);
    Fiber* p = (Fiber*)StackAllocator::Alloc(sizeof(Fiber) + stacksize);
    return new (p) Fiber(cb, stacksize, use_caller);
    // p->Fiber(cb, stacksize, use_caller);
    // return p;
}
```

一个 Fiber 协程对象 p 使用自封装的 Allocator 分配到了 `sizeof(Fiber) + stacksize` 大小的协程栈，那么多出来的 `stacksize` 意义在于 **内存布局**：内存块的前部存放 `Fiber` 对象本身，紧接着后面就是该协程的运行栈，随后在这块分配到的连续内存上直接原地 new 构造

就像这样：

```cpp
// 申请一整块内存，比如 128KB + 1KB
void* raw_ptr = StackAllocator::Alloc(sizeof(Fiber) + stacksize);

// [一段：Fiber对象] [二段：空白的栈空间]
// ^ p 指向这里
```

### 协程构建原理

这段栈空间用于存放协程要执行的代码数据，为什么需要：

- 如果让让协程直接使用“普通函数栈”（即线程栈），会遇到一个致命的物理冲突：**栈的连续性（Continuity）与覆盖（Overwriting）问题**
- 正常来讲，协程是一种轻量化的线程，多个协程可以跑在同一个线程，又或者不同线程的 ` M: N ` 模型上，这种时候协程的切换就不能依靠 pthread 本身的现场保护（context），而是手动管理 context，那么每个协程有独立运行的栈空间，以及独立的 context 模型就非常必要

从正常 c++语法中，你没办法把要执行的代码数据放到某对象指针指向对象后的某段空间，所以引入 `ucontext` 或 `fcontext`：

```cpp
Fiber::Fiber() {
    m_state = EXEC;
    SetThis(this);
#if FIBER_CONTEXT_TYPE == FIBER_UCONTEXT
    if (getcontext(&m_ctx)) {
        SYLAR_ASSERT2(false, "getcontext");
    }
#elif FIBER_CONTEXT_TYPE == FIBER_LIBCO
    coctx_init(&m_ctx);
#elif FIBER_CONTEXT_TYPE == FIBER_LIBACO
    aco_thread_init(nullptr);
    m_ctx = aco_create(nullptr, nullptr, 0, nullptr, nullptr);
#endif

    ++s_fiber_count;

    SYLAR_LOG_DEBUG(g_logger) << "Fiber::Fiber main";
}

Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool use_caller)
    : m_id(++s_fiber_id), m_cb(cb) {
    ++s_fiber_count;
    // m_stacksize = stacksize ? stacksize : g_fiber_stack_size->getValue();
    m_stacksize = stacksize;

    // m_stack = StackAllocator::Alloc(m_stacksize);
#if FIBER_CONTEXT_TYPE == FIBER_UCONTEXT
    if (getcontext(&m_ctx)) {
        SYLAR_ASSERT2(false, "getcontext");
    }
    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;

    if (!use_caller) {
        makecontext(&m_ctx, &Fiber::MainFunc, 0);
    } else {
        makecontext(&m_ctx, &Fiber::CallerMainFunc, 0);
    }
#elif FIBER_CONTEXT_TYPE == FIBER_FCONTEXT
    if (!use_caller) {
        m_ctx = make_fcontext((char*)m_stack + m_stacksize, m_stacksize, &Fiber::MainFunc);
    } else {
        m_ctx = make_fcontext((char*)m_stack + m_stacksize, m_stacksize, &Fiber::CallerMainFunc);
    }
#elif FIBER_CONTEXT_TYPE == FIBER_LIBCO
    m_ctx.ss_size = m_stacksize;
    m_ctx.ss_sp = (char*)m_stack;
    if (!use_caller) {
        coctx_make(&m_ctx, &Fiber::MainFunc, 0, 0);
    } else {
        coctx_make(&m_ctx, &Fiber::CallerMainFunc, 0, 0);
    }
#elif FIBER_CONTEXT_TYPE == FIBER_LIBACO
    aco_share_stack_init(&m_astack, m_stack, m_stacksize);
    m_ctx = aco_create(t_threadFiber->m_ctx, &m_astack, 0, &Fiber::MainFunc, nullptr);
#endif

    SYLAR_LOG_DEBUG(g_logger) << "Fiber::Fiber id=" << m_id;
}
```

- `ucontext`：系统调用级别的上下文管理，定义在 glibc 中。它主要通过 `getcontext`、`makecontext` 和 `setcontext` 这三个函数来操作。

- 关键步骤：

  - **`getcontext(&m_ctx)`**：首先获取 **当前 CPU 的所有寄存器快照**，存入 `m_ctx`。此时 `m_ctx.uc_mcontext` 里的 `RSP` 还是指向当前的系统栈。

  - **手动修改栈信息**：

    - `m_ctx.uc_stack.ss_sp = m_stack;`（你分配的堆内存起始地址）

    - `m_ctx.uc_stack.ss_size = m_stacksize;`

  - **`makecontext` 的劫持逻辑**： 这是最核心的一步。`makecontext` 会修改 `m_ctx` 结构体中保存的寄存器数值：

    - 它会将 `m_ctx` 中的 **`RSP`** 修改为你提供的 `m_stack` 的末尾（栈向下增长）。

    - 它会将 **`RIP`**（指令指针）修改为函数指针 `&Fiber::MainFunc`。

    - 它会在新栈的顶部压入一些参数或返回地址。
  
  - 当后续调用 `swapcontext` 或 `setcontext` 时，CPU 的 `RSP` 寄存器会被强制加载为 `m_ctx` 中保存的那个新地址，程序便开始在你的私有栈上运行了。

#### 代码中的“劫持”点解析

该协程模型为 **有栈协程**，对于上下文保存的方案，根据条件宏不同选择不同的方式：

**ucontext 的劫持（来自 glibc，属于 syscall，涉及到强制保存和恢复信号掩码（Signal Mask）需要用户态到内核态的上下文切换，性能较慢）：**

在你的代码第 28-35 行：

```cpp
#if FIBER_CONTEXT_TYPE == FIBER_UCONTEXT
    if (getcontext(&m_ctx)) {
        SYLAR_ASSERT2(false, "getcontext");
    }
    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;       // 1. 指定劫持的目标内存块地址
    m_ctx.uc_stack.ss_size = m_stacksize; // 2. 指定栈大小

    if (!use_caller) {
        makecontext(&m_ctx, &Fiber::MainFunc, 0); // 3. 核心劫持：修改 m_ctx 里的 RSP 和 RIP
    } 
    // ...
#endif
```

- **原理点**：`makecontext` 会直接操作 `m_ctx` 这个结构体。它会将 `m_ctx.uc_mcontext.gregs[REG_RSP]`（x86_64 下）修改为 `ss_sp + ss_size` 附近的位置，并将 `REG_RIP` 修改为 `MainFunc` 的入口。

**fcontext 的劫持（来自 boost.context，切换速度比 ucontext 快 10 倍以上）：**

在你的代码第 43-48 行：

```cpp
#elif FIBER_CONTEXT_TYPE == FIBER_FCONTEXT
    if (!use_caller) {
        // 核心劫持：通过汇编函数直接计算出切换后的栈帧起点
        m_ctx = make_fcontext((char*)m_stack + m_stacksize, m_stacksize, &Fiber::MainFunc);
    } 
    // ...
#endif
```

- **原理点**：`make_fcontext` 本身就是一段汇编逻辑。它接收栈顶指针（高地址），然后在该内存顶端 **强行压入** 一个预先构造好的寄存器环境（跳转地址、状态等），并返回一个新的上下文指针给 `m_ctx`。

**libco 的劫持（来自腾讯，高性能有栈协程代表）：**

在你的代码第 49-56 行：

```cpp
#elif FIBER_CONTEXT_TYPE == FIBER_LIBCO
    m_ctx.ss_size = m_stacksize;
    m_ctx.ss_sp = (char*)m_stack;
    if (!use_caller) {
        coctx_make(&m_ctx, &Fiber::MainFunc, 0, 0); // 核心劫持：手动设置上下文
    } else {
        coctx_make(&m_ctx, &Fiber::CallerMainFunc, 0, 0);
    }
#endif
```

- **原理点**：`libco` 并没有使用 `ucontext` 的系统调用，而是定义了自己的 `coctx_t` 结构体（通常只包含一个寄存器数组）。`coctx_make` 是一个 **C/汇编混合函数**，它将你传入的栈空间（`ss_sp`）进行对齐处理，然后模仿函数调用的压栈逻辑，将 `MainFunc` 的地址放到栈帧的特定位置。
- **切换特性**：它的 `coctx_swap` 是一段精简的汇编，通过 `push` 保存当前寄存器到旧栈，`mov` 切换 `RSP` 到新栈，再 `pop` 恢复寄存器。它不保存信号掩码，极致追求生产环境下的高并发切换性能。

**libaco 的劫持（轻量级 C 协程库，极致精简）：**

在你的代码第 57-60 行：

```cpp
#elif FIBER_CONTEXT_TYPE == FIBER_LIBACO
    aco_share_stack_init(&m_astack, m_stack, m_stacksize); // 1. 初始化共享栈或独立栈
    m_ctx = aco_create(t_threadFiber->m_ctx, &m_astack, 0, &Fiber::MainFunc, nullptr); // 2. 创建上下文
#endif
```

- **原理点**：`libaco` 的核心设计在于它区分了“执行者”和“栈”。`aco_create` 会在内存中分配一个极小的 `aco_t` 结构体，其劫持方式与 `fcontext` 类似，通过汇编预设一个跳转环境。
- **黑科技：共享栈（Share Stack）**：代码中的 `aco_share_stack_init` 暗示了它支持多个协程复用同一块物理内存栈。切换时，它会将当前栈内存 Copy 出去（Save），再把目标的栈内容 Copy 进来（Restore）。
- **劫持逻辑**：它通过 `acosw`（aco switch）汇编函数直接劫持 `RSP` 和 `RET` 地址。由于其内部状态极简，它是目前 C 语言实现中开销最小的协程方案之一。

**总结对比**

| **方案**     | **归属/来源**  | **劫持手段**                  | **性能评价** | **备注**                             |
| ------------ | -------------- | ----------------------------- | ------------ | ------------------------------------ |
| **ucontext** | POSIX / glibc  | `makecontext` 修改结构体      | 慢 (Syscall) | 兼容性最好，但不再推荐用于高性能场景 |
| **fcontext** | Boost.Context  | 汇编直接在栈顶预压寄存器数据  | 极快         | 现代 C++ 协程库的首选底层            |
| **libco**    | Tencent (微信) | 手动构造 `coctx` 寄存器阵列   | 极快         | 针对高并发优化，经受过大规模业务考验 |
| **libaco**   | 三方轻量库     | 预制 `aco_t` 环境，支持共享栈 | 极快+省内存  |                                      |

### 协程切换

#### `swapIn()` 和 `swapOut()`

这两个函数用于 **协程** 与 **调度协程（MainFiber）** 之间的切换。

- **`swapIn()`**：将当前 CPU 执行权交给该协程。它会保存调度协程的现场，恢复当前协程的现场。
- **`swapOut()`**：当前协程主动让出执行权。它会保存当前现场，跳回到调度协程。

```cpp
// 切换到当前协程执行
void Fiber::swapIn() {
    SetThis(this);
    SYLAR_ASSERT(m_state != EXEC);
    m_state = EXEC;
#if FIBER_CONTEXT_TYPE == FIBER_UCONTEXT
    if (swapcontext(&Scheduler::GetMainFiber()->m_ctx, &m_ctx)) {
        SYLAR_ASSERT2(false, "swapcontext");
    }
#elif FIBER_CONTEXT_TYPE == FIBER_FCONTEXT
    jump_fcontext(&Scheduler::GetMainFiber()->m_ctx, m_ctx, 0);
#elif FIBER_CONTEXT_TYPE == FIBER_LIBCO
    coctx_swap(&Scheduler::GetMainFiber()->m_ctx, &m_ctx);
#elif FIBER_CONTEXT_TYPE == FIBER_LIBACO
    acosw(Scheduler::GetMainFiber()->m_ctx, m_ctx);
#endif
}

void Fiber::swapOut() {
    SetThis(Scheduler::GetMainFiber());
#if FIBER_CONTEXT_TYPE == FIBER_UCONTEXT
    if (swapcontext(&m_ctx, &Scheduler::GetMainFiber()->m_ctx)) {
        SYLAR_ASSERT2(false, "swapcontext");
    }
#elif FIBER_CONTEXT_TYPE == FIBER_FCONTEXT
    jump_fcontext(&m_ctx, Scheduler::GetMainFiber()->m_ctx, 0);
#elif FIBER_CONTEXT_TYPE == FIBER_LIBCO
    coctx_swap(&m_ctx, &Scheduler::GetMainFiber()->m_ctx);
#elif FIBER_CONTEXT_TYPE == FIBER_LIBACO
    acosw(m_ctx, Scheduler::GetMainFiber()->m_ctx);
#endif
}
```

#### `call()` 和 `back()`

同样是用于协程切换，但这个是绕过了 `scheduler` 直接手动切换当前主协程

- `swapin` 和 `swapout` 属于在 scheduler 内调用，业务端只需要提交协程任务就会自动管理调度
- 而手动 `call` 和 `back` 虽然有更高的控制细粒度，但需要使用时小心的规划 call 和 back 的时机，否则就会导致协程在主线程上长时间卡死

```cpp
void Fiber::call() {
    SetThis(this);
    m_state = EXEC;
#if FIBER_CONTEXT_TYPE == FIBER_UCONTEXT
    if (swapcontext(&t_threadFiber->m_ctx, &m_ctx)) {
        SYLAR_ASSERT2(false, "swapcontext");
    }
#elif FIBER_CONTEXT_TYPE == FIBER_FCONTEXT
    jump_fcontext(&t_threadFiber->m_ctx, m_ctx, 0);
#elif FIBER_CONTEXT_TYPE == FIBER_LIBCO
    coctx_swap(&t_threadFiber->m_ctx, &m_ctx);
#elif FIBER_CONTEXT_TYPE == FIBER_LIBACO
    acosw(t_threadFiber->m_ctx, m_ctx);
#endif
}

void Fiber::back() {
    SetThis(t_threadFiber.get());
#if FIBER_CONTEXT_TYPE == FIBER_UCONTEXT
    if (swapcontext(&m_ctx, &t_threadFiber->m_ctx)) {
        SYLAR_ASSERT2(false, "swapcontext");
    }
#elif FIBER_CONTEXT_TYPE == FIBER_FCONTEXT
    jump_fcontext(&m_ctx, t_threadFiber->m_ctx, 0);
#elif FIBER_CONTEXT_TYPE == FIBER_LIBCO
    coctx_swap(&m_ctx, &t_threadFiber->m_ctx);
#elif FIBER_CONTEXT_TYPE == FIBER_LIBACO
    acosw(m_ctx, t_threadFiber->m_ctx);
#endif
}
```

#### 协程执行 `MainFunc` 和 `CallMainFunc`

包装一层实际执行的函数 `std:function m_cb`，加上 try-catch 异常处理和协程状态机控制

```cpp
// MainFunc为std:function cb的执行包装器
#if FIBER_CONTEXT_TYPE == FIBER_UCONTEXT || FIBER_CONTEXT_TYPE == FIBER_LIBACO
void Fiber::MainFunc() {
#elif FIBER_CONTEXT_TYPE == FIBER_FCONTEXT
void Fiber::MainFunc(intptr_t vp) {
#elif FIBER_CONTEXT_TYPE == FIBER_LIBCO
void* Fiber::MainFunc(void*, void*) {
#endif
    // 静态方法，首先要拿到当前线程正在跑的那个对象实例
    Fiber::ptr cur = GetThis();
    SYLAR_ASSERT(cur);
    try {
        // 执行m_cb函数
        cur->m_cb();
        cur->m_cb = nullptr;
        cur->m_state = TERM;
    } catch (std::exception& ex) {
        cur->m_state = EXCEPT;
        SYLAR_LOG_ERROR(g_logger) << "Fiber Except: " << ex.what() << " fiber_id=" << cur->getId()
                                  << std::endl
                                  << sylar::BacktraceToString();
    } catch (...) {
        cur->m_state = EXCEPT;
        SYLAR_LOG_ERROR(g_logger) << "Fiber Except"
                                  << " fiber_id=" << cur->getId() << std::endl
                                  << sylar::BacktraceToString();
    }

    // 先拿原始指针reset清空再swapout的原因:
    // 如果直接cur.swapout，CPU 执行 swapOut：寄存器环境（RSP, RIP）瞬间从当前协程栈切换到了调度器栈
    // 局部变量cur（智能指针）还存在于协程栈的内存里，它的析构函数（Destructor）没有被调用，造成内存泄露
    // 虽然get拿到原始指针reset，会担心造成指针悬空，因为此时引用计数已经归0，会触发析构
    // 或者说逻辑上的“自毁”：虽然对象在逻辑上已经可以销毁了，但因为当前 CPU
    // 还在跑这个对象的静态函数（MainFunc），
    // 且我们只用了寄存器里的 raw_ptr 地址，所以程序能撑住最后一口气
    // 这里使用的是寄存器地址而不是已经销毁的内存地址，所以不会造成UB
    auto raw_ptr = cur.get();
    cur.reset();
    raw_ptr->swapOut();

    SYLAR_ASSERT2(false, "never reach fiber_id=" + std::to_string(raw_ptr->getId()));
}

#if FIBER_CONTEXT_TYPE == FIBER_UCONTEXT || FIBER_CONTEXT_TYPE == FIBER_LIBACO
void Fiber::CallerMainFunc() {
#elif FIBER_CONTEXT_TYPE == FIBER_FCONTEXT
void Fiber::CallerMainFunc(intptr_t vp) {
#elif FIBER_CONTEXT_TYPE == FIBER_LIBCO
void* Fiber::CallerMainFunc(void*, void*) {
#endif
    Fiber::ptr cur = GetThis();
    SYLAR_ASSERT(cur);
    try {
        cur->m_cb();
        cur->m_cb = nullptr;
        cur->m_state = TERM;
    } catch (std::exception& ex) {
        cur->m_state = EXCEPT;
        SYLAR_LOG_ERROR(g_logger) << "Fiber Except: " << ex.what() << " fiber_id=" << cur->getId()
                                  << std::endl
                                  << sylar::BacktraceToString();
    } catch (...) {
        cur->m_state = EXCEPT;
        SYLAR_LOG_ERROR(g_logger) << "Fiber Except"
                                  << " fiber_id=" << cur->getId() << std::endl
                                  << sylar::BacktraceToString();
    }

    auto raw_ptr = cur.get();
    cur.reset();
    raw_ptr->back();
    SYLAR_ASSERT2(false, "never reach fiber_id=" + std::to_string(raw_ptr->getId()));
}
```

- `CallerMainFunc` 的区别在于末尾使用的是 back 而不是 swapout，也就是这个函数用于绕过 scheduler 单独处理的情况

## 协程调度 Scheduler

协程调度器就在这两个文件：

  - 定义：`sylar/scheduler.h:27`
  - 实现：`sylar/scheduler.cc:14`

它的定位是：**N-M 协程调度器，也就是 N 个线程运行 M 个协程/回调任务**。

注意这里的 Scheduler 只负责“线程池 + 协程任务队列 + 调度循环”。它本身不负责 IO 监听，不负责定时器，也不负责 epoll。后面的 `IOManager` 继承它以后，才把 `idle()` 和 `tickle()` 扩展成 epoll 事件循环和 pipe 唤醒。

### 核心数据结构

Scheduler 最重要的成员在 `sylar/scheduler.h:223` 之后：

```cpp
RWMutexType m_mutex;
std::vector<Thread::ptr> m_threads;
std::list<FiberAndThread> m_fibers;
Fiber::ptr m_rootFiber;
std::vector<int> m_threadIds;
size_t m_threadCount = 0;
std::atomic<size_t> m_activeThreadCount = {0};
std::atomic<size_t> m_idleThreadCount = {0};
bool m_stopping = true;
bool m_autoStop = false;
int m_rootThread = 0;
```

可以按职责拆成三类：

- `m_threads` / `m_threadIds`：调度器持有的 OS 线程池。
- `m_fibers`：待执行任务队列，里面既可以放 `Fiber::ptr`，也可以放 `std::function<void()>`。
- `m_rootFiber` / `m_rootThread`：当 `use_caller = true` 时，调用构造函数的线程也会变成调度线程，这个线程上的调度循环被包装成 `m_rootFiber`。
- `m_activeThreadCount` / `m_idleThreadCount`：统计当前有多少线程正在执行任务，多少线程在 idle。
- `m_stopping` / `m_autoStop`：控制调度器生命周期。

`FiberAndThread` 是任务队列里的元素，位置在 `sylar/scheduler.h:166`：

```cpp
struct FiberAndThread {
    Fiber::ptr fiber;
    std::function<void()> cb;
    int thread;
};
```

它表示一个待调度单元：

- `fiber`：已经创建好的协程。
- `cb`：普通回调，调度器执行时会临时包装成 Fiber。
- `thread`：指定运行线程，`-1` 表示任意调度线程都可以执行。

这里有一个细节：`FiberAndThread(Fiber::ptr* f, int thr)` 和 `FiberAndThread(std::function<void()>* f, int thr)` 使用的是 `swap`。这意味着如果你传入指针版本，任务所有权会被移动进队列，原对象会被置空，避免同一个 Fiber 被外部和调度器同时持有并重复调度。

### 两个 thread_local 变量

`sylar/scheduler.cc:11` 定义了两个线程局部变量：

```cpp
static thread_local Scheduler* t_scheduler = nullptr;
static thread_local Fiber* t_scheduler_fiber = nullptr;
```

- `t_scheduler`：当前线程属于哪个 Scheduler。`Scheduler::GetThis()` 返回的就是它。
- `t_scheduler_fiber`：当前线程的“调度协程”。`Fiber::swapIn()` / `swapOut()` 依赖 `Scheduler::GetMainFiber()` 找到它。

这两个变量是 thread_local 的原因是：调度器里有多个 OS 线程，每个线程都有自己的当前协程、主协程和调度上下文。如果做成全局共享变量，多个线程切换协程时会互相覆盖。

### 构造函数和 use_caller

构造函数位置：`sylar/scheduler.cc:15`。

```cpp
Scheduler::Scheduler(size_t threads, bool use_caller, const std::string& name)
```

`use_caller` 决定“调用构造函数的当前线程”是否也参与调度。

#### `use_caller = false`

如果 `use_caller = false`：

- 当前线程不参与调度。
- `m_rootThread = -1`。
- `m_threadCount = threads`。
- `start()` 会创建 `threads` 个新线程，每个线程都运行 `Scheduler::run()`。

测试文件 `tests/test_scheduler.cc` 走的就是这个分支：

```cpp
sylar::Scheduler sc(3, false, "test");
sc.start();
sc.schedule(&test_fiber);
sc.stop();
```

这里主线程只负责启动、提交任务和停止，真正执行 `test_fiber` 的是调度器创建出的 3 个工作线程。

#### `use_caller = true`

如果 `use_caller = true`：

```cpp
sylar::Fiber::GetThis();
--threads;
t_scheduler = this;
m_rootFiber.reset(NewFiber([this] { run(); }, 0, true), FreeFiber);
t_scheduler_fiber = m_rootFiber.get();
m_rootThread = sylar::GetThreadId();
m_threadIds.push_back(m_rootThread);
```

这段代码做了几件事：

1. 先调用 `Fiber::GetThis()`，把当前普通线程初始化成主协程。
2. `--threads`，因为当前线程也算一个调度线程，所以需要少创建一个新线程。
3. 创建 `m_rootFiber`，它的执行函数就是 `Scheduler::run()`。
4. 把当前线程 id 记录为 `m_rootThread`。

比如 `Scheduler sc(3, true, "test")`，最终效果是：

- 当前调用线程：1 个调度线程。
- 新创建线程：2 个调度线程。
- 总调度能力仍然是 3 个线程。

这里容易误解的一点是：`start()` 里没有直接 `m_rootFiber->call()`，相关代码被注释掉了。当前版本中，如果 `use_caller = true`，caller 线程真正进入 `run()` 的时机是在 `stop()` 里：

```cpp
if (m_rootFiber) {
    if (!stopping()) {
        m_rootFiber->call();
    }
}
```

所以在 `use_caller = true` 且没有额外工作线程的场景下，`start()` 后提交的任务，通常要等到调用 `stop()` 时由 caller 线程跑完。这是 sylar 这个版本的行为，读源码时要按当前代码理解，不要按常见线程池直觉理解成 `start()` 立刻让 caller 线程阻塞进入事件循环。

### start：创建工作线程

`start()` 位置：`sylar/scheduler.cc:56`。

核心逻辑是：

```cpp
m_stopping = false;
m_threads.resize(m_threadCount);

for (size_t i = 0; i < m_threadCount; ++i) {
    m_threads[i] =
        std::make_shared<Thread>([this] { run(); }, m_name + "_" + std::to_string(i));
    m_threadIds.push_back(m_threads[i]->getId());
}
```

也就是说，`start()` 并不直接执行任务，它只是创建 OS 线程，并让每个线程进入 `run()` 调度循环。任务什么时候被执行，取决于后续是否有人调用 `schedule()` 把任务放进 `m_fibers`。

### schedule：提交任务

`schedule()` 在 `sylar/scheduler.h:79`，它是模板函数，可以接收：

- `Fiber::ptr`
- `Fiber::ptr*`
- `std::function<void()>`
- `std::function<void()>*`
- 普通函数指针，例如 `&test_fiber`

核心逻辑：

```cpp
template <class FiberOrCb>
void schedule(FiberOrCb fc, int thread = -1) {
    bool need_tickle = false;
    {
        RWMutexType::WriteLock lock(m_mutex);
        need_tickle = scheduleNoLock(fc, thread);
    }

    if (need_tickle) {
        tickle();
    }
}
```

`scheduleNoLock()` 只做两件事：

```cpp
bool need_tickle = m_fibers.empty();
FiberAndThread ft(fc, thread);
if (ft.fiber || ft.cb) {
    m_fibers.push_back(ft);
}
return need_tickle;
```

这里 `need_tickle = m_fibers.empty()` 的意思是：如果提交任务之前队列是空的，那么可能有工作线程正睡在 `idle()` 里，需要唤醒一下。普通 Scheduler 的 `tickle()` 只是打日志；IOManager 重写后会往 pipe 写数据，唤醒 `epoll_wait`。

批量 `schedule(begin, end)` 也是同样逻辑，只是一次性把多个任务塞进队列，最后统一 tickle，避免每插入一个任务都唤醒一次。

### run：调度主循环

`run()` 是 Scheduler 的核心，位置：`sylar/scheduler.cc:141`。

每个工作线程启动后都会进入 `run()`。它先做线程级初始化：

```cpp
set_hook_enable(true);
setThis();
if (sylar::GetThreadId() != m_rootThread) {
    t_scheduler_fiber = Fiber::GetThis().get();
}
```

含义是：

- 开启当前线程的 hook。
- 把当前线程绑定到这个 Scheduler。
- 如果不是 caller 线程，就把当前线程的主协程设置为调度协程。

然后创建一个 `idle_fiber`：

```cpp
Fiber::ptr idle_fiber(NewFiber([this] { idle(); }), FreeFiber);
```

调度循环可以简化成下面的伪代码：

```cpp
while (true) {
    从 m_fibers 中找一个当前线程可以执行的任务;

    if (找到了 Fiber) {
        fiber->swapIn();
        根据 Fiber 状态决定是否重新 schedule 或置为 HOLD;
    } else if (找到了 callback) {
        把 callback 包装/复用成 cb_fiber;
        cb_fiber->swapIn();
        根据 cb_fiber 状态决定是否重新 schedule、reset 或 HOLD;
    } else {
        没有任务，执行 idle_fiber;
    }
}
```

#### 取任务逻辑

取任务时会遍历 `m_fibers`：

```cpp
while (it != m_fibers.end()) {
    if (it->thread != -1 && it->thread != sylar::GetThreadId()) {
        ++it;
        tickle_me = true;
        continue;
    }

    if (it->fiber && it->fiber->getState() == Fiber::EXEC) {
        ++it;
        continue;
    }

    ft = *it;
    m_fibers.erase(it++);
    ++m_activeThreadCount;
    is_active = true;
    break;
}
tickle_me |= !m_fibers.empty();
```

几个关键点：

- 如果任务指定了 `thread`，但不是当前线程，就跳过，并设置 `tickle_me = true`，提醒其他线程可能有它们能执行的任务。
- 如果 Fiber 已经是 `EXEC`，说明它正在执行，不能重复切进去。
- 成功取出任务后，从队列删除，并增加 `m_activeThreadCount`。
- 如果队列里还有任务，继续 `tickle()`，让别的空闲线程也来抢任务。

这是一种很典型的“多线程共享任务队列 + 条件过滤”的调度方式。它不是 work stealing，因为所有线程看的是同一个 `m_fibers` 队列。

#### 执行 Fiber 任务

```cpp
ft.fiber->swapIn();
--m_activeThreadCount;

if (ft.fiber->getState() == Fiber::READY) {
    schedule(ft.fiber);
} else if (ft.fiber->getState() != Fiber::TERM &&
           ft.fiber->getState() != Fiber::EXCEPT) {
    ft.fiber->m_state = Fiber::HOLD;
}
```

`swapIn()` 会从调度协程切到业务协程。业务协程执行一段时间后可能出现几种状态：

- `TERM`：执行完了，不再调度。
- `EXCEPT`：执行异常，不再调度。
- `READY`：主动让出，但希望继续运行，重新放回任务队列。
- 其他未结束状态：设置成 `HOLD`，表示暂时挂起，等待外部事件重新调度。

比如 hook IO 的场景里，协程读 socket 遇到 `EAGAIN` 后会注册 IO 事件，然后 `YieldToHold()`。这时调度器不会立刻把它放回队列，而是等 IOManager 在 epoll 事件到来后再 `schedule()` 它。

#### 执行 callback 任务

如果提交的是普通函数或 lambda，调度器会把它包装成 Fiber：

```cpp
if (cb_fiber) {
    cb_fiber->reset(ft.cb);
} else {
    cb_fiber.reset(NewFiber(ft.cb), FreeFiber);
}
cb_fiber->swapIn();
```

这里复用 `cb_fiber` 是一个小优化：同一个调度线程循环执行很多 callback 时，不必每次都重新分配 Fiber 对象和协程栈。callback 执行完成后，如果状态是 `TERM` 或 `EXCEPT`，就 `reset(nullptr)` 清掉回调，等待下次复用。

### idle：没有任务时做什么

普通 Scheduler 的 `idle()` 很简单：

```cpp
while (!stopping()) {
    sylar::Fiber::YieldToHold();
}
```

它只是不断让出执行权，避免调度循环没有任务时直接退出。因为普通 Scheduler 没有 epoll、timer、条件变量等阻塞等待机制，所以它的 idle 更像是一个占位实现。

IOManager 会重写 `idle()`，在里面执行 `epoll_wait`。所以真正生产环境里更常用的是 IOManager：没有普通协程任务时，线程睡在 epoll 上；有 fd 事件、定时器或 pipe 唤醒后，再把事件回调重新 `schedule()` 到 Scheduler。

### stop：停止调度器

`stop()` 位置：`sylar/scheduler.cc:81`。

停止流程是：

1. 设置 `m_autoStop = true`。
2. 检查调用 stop 的线程是否合法：
   - `use_caller = true` 时，必须在 root 线程调用 stop。
   - `use_caller = false` 时，不能在调度器线程里调用 stop。
3. 设置 `m_stopping = true`。
4. 对所有工作线程 `tickle()`，让 idle 的线程醒来检查退出条件。
5. 如果有 `m_rootFiber` 且还没达到停止条件，就 `m_rootFiber->call()`，让 caller 线程也进入调度循环处理剩余任务。
6. 交换出 `m_threads`，逐个 `join()`。

真正判断能不能停的是 `stopping()`：

```cpp
return m_autoStop && m_stopping && m_fibers.empty() && m_activeThreadCount == 0;
```

也就是必须同时满足：

- 已经请求自动停止。
- 调度器进入 stopping 状态。
- 任务队列为空。
- 没有线程正在执行任务。

所以 `stop()` 不是粗暴停止。它会等已经提交的任务跑完，再让调度线程退出。

### switchTo 和 SchedulerSwitcher

`switchTo()` 用于把当前协程切换到某个 Scheduler 或某个指定线程上执行：

```cpp
schedule(Fiber::GetThis(), thread);
Fiber::YieldToHold();
```

本质上是：

1. 把当前协程重新提交到目标 Scheduler。
2. 当前协程让出执行权。
3. 之后由目标 Scheduler 的某个线程恢复它。

`SchedulerSwitcher` 是 RAII 封装：

```cpp
SchedulerSwitcher::SchedulerSwitcher(Scheduler* target) {
    m_caller = Scheduler::GetThis();
    if (target) {
        target->switchTo();
    }
}

SchedulerSwitcher::~SchedulerSwitcher() {
    if (m_caller) {
        m_caller->switchTo();
    }
}
```

它的意图是：进入作用域时切到目标 Scheduler，离开作用域时切回原来的 Scheduler。

### 用测试用例串起来理解

`tests/test_scheduler.cc`：

```cpp
void test_fiber() {
    static int s_count = 5;
    sleep(1);
    if (--s_count >= 0) {
        sylar::Scheduler::GetThis()->schedule(&test_fiber, sylar::GetThreadId());
    }
}

int main() {
    sylar::Scheduler sc(3, false, "test");
    sc.start();
    sleep(2);
    sc.schedule(&test_fiber);
    sc.stop();
}
```

流程如下：

1. `Scheduler sc(3, false, "test")`：创建调度器，不使用主线程。
2. `sc.start()`：创建 3 个工作线程，这 3 个线程进入 `run()`，没有任务时跑 `idle()`。
3. `sc.schedule(&test_fiber)`：主线程把函数指针包装成 `FiberAndThread` 放进 `m_fibers`。
4. 某个工作线程从队列取到任务，把 `test_fiber` 包装成 `cb_fiber`，然后 `swapIn()` 执行。
5. `test_fiber` 内部再次 `schedule(&test_fiber, sylar::GetThreadId())`，并指定当前线程 id，所以后续几次会尽量回到同一个线程执行。
6. `sc.stop()` 请求停止，但会等待 `m_fibers` 清空、`m_activeThreadCount == 0` 后才 join 线程返回。

### Scheduler 模块一句话总结

Scheduler 的本质是一个协程版线程池：外部通过 `schedule()` 提交 Fiber 或 callback，内部多个 OS 线程在 `run()` 里循环抢任务，抢到后用 `swapIn()` 切入协程执行；协程结束就销毁，协程主动让出则根据状态决定重新入队或挂起等待外部事件。

## Hook

hook 了一些常用的底层函数，例如把 `read`, `write` 重写为非阻塞，这样在框架代码中使用的 read 和 write 就是 hook 修改之后的函数而不是 glibc 原生函数

### 实现原理

体现在 `CMakeList` 中：

```cmake
set(LIBS
"-Wl,--whole-archive"
sylar
"-Wl,--no-whole-archive"
dl
pthread
tbb
yaml-cpp
jsoncpp
${ZLIB_LIBRARIES}
${OPENSSL_LIBRARIES}
protobuf
event
)

#sylar自定义宏
sylar_add_executable(bin_sylar "sylar/main.cc" sylar "${LIBS}")
```

- **`--whole-archive` 的作用**
  - 在 Linux 链接过程中，链接器（ld）是非常“懒惰”的。**普通链接逻辑**：如果你链接 `sylar` 库，链接器会扫描你的 `main.cc`。如果 `main.cc` 里没直接用到 `read`、`write`（或者用的是系统库里的），链接器就会认为 `libsylar.a` 里的这些同名 Hook 函数是“无用的”，从而 **丢弃它们**，不打进最终的可执行文件里
  - 它告诉链接器：“**别管有没有人用，把 `sylar` 库里所有的符号（包括那些 Hook 系统函数的同名函数）全部强制打进二进制包里**”，不这样做就不能完整 Hook

- **`sylar`**：这是核心库，里面包含了你定义的 `sleep`、`read`、`write` 等 Hook 函数。
  - **`-Wl,--no-whole-archive`**：在引入 hook 模块之后必须调用，**非常重要，必须关闭这个模式**。否则，后续的 `dl`、`pthread` 等系统库也会被尝试完整打入。这不仅会导致二进制文件巨大，还会引发大量符号冲突（甚至导致链接失败）。

### 函数签名定义

```cpp
namespace sylar {
/**
 * @brief 当前线程是否hook
 */
bool is_hook_enable();
/**
 * @brief 设置当前线程的hook状态
 */
void set_hook_enable(bool flag);
}  // namespace sylar

extern "C" {

// sleep
typedef unsigned int (*sleep_fun)(unsigned int seconds);
extern sleep_fun sleep_f;

typedef int (*usleep_fun)(useconds_t usec);
extern usleep_fun usleep_f;

typedef int (*nanosleep_fun)(const struct timespec *req, struct timespec *rem);
extern nanosleep_fun nanosleep_f;

// socket
typedef int (*socket_fun)(int domain, int type, int protocol);
extern socket_fun socket_f;

typedef int (*connect_fun)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
extern connect_fun connect_f;

typedef int (*accept_fun)(int s, struct sockaddr *addr, socklen_t *addrlen);
extern accept_fun accept_f;

// read
typedef ssize_t (*read_fun)(int fd, void *buf, size_t count);
extern read_fun read_f;

typedef ssize_t (*readv_fun)(int fd, const struct iovec *iov, int iovcnt);
extern readv_fun readv_f;

typedef ssize_t (*recv_fun)(int sockfd, void *buf, size_t len, int flags);
extern recv_fun recv_f;

typedef ssize_t (*recvfrom_fun)(int sockfd, void *buf, size_t len, int flags,
                                struct sockaddr *src_addr, socklen_t *addrlen);
extern recvfrom_fun recvfrom_f;

typedef ssize_t (*recvmsg_fun)(int sockfd, struct msghdr *msg, int flags);
extern recvmsg_fun recvmsg_f;

// write
typedef ssize_t (*write_fun)(int fd, const void *buf, size_t count);
extern write_fun write_f;

typedef ssize_t (*writev_fun)(int fd, const struct iovec *iov, int iovcnt);
extern writev_fun writev_f;

typedef ssize_t (*send_fun)(int s, const void *msg, size_t len, int flags);
extern send_fun send_f;

typedef ssize_t (*sendto_fun)(int s, const void *msg, size_t len, int flags,
                              const struct sockaddr *to, socklen_t tolen);
extern sendto_fun sendto_f;

typedef ssize_t (*sendmsg_fun)(int s, const struct msghdr *msg, int flags);
extern sendmsg_fun sendmsg_f;

typedef int (*close_fun)(int fd);
extern close_fun close_f;

//
typedef int (*fcntl_fun)(int fd, int cmd, ... /* arg */);
extern fcntl_fun fcntl_f;

typedef int (*ioctl_fun)(int d, unsigned long int request, ...);
extern ioctl_fun ioctl_f;

typedef int (*getsockopt_fun)(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
extern getsockopt_fun getsockopt_f;

typedef int (*setsockopt_fun)(int sockfd, int level, int optname, const void *optval,
                              socklen_t optlen);
extern setsockopt_fun setsockopt_f;

extern int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen,
                                uint64_t timeout_ms);
}
```

## IO Manager

IO Manager 本质是：

**class IOManager : public Scheduler, public TimerManager**  

也就是它把三件事合到一起：

  - Scheduler：负责线程池和协程调度。
  - TimerManager：负责定时器，按最近超时时间驱动 epoll_wait。
  - epoll：负责 socket READ/WRITE 事件。

  核心文件是 `sylar/iomanager.h:20` 和 `sylar/iomanager.cc:102`

  它内部最重要的数据结构是 `FdContext`：

```c
  struct FdContext {
      EventContext read;
      EventContext write;
      int fd;
      Event events;
      MutexType mutex;
  };
```

每个 fd 对应一个 `FdContext`，里面分别保存 `READ` 和 `WRITE` 的上下文

EventContext 里存三样东西：事件触发后要回到哪个 Scheduler、恢复哪个 Fiber，或者执行哪个 callback。

### 工作流程

1. 构造 IOManager 时创建 epoll fd，再创建一对 pipe 作为唤醒管道。
     位置：`sylar/iomanager.cc:102`
  2. `addEvent(fd, READ/WRITE)` 时：
     
      - 找到或扩容 `m_fdContexts`
      - 调用 `epoll_ctl ADD/MOD`
      - 保存当前协程或 callback
     - `m_pendingEventCount++`
     
     位置：`sylar/iomanager.cc:150`
  3. 没有普通调度任务时，`Scheduler::run()` 会进入 `idle()`
     `IOManager::idle()` 不是空转，而是在里面调用 `epoll_wait`
     位置：`sylar/iomanager.cc:327`
  4. `epoll_wait` 返回后：
     
      - 如果是 pipe 事件，说明只是被 `tickle()` 唤醒。
      - 如果是 socket 事件，转换成 `READ/WRITE`。
      - 从 epoll 中删除或修改剩余事件。
     - 调用 `triggerEvent()`，把原来的协程或 callback 重新丢回调度器。
     
     位置：`sylar/iomanager.cc:367`
  5. 定时器也在同一个 `idle()` 中处理：
      - `getNextTimer()` 给 epoll_wait 提供 timeout。
      - `listExpiredCb()` 取出到期回调。
      - 到期回调重新 `schedule()` 到协程调度器。

     位置：`sylar/timer.cc:109`
  6. Hook 和 IOManager 的关系是：阻塞 IO 遇到 EAGAIN 后，注册 READ/WRITE 事件，然后当前协程 `YieldToHold()`。等 epoll 事件回来，IOManager 再恢复这个协程。
     位置：`sylar/hook.cc:93`

> 一句话总结：IOManager 是“**协程调度器 + epoll 事件循环 + 定时器**”的组合，它把阻塞式写法变成协程级非阻塞执行。

### 和 Scheduler 的关系补充

读代码时可以把 Scheduler 和 IOManager 的关系理解成：

- Scheduler 决定“哪个协程在哪个线程上执行”。
- IOManager 决定“什么时候因为 IO 或定时器事件把协程重新放回 Scheduler”。

所以 `Scheduler::run()` 是所有调度线程的主循环，`IOManager::idle()` 是没有普通任务时的事件等待点。业务协程因为 IO 暂停后，不是 Scheduler 主动知道 IO 什么时候完成，而是 IOManager 在 epoll 事件触发后调用 `schedule()`，把原来的协程重新交还给 Scheduler。

