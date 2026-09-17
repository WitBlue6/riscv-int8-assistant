# Transformer SoC：注意力硬件、KV 缓存与外部权重分块

基于 PicoRV32 RV32IM、四路 INT8 乘加器和 Verilator 的数字 IC 仿真项目，在 RISC-V 上运行整数 Transformer 并逐字符生成中文。核心功能：

1. **新增注意力 RTL**：QK 点积、注意力权重与 V 的加权累加，以及保存历史 K/V 的硬件缓存。
2. **扩大到 285,312 个参数**：模型权重从独立外部存储仿真模型读取，经 CPU 分块搬入加速器，支持跨输入分块保留部分和。

仍使用开源 PicoRV32，没有自研 CPU 流水线。Softmax、RMSNorm 和生成循环仍在 RISC-V CPU 上执行。外部存储是带等待周期的事务级仿真，不是 DDR 控制器或实际开发板。

## 源码阅读入口

[文档索引](docs/README.md) · [IC 设计逐句导读](docs/新增IC设计源码导读.md)

推荐按 `soc.sv → tiled_fc.sv → attention.sv → main.c` 阅读，并对照导读中的时序表与手算例子。CPU负责调度，SoC负责连接，两个加速器负责数值计算。C固件编译为RISC-V指令，SV描述电路，C++宿主推动仿真时钟。

## 项目结构

| 文件 | 职责 |
|---|---|
| `soc.sv` | 地址译码、CPU与双加速器连接、外部存储等待握手 |
| `tiled_fc.sv` / `attention.sv` | 分块线性乘加 / KV缓存与QK、AV |
| `main.c` | RISC-V裸机固件：分块调度、Softmax、归一化、生成 |
| `main.cpp` | 电脑上的仿真宿主：推进时钟、加载输入、收集输出 |
| `train.py` / `model.py` / `corpus.json` | 训练导出、整数参考、训练语料 |
| `external.hex` / `external.bin` | 同一外部权重布局的镜像文件；仿真加载HEX |
| `weights.h` / `weights.json` / `layout.json` | 固件参数指针与表格、参考权重、镜像地址布局 |
| `attention_test.cpp` / `fc_test.cpp` | 单独驱动两个RTL模块并对照数值与协议 |
| `test.py` / `verification_report.json` | 整体差分验证脚本 / 已有结果 |
| `server.py` / `index.html` | 网页服务与展示，不执行模型推理 |
| `Makefile` / `build/` | 构建与测试入口 / 固件及仿真产物 |

```text
riscv-int8-assistant/
├── soc.sv / tiled_fc.sv / attention.sv  SoC与两个硬件加速器
├── main.c / main.cpp                   CPU固件与仿真宿主
├── firmware/start.S / link.ld          启动代码与链接布局
├── vendor/picorv32/                    开源CPU、来源与许可证
├── scripts/elf_to_hex.py               ELF转固件镜像工具
├── train.py / model.py / corpus.json   训练、整数参考与语料
├── weights.* / external.* / layout.json 参数定义、镜像与布局
├── attention_test.cpp / fc_test.cpp    RTL模块测试
├── test.py / verification_report.json  系统测试与结果
├── server.py / index.html              网页服务与界面
├── docs/                              当前设计导读
├── Makefile / requirements.txt         构建入口与Python依赖
└── build/                             生成的固件、仿真器和波形
```

根目录即唯一构建入口，仿真器输出为 `build/obj/Vsoc`。`firmware/`仅保留启动和链接设施，`scripts/`仅保留镜像转换工具。模型、RTL和测试均为当前版本。

## 运行入口

```bash
cd /Users/kerman/university/pku/work/riscv-int8-assistant
make build
make demo
make unit
make test
make serve
```

直接执行 `make` 默认构建，不会主动启动训练。`make serve` 会先检查构建，再启动网页；也可在已构建后直接执行 `python3 server.py`。

网页地址：<http://127.0.0.1:8767>。按 `Ctrl+C` 关闭服务；端口占用时可用 `python3 server.py --port 8768`。

推荐提示词：`你好：`、`你是谁：`、`早上好：`、`你好：你`。网页会清除首尾空白；词表外字符映射为 UNK。每次请求从空 KV 历史开始，不是持续多轮会话。

首次构建需要 LLVM Clang/LLD、Verilator、本机 C++ 编译器、Make 及 Python NumPy。训练语料见 [corpus.json](corpus.json)，执行 `make train` 可重建模型、权重镜像和头文件。

## 硬件设计职责

| 文件 | 新增设计 | 作用 |
|---|---|---|
| [attention.sv](attention.sv) | 独立命令控制、四路 QK 点积、四路权重×V 累加、K/V 缓存 | 将两项注意力乘加从 CPU 迁到硬件 |
| [tiled_fc.sv](tiled_fc.sv) | 部分和有效状态、行数检查、从上一块结果继续累加的模式 | 同一行可以跨越多个输入块计算 |
| [soc.sv](soc.sv) | 第二个加速器地址译码、外部存储区域、请求等待与完成握手 | CPU 能访问新硬件和更大的权重空间 |
| [main.c](main.c) | 双方向分块、外部参数指针、注意力硬件驱动 | 让新增 RTL 参与真正的生成流程 |

tiled_fc由项目原四路乘加设计扩展而来。CPU复用开源 [PicoRV32](vendor/picorv32/UPSTREAM.md)，许可证见 [COPYING](vendor/picorv32/COPYING)。项目设计集中在加速器、SoC集成、固件和验证，不宣称CPU核心为原创。

## 固件选择与运行链路

| 固件 | 线性层 | QK / AV | 在线逐项参考校验 |
|---|---|---|---|
| `build/fast.hex` | 硬件 | 硬件 | 关闭，用于性能比较 |
| `build/checked.hex` | 硬件 | 硬件 | 开启，用于排错 |
| `build/cpu_attention.hex` | 硬件 | CPU | 关闭，用于单独衡量注意力收益 |
| `build/cpu.hex` | CPU | CPU | 关闭，纯CPU基线 |

网页默认checked；`make demo`使用fast。`match=null`代表没有启用在线逐项校验，不代表该次校验成功或失败。系统测试另外与Python参考比较。

```text
电脑训练/导出 → external.hex + weights.h + 参考权重
C交叉编译 → RISC-V固件HEX
SV + PicoRV32 + main.cpp → Verilator仿真器Vsoc
Vsoc加载固件和外部镜像 → CPU搬权重、调度加速器 → JSON输出
```

完整一次前向由CPU调度：嵌入与归一化 → tiled_fc计算Q/K/V → attention追加KV并计算QK → CPU查表Softmax → attention计算AV → CPU除以权重总和 → tiled_fc完成O、up、down、head投影 → CPU选下一个字符。

## 一、注意力硬件怎样工作

以生成第 t 个词元为例，CPU 先使用线性加速器得到当前 Q、K、V，各为 64 个 INT8 数。

```text
CPU 写入当前 K、V → APPEND 命令 → 保存到硬件缓存的第 t 个位置
CPU 写入当前 Q   → QK 命令     → 与已有 t+1 个 K 分别点积
CPU 读回点积分数 → Softmax     → 得到未归一化的指数权重
CPU 写入权重    → AV 命令     → 对历史 V 加权求和
CPU 读回 INT32 和 → 除以权重总和 → 得到注意力结果
```

历史 K/V 不需要每次从 CPU 重新上传。缓存固定按每个词元64个位置存放，最多32个词元，K 和 V 合计4096字节。每次 APPEND 只复制新词元的数据，计数加一。

QK 使用四路有符号 INT8×INT8；AV 使用四路无符号16位权重×有符号INT8。AV 必须先把权重零扩展成17位有符号正数，再参与有符号乘法，避免把32768、65535误当负数。输出为 INT32 累加结果。

Softmax 没有新增 RTL：CPU 使用指数查表，并在读回 AV 结果后做整数除法。RMSNorm 也仍由 CPU 使用整数平方根完成。

### 注意力寄存器表

基地址为 `0x21000000`，以下列出内部偏移。控制寄存器使用对齐的完整32位写；数据缓冲区支持字节使能。

| 偏移 | 用途 |
|---|---|
| `0x0000` | 命令：0清标志，1追加KV，2计算QK，3计算AV，4清除KV历史和任务计数 |
| `0x0004` | 状态：bit0 BUSY、bit1 DONE、bit2 ERROR |
| `0x0008` | DIM，合法值1～64；历史非空时禁止修改 |
| `0x000C` | 已缓存词元数，只读 |
| `0x0010` | 最近命令的核心周期，只读 |
| `0x0014` | 已完成命令数，只读 |
| `0x1000～0x103F` | 当前 Q，64字节 |
| `0x1100～0x113F` | 等待追加的 K，64字节 |
| `0x1200～0x123F` | 等待追加的 V，64字节 |
| `0x1300～0x133F` | 最多32个无符号16位注意力权重 |
| `0x2000～0x207F` | 最多32个 QK 分数，只读 |
| `0x3000～0x30FF` | 最多64个 AV 加权和，只读 |

开始命令后硬件自行推进，CPU 轮询 DONE。DONE 保持到新命令或清除。忙碌期间写入被拒绝并置 ERROR，但当前计算继续；最后一个周期同时到来的写入也按沿前 BUSY 拒绝。

复位为低有效同步复位，会清除控制状态与有效历史计数，但不逐字节清空数据数组。count=0 使旧缓存不可作为历史访问；驱动需写全本次使用的向量和权重。缓存满32项后 APPEND 被拒绝，没有滑动窗口或环形覆盖。

核心周期公式：

```text
APPEND = ceil(DIM/4)
QK     = 历史长度 × ceil(DIM/4)
AV     = DIM × ceil(历史长度/4)
```

当前 DIM=64；这些周期只包括命令启动之后的计算/复制，不含 MMIO 上传、状态查询和结果读回。

## 二、跨输入分块为什么需要修改 RTL

大模型前馈层包含 `64→2048→64`。其中第二个矩阵每个输出需要2048个输入，超过原加速器一次256个输入的限制。

现在把它分为8个输入块，每块256项。每组32个输出按如下过程执行：

```text
块0：结果 = 偏置 + 前256项乘积之和
块1：结果 = 上一块结果 + 下一组256项乘积之和
……
块7：结果 = 上一块结果 + 最后一组256项乘积之和
最后才进行尺度转换或激活
```

结果保持在 RTL 的 results 数组，下一次启动时直接作为累加初值。CPU 无需把上一块结果读出来再写成下一块偏置。固件仍需搬运下一块的输入与权重。

原模式寄存器偏移 `0x0010` 扩展为：

| 模式值 | 行为 |
|---:|---|
| 0 | 从偏置开始，不裁剪 |
| 1 | 从偏置开始，最终裁剪到0～127 |
| 2 | 从上一次未裁剪结果继续累加，忽略偏置 |
| 3 | 从上一次未裁剪结果继续累加，当前块完成后裁剪 |

新增 `partial_valid` 和 `partial_rows`：复位后不能直接累加；输出行数必须与上一块一致；上一块若已经裁剪，也不能继续拿它当原始部分和。输入块长度可以变化，因此尾块不必等长。

当前 Transformer 固件使用0与2模式，完整行求和后在 CPU 上按权重尺度64执行向零截断。偏置只在第一块加入，本模型线性层偏置为0；独立测试使用非零偏置检查不会重复加入。

输出方向也分块：例如2048个输出分成64组，每组32个。输入方向分块和输出方向分块可以组合起来，形成大矩阵的二维分块。

RTL 独立测试覆盖不对齐于四路的尾部、257/511/1024/2048输入及37/65输出。当前固件的快速搬运路径要求输入尺寸/行跨度为4的倍数，这个64宽度模型满足；不能把模块测试覆盖范围误称为固件已经支持所有非对齐矩阵。

## 三、外部权重存储具体是什么

模型共有285,312个可学习参数。量化线性权重为INT8，嵌入和位置表为INT16，导出参数镜像占 **289,728字节（约283 KiB）**，确实超过原系统全部256 KiB RAM，更超过固件128 KiB链接区域。

```text
train.py
    ├─ weights.json：Python 参考使用
    ├─ external.bin / external.hex：外部参数镜像
    ├─ layout.json：每个张量的地址、尺寸
    └─ weights.h：指向外部地址的 C 宏、词表和指数表
```

`weights.h` 不再把全部参数声明为固件内的数组，而是例如：

```c
#define wt_q ((const int8_t *)(uintptr_t)0x01002280u)
```

这表示 CPU 从模拟外部地址读取 Q 投影权重。具体地址以生成头文件和 [layout.json](layout.json) 为准。

SoC 的外部区域为 `0x01000000～0x010FFFFF`，共1 MiB，在仿真初始化时加载 external.hex。CPU 运行时发出的读取确实经过新地址译码和握手；宿主没有替 CPU 计算矩阵结果。

参数 `+ext_latency=3` 控制一次外部请求登记后需要等待的计数。即使配置0，也仍经过请求登记和完成握手，不是直接把外部 ready 恒置1。外部请求处于等待期间，CPU 不能完成该访问；内部 RAM/MMIO 保持原有快速响应方式。

本实现只为参数读取使用该区域，不支持更新权重；没有外部写回、DDR训练、刷新、突发传输、DMA或真实存储器时序。`0x10000008` 返回外部访问完成计数，当前正常固件只有读请求。

延迟测试分别运行0、3、7设置，要求生成结果不变，并检查增加的CPU周期与每次访问增加的等待量严格对应。外部镜像与固件分离，固件 ELF 仍只有约18 KiB量级。

## 四、完整 Transformer 的结构和分工

结构为：单层、单头、隐藏维度64、前馈维度2048、上下文32、词表37。它是用于验证存储与硬件扩展的较宽前馈模型，不代表这是面向语言质量的最佳参数配置。

每个输入词元依次执行：

```text
外部读取词嵌入和位置向量
    ↓
CPU RMSNorm
    ↓
线性硬件 Q/K/V
    ↓
注意力硬件追加 KV、计算 QK
    ↓
CPU Softmax
    ↓
注意力硬件计算 AV，CPU归一化
    ↓
线性硬件 O + CPU残差
    ↓
CPU RMSNorm → 线性硬件 up → 激活 → 线性硬件 down → 残差
    ↓
CPU RMSNorm → 线性硬件词表投影 → 选择下一个字符
```

每个词元执行90次线性块任务，共70,224个线性核心周期。词表投影仍为32+5个输出。前馈 down 的跨输入分块在硬件中累加；只有完整矩阵行得到后才量化。

整数推理保持激活尺度32、权重尺度64；注意力缩放使用sqrt(64)=8，因此指数查表桶的除数相应为128。RMS求平方和与整数平方根使用64位中间值，以容纳扩大前馈层后的残差范围。RISC-V 本身仍是32位，编译器把这些整数操作展开为可执行指令，不依赖浮点单元。

## 五、验证与实际结果

[verification_report.json](verification_report.json) 保存本版本完整数据。模块测试：

| 测试 | 任务数 | 检查数 |
|---|---:|---:|
| attention_test.cpp | 674 | 12,162 |
| fc_test.cpp | 306 | 7,230 |

注意力覆盖DIM=1/3/4/7/16/63/64、全部1～32历史长度、随机正负数、-128极值、65535权重、尾部、缓存溢出、空历史、忙碌写入、非法配置、同步复位与重新运行。

乘加覆盖跨输入/输出分块、32位回绕、偏置仅首块加入、最后一次裁剪、无有效部分和启动、行数不一致、裁剪后续算拒绝及复位失效。

系统运行八个训练提示词与三个额外输入；逐项比较每个生成分数、token，以及中间状态/投影哈希与Python参考。哈希用于紧凑对照，不声称等价于无碰撞的全状态证明。checked 固件还在 RISC-V 内部逐个比较线性输出、QK分数和AV和。

`你好：` → `你好！` + EOS，默认外部等待参数3时：

| 模式 | 完整推理周期 |
|---|---:|
| 全部由 RISC-V CPU 计算 | 70,088,556 |
| 线性硬件，注意力由 CPU 计算 | 16,921,043 |
| 线性 + 注意力硬件 | 16,842,555 |
| 硬件并重复执行 CPU 数值校验 | 84,448,184 |

完整加速比约 **4.16倍**。单独注意力硬件带来的总周期减少约 **0.46%**，不是整个模型再加速几倍。这个短上下文模型的大部分时间仍耗在线性计算与参数搬运；不能将新增RTL面积或设计工作量等同于应用提速。

该请求处理7个词元，线性任务630次，注意力任务21次，缓存7组K/V。外部参数访问492,464次。硬件注意力核心周期共1200，线性核心周期共491,568；这些不包含搬运和CPU操作，不能拿它们替代完整推理周期。

| 外部等待参数 | 完整推理周期 |
|---:|---:|
| 0 | 15,365,163 |
| 3 | 16,842,555 |
| 7 | 18,812,411 |

0到7的差值正好是 `492464×7`，同时输出数值一致。这验证了外部等待确实作用于CPU访问，而不是只在说明中声称有延迟。

计时覆盖prefill和decode，包含搬运、归一化、Softmax、硬件控制及跟踪记录，排除UTF-8输入解析和JSON打印。checked额外执行重复CPU计算，不能拿其总时间衡量加速效果。宿主仿真的秒数不是物理芯片推理时间。

当前环境没有完成逻辑综合、布局布线或静态时序分析，不能报告面积、最高频率或功耗。KV数组的多端口访问和乘法器资源映射还需在选定工艺/FPGA目标后评估。

## 六、如何切换模式与观察波形

网页提供fast、checked、cpu_attention、cpu四种模式。终端可直接运行：

```bash
build/obj/Vsoc +firmware=build/fast.hex +weights=external.hex +ext_latency=3 --text '你好：'
build/obj/Vsoc +firmware=build/checked.hex +weights=external.hex +ext_latency=3 --text '你好：'
build/obj/Vsoc +firmware=build/fast.hex +weights=external.hex +ext_latency=7 --text '你好：' --trace build/attention.vcd
```

波形文件可能较大。重点观察 SoC 的外部pending/wait/ready，以及注意力模块的op/count/row/col/busy/done，和tiled_fc的partial_valid/relu_en/results。跟踪深度沿用驱动设置，实际可见层级以生成波形为准。

## 七、能力边界

语料仍是同样的八组短句。扩大参数后成功重现它们，说明存储、分块、注意力硬件和生成流程可运行；不能据此声称新增通用知识或问答能力。未训练提示可能生成无关文本或立刻EOS，这属于模型能力边界。

下一步若追求性能，优先评估参数驻留和批量搬运；若追求设计深度，可以继续将Softmax、归一化硬件化，并完成综合、时序与资源评估。若追求语言能力，则需要扩大词表、丰富语料和更合理的模型训练与评测。这三种目标需要各自的证据。

## 环境与复现

已验证环境为 macOS arm64、Verilator 5.034、LLVM Clang/LLD、Python 3和NumPy。默认Clang路径为Homebrew安装位置，其他环境可显式指定：

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
make PYTHON="$PWD/.venv/bin/python" RVCC=clang LLD=/path/to/ld.lld test
make PYTHON="$PWD/.venv/bin/python" RVCC=clang LLD=/path/to/ld.lld serve
```

把示例中的LLD路径替换为实际位置，Clang必须支持RISC-V目标。附带权重可直接使用；`make train`会覆盖权重、镜像与训练报告。修改C后重编译固件，修改SV后重建仿真器；修改模型后应重新导出并运行验证。四份固件是同一实现的性能/校验对照模式，均保留用于差分测试。
