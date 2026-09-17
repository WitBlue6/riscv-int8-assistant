# 新增 IC 设计源码导读：把外部存储、分块乘加和注意力硬件串起来

依据 2026-09-16 的实际源码编写。本篇从当前系统的数据流开始讲解，不要求你先熟悉 Transformer 的数学推导。

先记住一句话：**CPU 负责安排步骤；SoC 根据地址接通对应模块；乘加模块计算线性层；注意力模块保存历史并计算 QK、AV；外部存储模型提供放不进原 RAM 的参数。**

本文位于 `docs`，讲解的源码位于仓库根目录。当前工作区只保留最新设计，历史实现通过Git查看。没有修改开源 PicoRV32 的 CPU 流水线。

## 1. 先看新增文件，不急着看代码

| 文件 | 可以先怎样理解 | 与 IC 设计的关系 |
|---|---|---|
| [attention.sv](../attention.sv) | 新增的注意力计算设备 | 新数据通路、KV 存储、命令与状态控制 |
| [tiled_fc.sv](../tiled_fc.sv) | 能接着上一次结果继续算的乘加设备 | 在原 INT8 模块上增加跨块累加与有效性检查 |
| [soc.sv](../soc.sv) | 连接 CPU、两台计算设备和存储的线路 | 新地址译码、读数据选择、外部访问等待握手 |
| [main.c](../main.c) | 运行在 RISC-V CPU 上的操作步骤 | 配置与使用硬件的驱动，不是新增电路 |
| [attention_test.cpp](../attention_test.cpp) | 单独给注意力硬件出题并判卷 | 模块级硬件验证 |
| [fc_test.cpp](../fc_test.cpp) | 检查跨块累加和边界条件 | 模块级硬件验证 |
| [test.py](../test.py) | 运行整个 CPU＋硬件系统，核对结果和周期 | 系统级差分验证 |
| [main.cpp](../main.cpp) | 电脑上推进模拟时钟的程序 | 仿真环境，不是在 RISC-V 中执行的固件 |

训练、权重导出和网页是配套软件。本篇会提到它们如何连接硬件，但不把它们算作 Verilog 设计。

## 2. 总连接图：先区分“连线”与“执行先后”

硬件连接关系是：

```text
                        soc.sv
 ┌─────────────────────────────────────────────────────────┐
 │                        PicoRV32 CPU                     │
 │                  地址 / 数据 / 读写请求                  │
 │                            │                            │
 │                      地址译码与读选择                    │
 │       ┌──────────────┬──────┴─────────┬──────────────┐    │
 │       ↓              ↓                ↓              ↓  │
 │   内部 RAM       外部参数存储       tiled_fc       attention│
 │ 程序/工作数据    模拟1 MiB空间      分块线性运算    KV/QK/AV│
 └─────────────────────────────────────────────────────────┘
```

**tiled_fc 与 attention 之间没有直接相连的数据流接口。** CPU 先从一个模块读结果、整理，再写给另一个模块。两个模块虽然共用时钟，也不会自动知道对方做到了哪一步。

比如计算当前 Q：CPU 搬权重、启动 tiled_fc、读取 Q；然后 CPU 把 Q 写到 attention 的输入缓冲区，启动注意力命令。这种方式容易验证，但搬运和控制也会花时间。

地址分配决定请求送到谁：

| CPU 地址范围/位置 | 目标 |
|---|---|
| `0x00000000～0x0003FFFF` | 内部256 KiB RAM |
| `0x01000000～0x010FFFFF` | 外部参数存储模型 |
| `0x20000000～0x2000FFFF` | tiled_fc 乘加模块 |
| `0x21000000～0x2100FFFF` | attention 注意力模块 |
| `0x10000000` | 仿真字符输出 |
| `0x10000004` | 固件退出通知 |
| `0x10000008` | 外部访问完成计数 |

地址只是编号，不是文件路径。CPU 不会把“attention.sv”当文件打开。它发地址，SoC 的逻辑信号将访问送过去。

## 3. 外部参数怎样进入 tiled_fc：跟踪一次实际搬运

### 3.1 权重文件与 CPU 指针之间的关系

[train.py](../train.py) 导出 `external.hex`，里面是全部参数的数字。仿真开始时 soc.sv 用 `$readmemh` 加载到 `external_mem`。

[weights.h](../weights.h) 不再放全部参数数组，而是保存类似这样的定义：

```c
#define wt_q ((const int8_t *)(uintptr_t)0x01002280u)
```

Q 投影权重位于外部区域起点加8832字节，布局来源见 [layout.json](../layout.json)。CPU 执行读取 `wt_q[i]` 的指令时，真的产生 `0x01002280+i` 附近的地址访问。

外部镜像289,728字节，比原来的256 KiB总RAM还大。固件只保存程序、指针及少量表格，所以仍能放进原链接区域。

### 3.2 一次 upload 实际包含两笔不同访问

`main.c::upload()` 核心语句：

```c
MMIO(dst+4*i)=p[i];
```

例如 p 指向外部权重，dst 指向 tiled_fc 的权重区：

```text
第一笔：CPU 从 0x01002280 读取四个权重字节
       → soc.sv 判断属于外部区域
       → 等待外部访问 ready
       → 数据进入 CPU

第二笔：CPU 将这个32位数写到 0x20002000
       → soc.sv 判断属于 tiled_fc
       → tiled_fc 将四个字节放入 weights[0…3]
```

没有一根“external_mem 自动直通加速器”的线路，也没有 DMA。当前是 CPU 读一份、写一份。只有在它们地址及长度满足四字节对齐条件时，这个优化搬运函数才能完整处理；本模型的线性层输入宽度满足条件。

## 4. 新 soc.sv 最重要的变化：CPU 要学会等待

旧系统把 CPU 的 ready 直接接到 valid，基本不增加访存等待。现在外部存储要等，新逻辑是：

```verilog
wire ready = valid && (!is_external || (ext_pending && ext_wait==0));
```

逐项读：

- `valid`：CPU 正在发起请求。
- `!is_external`：不是外部访问，沿用原来的快速响应。
- `ext_pending`：外部请求已经登记，正在处理。
- `ext_wait==0`：等待计数结束，可以完成此次访问。

假设 CPU 发起一次外部读取，`ext_latency=3`，且期间按协议保持请求和地址稳定：

| 上升沿 | 沿前情况 | 沿后状态 |
|---|---|---|
| T0 | valid=1，pending=0，ready=0 | 登记请求，pending=1，wait=3 |
| T1 | wait=3，ready=0 | wait=2 |
| T2 | wait=2，ready=0 | wait=1 |
| T3 | wait=1，ready=0 | wait=0，组合 ready 随后变成1 |
| T4 | valid=1、ready=1 | CPU接收数据，pending清0，访问计数加1 |

所以配置3不是承诺“从请求到接收恰好只有3个时钟沿”。它表示登记后倒数的等待量。配置0也经过登记和握手；相较配置0，配置3在每次访问中多等3周期。

**访存 ready 与加速器 done 完全不同。** CPU 写启动命令的那笔访问可以立即完成，但加速器要再运行很多周期才能 done。CPU 后续通过新的读访问查询 done。

这里的外部存储是仿真行为模型。没有实现真实 DDR 的引脚、刷新、训练或突发协议。源码没有外部区域写入分支；若 CPU 错误地往该区域写，当前模型会完成访问而不更新参数，且没有总线写错误响应。当前固件仅使用读取，不能把它当成完善的只读访问保护机制。

## 5. tiled_fc.sv：原乘加器怎样学会“分多次算一个结果”

### 5.1 哪些东西没变

仍然是四路 INT8 乘法，求和后进入一个 INT32 累加器。单次任务最多256个输入、32个输出，权重缓冲区仍为8192字节。

“能算2048维输入”不是一次装入2048项，而是让同一个输出经历8次任务：

```text
2048个输入 → 8块，每块256个输入
64个输出  → 2组，每组32个输出
因此这个线性层一共启动 8×2=16 次。
```

### 5.2 新增的两个状态

```verilog
reg partial_valid;
reg [31:0] partial_rows;
```

`partial_valid` 表示结果数组里有一次已完成、未被裁剪的结果，可作为下一输入块的起点；`partial_rows` 记录它有多少行。

它们是硬件能检查的条件。硬件并不知道这些结果属于模型哪一层、全局哪32行。CPU 仍必须保证不会将不相关层的结果错误接起来；不能因为有 valid 就认为任何调度错误都能发现。

### 5.3 模式寄存器现在包含两个位

变量名仍叫 `relu_en`，但 bit1 已新增“从上一块结果开始”的含义：

| 模式 | 二进制 | 起始值 | 当前块结束后 |
|---|---|---|---|
| 0 | 00 | 偏置 | 保留原始INT32 |
| 1 | 01 | 偏置 | 裁剪至0～127 |
| 2 | 10 | 上一次 results | 保留原始INT32 |
| 3 | 11 | 上一次 results | 裁剪至0～127 |

启动第一行时：

```verilog
accumulator <= relu_en[1] ? results[0] : biases[0];
```

换到下一行时：

```verilog
accumulator <= relu_en[1] ? results[row+1] : biases[row+1];
```

意思是：普通任务从偏置开始；追加任务从相应行的旧结果开始。这些旧结果就留在硬件里，不用 CPU 读回再写作偏置。

### 5.4 一个六个输入、分两块的手算例子

为了算得简单，这里人为按4+2分块；真实固件默认按最多256项切块，且当前优化搬运路径不处理这种两字节尾部。RTL本身支持，独立测试通过不同写入方式覆盖。

```text
输入 x = [1,2,3,4,5,6]
权重 w = [1,1,1,1,1,1]
偏置 b = 10
完整答案 = 10+1+2+3+4+5+6 = 31
```

| 任务 | 本次配置 | 初始值 | 本次增加 | 留在 results[0] |
|---|---|---:|---:|---:|
| 第一块 | n=4，m=1，模式0 | 偏置10 | 1+2+3+4=10 | 20 |
| 第二块 | n=2，m=1，模式2 | 旧结果20 | 5+6=11 | 31 |

如果第二块仍用模式0，就会重新从偏置10开始，得到21，丢掉前一块结果。

如果每块结束都裁剪，可能提前丢失信息。例如第一块和为200、第二块增加-100，正确最终值100；若第一块先裁剪成127，后面只能得到27。量化除法也存在类似问题，因此本模型在完整行累加结束后才除以64。

### 5.5 CPU 怎样安排二维分块

在 `main.c::linear()` 中：

```c
for (int base=0; base<m; base+=32) {         // 全局输出，每次最多32行
    for (int first=0; first<n; first+=256) { // 全局输入，每次最多256列
        // 上传这一小块输入、权重
        // first==0：模式0；其余输入块：模式2
        // 启动并等待当前块完成
    }
    // 到这里才读取完整结果、进行除以64的尺度转换
}
```

原始代码的两个地址表达式很重要：

```c
upload(A+0x2000+r*width, w+(base+r)*n+first, width);
```

左边是硬件缓冲区，保存紧凑的小矩阵，一行只有 width 个数；右边是外部完整矩阵，一行有 n 个数。

例如大矩阵 n=2048，现在算输出组 base=32、输入块 first=256，组内行 r=1：右边选择全局第33行，从输入256开始；左边放到本地第1行。本地 row 编号会从0重新开始，所以 CPU 必须做好这次地址转换。

## 6. attention.sv：先把三个命令认清楚

这个模块不是“拿到汉字就自动做注意力”。它接收 CPU 已经计算好的数字向量，通过三个独立命令完成工作：

| 命令 | 做什么 | 返回什么 |
|---|---|---|
| APPEND，写1 | 保存当前 K、V 到历史缓存 | 更新有效词元数 |
| QK，写2 | 当前 Q 与每个历史 K 做点积 | 每个历史词元一个相关分数 |
| AV，写3 | 用 CPU 给出的权重对历史 V 加权求和 | 每个通道一个INT32和 |

**注意：attention 的命令值不是 tiled_fc 的位标志。** 同样写3，向 `0x21000000` 写是“执行AV”；向 `0x20000000` 写是“清标志并启动乘加”。地址不同，解释协议也不同。

### 6.1 dim、count、row、col 分别表示什么

| 变量 | APPEND时 | QK时 | AV时 |
|---|---|---|---|
| dim | 新向量有几维 | 每次点积的长度 | 输出向量有几维 |
| count | 已缓存词元数/本次追加的槽位 | 要计算几个历史词元分数 | 每个输出累加几个历史V |
| row | 不使用 | 当前历史词元编号 | 当前输出通道编号 |
| col | 向量组起点 | 向量组起点 | 历史词元组起点 |

同样两个循环索引，QK和AV的“行列”含义不同。这是最容易读错的位置。

### 6.2 缓存编号为什么乘64，不是乘dim

```verilog
keys[count*64+col+k] <= new_k[col+k];
```

每个词元固定预留64格，即使实际dim=4，第0个词元仍占槽位0～63，第1个从64开始。这样存储布局固定，地址生成简单。32个词元×64格=2048项，K和V各一组INT8数组。

`count` 在追加结束之前不变，因此每一组新数据都写到同一个新槽位；整条向量复制完成后，count才加1。复位或清历史只将count归零，不逐项擦除缓存；没有计入有效历史的数据不会被运算使用。

### 6.3 QK：固定一个历史词元，走遍向量各维

```verilog
dot_product = $signed(query[col+k])
            * $signed(keys[row*64+col+k]);
```

比如row=2、col=4、k=1：拿query[5]，乘第2个历史词元的第5维，即keys[133]。一行算完形成scores[row]，再换下一个历史词元。

### 6.4 AV：固定一个输出通道，走遍历史词元

```verilog
weighted_product = $signed({1'b0,probability[col+k]})
                 * $signed(values[(col+k)*64+row]);
```

比如row=5、col=0、k=2：取第2个历史词元的权重，乘那个词元V的第5维。这里col+k选择词元，row选择通道，与QK不同。

虽然数组名叫probability，当前实际写入的是Softmax的**未归一化指数权重**，还不是总和为1的概率。硬件算完加权和后，CPU再除以权重总和。

权重是无符号16位，可能为32768。若直接把16位模式当成有符号数，会解释成-32768。前面补一个0再做 `$signed`，就用17位正确表示正的32768。乘INT8后用25位保存乘积，再符号扩展到32位累加。

## 7. 将一次注意力运算完整手算出来

下面人为选dim=4、已有两个词元，使数字容易核对。它不是模型的实际权重，但遵循当前硬件接口与固件的指数表规则。

```text
Q  = [1,2,-1,0]
K0 = [1,0, 1,0]      V0 = [4,0,-4,8]
K1 = [0,1, 2,1]      V1 = [0,8, 4,0]
```

CPU先设置DIM=4，并两次写new_k/new_v、发APPEND。因为每次只有4维，分别用一个复制周期完成。随后count=2。

**第一段：CPU写Q，发QK命令。**

```text
score0 = 1×1 + 2×0 + (-1)×1 + 0×0 = 0
score1 = 1×0 + 2×1 + (-1)×2 + 0×1 = 0
```

QK第一周期算row=0，第二周期算row=1，随后DONE=1。CPU分别读 `AT+0x2000`、`AT+0x2004`，得到两个0。

**第二段：CPU做Softmax的查表部分。** 两个分数相同，减去最大值后都为0，查表得到两个32768，总和den=65536。固件将它们打包：

```c
MMIO(AT+0x1300) = 0x80008000u;  // 低16位和高16位都是32768
```

attention内部逐字节写入后，`probability[0]=32768`、`probability[1]=32768`。

**第三段：CPU发AV命令。** 每次算一个输出通道，有两个历史词元参与，四路中的后两路屏蔽：

| 核心周期 | row | 运算 | sums[row] |
|---|---:|---|---:|
| 1 | 0 | 32768×4 + 32768×0 | 131072 |
| 2 | 1 | 32768×0 + 32768×8 | 262144 |
| 3 | 2 | 32768×(-4) + 32768×4 | 0 |
| 4 | 3 | 32768×8 + 32768×0 | 262144 |

**第四段：CPU读四个和，除以den。** 得到 `[2,4,0,4]`，也就是两个V向量等权平均。这个结果再通过CPU搬给tiled_fc，执行注意力输出投影。

这一小例子同时展示了QK、CPU Softmax、无符号权重乘有符号V、AV结果读回。特别注意：AV返回131072等整数和，不是直接返回2、4、0、4。

## 8. 把整次生成串起来：以处理一个词元为单位

`main.c::forward(token,pos,logits)` 是主线，推荐打开它对照以下步骤。

1. CPU从外部区域读取该字符的嵌入和位置向量，相加得到64维x。
2. CPU归一化x；调用三次linear，通过tiled_fc分别得到Q、K、V，并裁剪为INT8范围。
3. CPU仅上传当前K/V到attention，发APPEND；历史保留在硬件中。
4. CPU上传当前Q，发QK，读出对全部已有词元的分数。
5. CPU计算最大值、指数查表和权重总和；上传权重。
6. CPU发AV，读回加权和，除以权重总和得到ctx。
7. CPU调用linear完成O投影，加残差，再进行前馈层up、激活、down及残差。
8. CPU归一化后调用词表投影，取得37个候选分数，选择下一个字符。

新字符重新进入forward时，pos加1、KV历史增加一条；不是每次从头上传所有历史K/V。单层实现不需要区分不同层的KV缓存；如果以后加多层，必须为每层分别安排历史空间或调度，不能共用现在这一份数据而不做区分。

实际64维模型，每个词元的线性任务数量如下：

| 运算 | 全局输入→输出 | 硬件任务数 |
|---|---|---:|
| Q、K、V、O，各一次 | 64→64 | 每个2次，共8次 |
| up | 64→2048 | 64次 |
| down | 2048→64 | 8个输入块×2个输出组=16次 |
| head | 64→37 | 2次 |
| 合计 | | 90次 |

每个词元还有APPEND、QK、AV三次注意力任务。`你好：` 加BOS有4个提示词元，生成“你”“好”“！”后分别再次forward，最后预测EOS就停止，共7次forward。因此报告看到630次线性任务、21次注意力任务和7项KV缓存，不是随机计数。

## 9. 接口速查：这两个基地址不要混用

`A=0x20000000` 是tiled_fc；`AT=0x21000000` 是attention。

| 偏移 | tiled_fc | attention |
|---|---|---|
| 0x0000 | CTRL位命令 | 枚举命令0/1/2/3/4 |
| 0x0004 | BUSY/DONE/ERROR | BUSY/DONE/ERROR |
| 0x0008 | 本块输入数n | 向量维数dim |
| 0x000C | 本块输出数m | 有效KV数，只读 |
| 0x0010 | 模式0～3 | 核心周期，只读 |
| 0x0014 | 核心周期 | 完成任务数 |
| 0x0018 | 完成任务数 | 未实现读，默认0 |
| 0x1000起 | 输入INT8 | 当前Q |
| 0x1100起 | 非输入缓冲区 | 当前待追加K |
| 0x1200起 | 未实现 | 当前待追加V |
| 0x1300起 | 未实现 | 未归一化注意力权重 |
| 0x2000起 | 权重INT8，写 | QK分数，读 |
| 0x3000起 | 仍在权重区，写 | AV和，读 |
| 0x4000起 | 偏置INT32 | 未实现 |
| 0x5000起 | 输出INT32，读 | 未实现 |

表中“起”表示该区起始位置，不代表之后所有地址都有效。精确范围见下面代码中的上下界。输入缓冲区大多没有读回接口，读到默认0并不表示之前没写成功。

## 10. 怎样对照代码阅读后面的逐句注释

`always @*` 部分描述“当前输入与状态如何形成计算结果”；`always @(posedge clk)` 部分描述“下一次时钟沿保存什么”。`<=` 的右侧和条件使用沿前状态，不能当成C程序里立即改值的赋值。

比如完成当前行时，既能写 `results[row]`，也能将累加器装成下一行初值，二者在同一个上升沿完成。固定四次的组合for循环不代表循环四个时钟，而是描述四组运算关系。

下面完整保留三份RTL，仅在非空代码行前加中文解释。`【L数字】` 对应当前原文件行号。行号以后可能变化，以信号名和语句为准。文档不修改实际运行代码。

## 11. soc.sv 完整逐句注释

```systemverilog
//【L1】声明整个系统顶层；这个模块把 CPU、存储和加速器连接起来。
module soc (
//【L2】输入同一个时钟，以及低电平有效的同步复位。
    input wire clk, input wire resetn,
//【L3】仿真宿主写 RAM 的专用端口；host_word 是32位字的编号，不是字节地址。
    input wire host_we, input wire [15:0] host_word, input wire [31:0] host_data,
//【L4】字符输出通知及其8位内容，供 main.cpp 收集。
    output reg console_valid, output reg [7:0] console_data,
//【L5】固件结束标志及退出码，供仿真宿主判断结束。
    output reg finished, output reg [31:0] exit_code,
//【L6】将 PicoRV32 的 trap 信号引出，便于发现执行异常。
    output wire trap,
//【L7】这里只引出 tiled_fc 的状态；attention 状态仍可通过 MMIO 查询。
    output wire accel_busy, output wire accel_done, output wire accel_error
//【L8】结束端口或实例连接列表。
);
//【L9】65536个32位字，合计256 KiB内部 RAM。
    reg [31:0] ram[0:65535];
//【L10】注释说明新增的外部参数区为1 MiB，可设置等待量。
    // External read-only weight memory model: 1 MiB, configurable request latency.
//【L11】明确这只是访问行为模型，不是真实 DDR 控制器。
    // This models transactions, not a physical DDR controller.
//【L12】262144个32位字形成外部存储数组。
    reg [31:0] external_mem[0:262143];
//【L13】仿真用字符串，保存权重文件路径。
    string external_path;
//【L14】仿真用整数，保存配置的等待周期数。
    integer ext_latency;
//【L15】记录当前是否已有外部访问等待完成。
    reg ext_pending;
//【L16】ext_wait 是倒计时；external_reads 是完成的访问次数。
    reg [31:0] ext_wait, external_reads;
//【L17】保存固件镜像文件路径。
    string firmware_path;
//【L18】仿真初始化块，在运行开始时装载镜像；不是 CPU 执行文件读取。
    initial begin
//【L19】先把内部 RAM 清零。
        for (integer i=0;i<65536;i=i+1) ram[i]=0;
//【L20】从命令行 +firmware=... 取路径，未指定时用默认文件。
        if (!$value$plusargs("firmware=%s",firmware_path)) firmware_path="build/firmware.hex";
//【L21】将十六进制固件镜像加载到内部 RAM。
        $readmemh(firmware_path,ram);
//【L22】先把外部存储数组清零。
        for(integer i=0;i<262144;i=i+1)external_mem[i]=0;
//【L23】读取 +weights=...；未指定则使用 external.hex。
        if(!$value$plusargs("weights=%s",external_path))external_path="external.hex";
//【L24】将权重镜像加载到外部数组。
        $readmemh(external_path,external_mem);
//【L25】读取 +ext_latency=...；默认额外等待量为3。
        if(!$value$plusargs("ext_latency=%d",ext_latency))ext_latency=3;
//【L26】参数越界时终止仿真，避免无意义的配置。
        if(ext_latency<0 || ext_latency>100)$fatal(1,"bad external latency");
//【L27】结束当前 begin 分支或循环块。
    end
//【L28】CPU 总线请求有效信号；instr 指示是否为取指，这里没有另行使用。
    wire valid, instr;
//【L29】CPU 输出的字节地址和32位写数据。
    wire [31:0] address, write_data;
//【L30】四个字节写使能；全0表示读访问。
    wire [3:0] strobe;
//【L31】tiled_fc 给出的组合读数据。
    wire [31:0] accel_rdata;
//【L32】地址小于0x40000时选择内部 RAM。
    wire is_ram = address < 32'h00040000;
//【L33】高16位为0x2000时选择乘加器，低16位留作模块内偏移。
    wire is_accel = address[31:16] == 16'h2000;
//【L34】高16位为0x2100时选择注意力模块。
    wire is_attention = address[31:16] == 16'h2100;
//【L35】判断是否落在新增的1 MiB外部区域内。
    wire is_external = address>=32'h01000000 && address<32'h01100000;
//【L36】注意力模块给出的组合读数据。
    wire [31:0] attention_rdata;
//【L37】非外部访问立即响应；外部访问必须已登记且倒计时为0才响应。
    wire ready = valid && (!is_external || (ext_pending && ext_wait==0));
//【L38】读数据多路选择：RAM 用字节地址除以4索引32位字。
    wire [31:0] read_data = is_ram ? ram[address[17:2]] :
//【L39】否则依次选择乘加器、注意力模块的读数据。
        is_accel ? accel_rdata : is_attention ? attention_rdata :
//【L40】外部区域用低20位定位字；计数地址返回访问数；其余返回0。区域基址按1 MiB对齐，故可直接取这些位。
        is_external ? external_mem[address[19:2]] : address==32'h10000008 ? external_reads : 0;
//【L41】实例化已有 PicoRV32，复位从地址0取指，设置初始栈顶。
    picorv32 #(.PROGADDR_RESET(0),.STACKADDR(32'h0002fff0),
//【L42】启用 CPU 乘法、快速乘法和除法指令支持；与外挂 INT8 加速器是两回事。
        .ENABLE_MUL(1),.ENABLE_FAST_MUL(1),.ENABLE_DIV(1),
//【L43】启用周期计数，禁用中断；cpu 是实例名。
        .ENABLE_COUNTERS(1),.ENABLE_COUNTERS64(1),.ENABLE_IRQ(0)) cpu (
//【L44】连接 CPU 时钟、复位和异常输出。
        .clk(clk),.resetn(resetn),.trap(trap),
//【L45】把请求与完成握手连接至 SoC；CPU 在 ready 到来前保持请求。
        .mem_valid(valid),.mem_instr(instr),.mem_ready(ready),
//【L46】接通地址、写数据、写掩码和返回数据。
        .mem_addr(address),.mem_wdata(write_data),.mem_wstrb(strobe),.mem_rdata(read_data),
//【L47】未接自定义 PCPI 协处理器或 IRQ，将相应输入固定为0。
        .pcpi_wr(1'b0),.pcpi_rd(32'b0),.pcpi_wait(1'b0),.pcpi_ready(1'b0),.irq(32'b0)
//【L48】结束端口或实例连接列表。
    );
//【L49】实例化新增分块乘加器，实例名仍为 accelerator。
    tiled_fc accelerator (
//【L50】只有属于它且握手完成的请求才传入；模块只看低16位偏移。
        .clk(clk),.resetn(resetn),.req(valid && ready && is_accel),.addr(address[15:0]),
//【L51】接入写数据与字节掩码，接出读数据。
        .wdata(write_data),.wstrb(strobe),.rdata(accel_rdata),
//【L52】将乘加器状态连到顶层输出。
        .busy(accel_busy),.done(accel_done),.error(accel_error)
//【L53】结束端口或实例连接列表。
    );
//【L54】实例化 attention；它与 tiled_fc 是两个独立模块。
    attention attention_unit (
//【L55】以注意力地址范围产生请求，传入模块内偏移。
        .clk(clk),.resetn(resetn),.req(valid && ready && is_attention),.addr(address[15:0]),
//【L56】连接同一 CPU 总线的写数据与掩码，以及本模块的读数据。
        .wdata(write_data),.wstrb(strobe),.rdata(attention_rdata),
//【L57】状态引脚不单独引出；内部状态寄存器仍可供 CPU 读取。
        .busy(),.done(),.error()
//【L58】结束端口或实例连接列表。
    );
//【L59】时序逻辑：在每个时钟上升沿更新状态。
    always @(posedge clk) begin
//【L60】默认取消输出通知；有新的字符写入时，下方会重新置1，形成一次通知。
        console_valid<=0;
//【L61】复位分支。
        if (!resetn) begin
//【L62】清除结束标志、退出码和字符数据。
            finished<=0;exit_code<=0;console_data<=0;
//【L63】清除外部访问状态、等待计数和访问计数。
            ext_pending<=0;ext_wait<=0;external_reads<=0;
//【L64】正常运行分支。
        end else begin
//【L65】外部请求刚到且没有在途请求时，登记并装入等待量。
            if(!ext_pending && valid && is_external)begin ext_pending<=1;ext_wait<=ext_latency;end
//【L66】已有请求时转入等待/完成处理。
            else if(ext_pending)begin
//【L67】计数非0，每个时钟减1。
                if(ext_wait!=0)ext_wait<=ext_wait-1;
//【L68】计数为0且 CPU 请求仍有效，完成握手、清 pending、访问数加1。
                else if(valid)begin ext_pending<=0;external_reads<=external_reads+1;end
//【L69】结束当前 begin 分支或循环块。
            end
//【L70】结束当前 begin 分支或循环块。
        end
//【L71】仿真宿主仅能在复位期间通过该端口写 RAM，常用于准备输入。
        if (host_we && !resetn) ram[host_word]<=host_data;
//【L72】CPU 写入必须同时满足脱离复位、请求有效、握手成功、有写字节。
        if (resetn && valid && ready && |strobe) begin
//【L73】若目标为内部 RAM，执行字节写入。
            if (is_ram)
//【L74】只修改 wstrb 对应的字节；8*j+:8 表示从第8*j位开始取8位。
                for (integer j=0;j<4;j=j+1) if(strobe[j]) ram[address[17:2]][8*j+:8]<=write_data[8*j+:8];
//【L75】写字符输出地址时，给宿主一个通知，并保留低8位字符。
            if (address==32'h10000000) begin console_valid<=1;console_data<=write_data[7:0];end
//【L76】写退出地址时，记录结束及退出码。
            if (address==32'h10000004) begin finished<=1;exit_code<=write_data;end
//【L77】结束当前 begin 分支或循环块。
        end
//【L78】结束当前 begin 分支或循环块。
    end
//【L79】结束模块定义。
endmodule
```

## 12. tiled_fc.sv 完整逐句注释

```systemverilog
//【L1】四路有符号 INT8 乘加，INT32 累加，CPU 经 MMIO 操作本地缓冲区。
// Four signed INT8 MAC lanes, INT32 accumulator, memory-mapped local buffers.
//【L2】声明分块线性层模块。
module tiled_fc (
//【L3】共用时钟与低有效复位。
    input wire clk, input wire resetn,
//【L4】req 表示一次有效访问；addr 是模块内部16位偏移。
    input wire req, input wire [15:0] addr,
//【L5】32位写数据；四位掩码分别选择四个字节。
    input wire [31:0] wdata, input wire [3:0] wstrb,
//【L6】返回 CPU 的组合读数据；reg 关键字本身不意味着这里一定有触发器。
    output reg [31:0] rdata,
//【L7】运算进行中、完成、错误三种状态。
    output reg busy, output reg done, output reg error
//【L8】结束端口或实例连接列表。
);
//【L9】最多256个有符号 INT8 输入。
    reg signed [7:0] inputs [0:255];
//【L10】最多8192个INT8权重，按本块的行排列。
    reg signed [7:0] weights [0:8191];
//【L11】最多32个INT32偏置。
    reg signed [31:0] biases [0:31];
//【L12】最多32个INT32结果，也用来保留跨块部分和。
    reg signed [31:0] results [0:31];
//【L13】n、m 是本次任务输入和输出数；relu_en 现在实际是两位模式；另有周期与任务计数。
    reg [31:0] n, m, relu_en, cycles, completed;
//【L14】row 为本地输出行；col 为该行当前输入组起点。
    reg [31:0] col, row;
//【L15】当前行跨周期的部分和，保存在寄存器中。
    reg signed [31:0] accumulator;
//【L16】本周期四路和，以及加上旧累加器后的新和。
    reg signed [31:0] lane_sum, next_acc;
//【L17】一次 INT8×INT8 的16位有符号乘积。
    reg signed [15:0] product;
//【L18】j 用于逐字节写循环；lane、wi 在当前源码中未使用。
    integer lane, j, wi;
//【L19】新增：结果是否可以被下一块继续累加。
    reg partial_valid;
//【L20】新增：上次结果有多少行，防止不同输出行数直接接续。
    reg [31:0] partial_rows;

//【L22】组合逻辑，无须等下一个时钟就会随输入和状态变化。
    always @* begin
//【L23】先给临时值赋0，避免未覆盖路径留下旧值。
        lane_sum = 0; product = 0;
//【L24】固定四次组合循环描述四路运算，不是顺序执行四个周期。
        for (integer k=0; k<4; k=k+1) begin
//【L25】仅计算有效行和有效列；最后不足四项时自动屏蔽多余路。
            if (busy && col+k < n && row < m) begin
//【L26】输入第col+k项，乘本地第row行相同列的权重；本地行宽是n。
                product = $signed(inputs[col+k]) * $signed(weights[row*n+col+k]);
//【L27】将16位乘积的符号位复制16次，扩展成32位后加入四路和。
                lane_sum = lane_sum + {{16{product[15]}}, product};
//【L28】结束当前 begin 分支或循环块。
            end
//【L29】结束当前 begin 分支或循环块。
        end
//【L30】本周期新部分和 = 已保存部分和 + 本周期四路乘积和。
        next_acc = accumulator + lane_sum;
//【L31】读地址没有匹配项时返回0。
        rdata = 0;
//【L32】根据模块内部地址选择读数据。
        case (addr)
//【L33】状态位：bit0忙、bit1完成、bit2错误。
            16'h0004: rdata = {29'b0,error,done,busy};
//【L34】读取本次输入数。
            16'h0008: rdata = n;
//【L35】读取本次输出数。
            16'h000c: rdata = m;
//【L36】读取模式0～3；名字保留历史命名。
            16'h0010: rdata = relu_en;
//【L37】读取最近任务实际运算周期数。
            16'h0014: rdata = cycles;
//【L38】读取完成任务数，不是模型层数。
            16'h0018: rdata = completed;
//【L39】非控制寄存器地址进一步判断结果区。
            default: begin
//【L40】结果区128字节，共32个32位数。
                if (addr >= 16'h5000 && addr < 16'h5080)
//【L41】减去结果区起点再除以4，选中对应行。
                    rdata = results[(addr-16'h5000)>>2];
//【L42】结束当前 begin 分支或循环块。
            end
//【L43】结束地址选择。
        endcase
//【L44】结束当前 begin 分支或循环块。
    end

//【L46】在时钟上升沿保存状态。
    always @(posedge clk) begin
//【L47】复位时进入初始化。
        if (!resetn) begin
//【L48】清除忙、完成和错误。
            busy <= 0; done <= 0; error <= 0;
//【L49】清除配置。
            n <= 0; m <= 0; relu_en <= 0;
//【L50】清除当前遍历位置和累加器。
            row <= 0; col <= 0; accumulator <= 0;
//【L51】清除统计计数。
            cycles <= 0; completed <= 0;
//【L52】复位后不允许直接接续旧结果；数组本身无需在此逐项擦除。
            partial_valid <= 0; partial_rows <= 0;
//【L53】脱离复位后正常工作。
        end else begin
//【L54】沿前处于busy，才执行一个运算步骤。
            if (busy) begin
//【L55】核心忙周期数加1，不包括 CPU 上传和轮询时间。
                cycles <= cycles + 1;
//【L56】这一组是当前行最后一组时，准备保存结果。
                if (col+4 >= n) begin
//【L57】模式bit0决定是否裁剪结果。
                    if (relu_en[0])
//【L58】裁剪为0～127，兼有非负激活及上限饱和；不是普通无限上界的ReLU。
                        results[row] <= next_acc < 0 ? 0 : (next_acc > 127 ? 127 : next_acc);
//【L59】不裁剪时保留原始INT32和，供软件量化或下一块继续累加。
                    else results[row] <= next_acc;
//【L60】下一行从第0列开始。
                    col <= 0;
//【L61】如果当前行也是最后一行，整个任务完成。
                    if (row+1 >= m) begin
//【L62】撤销busy、置done、任务计数加1。
                        busy <= 0; done <= 1; completed <= completed+1;
//【L63】只有未裁剪的完整结果可续算，同时记住行数。
                        partial_valid <= !relu_en[0]; partial_rows <= m;
//【L64】若还有下一行，切换到下一行。
                    end else begin
//【L65】行索引加1。
                        row <= row+1;
//【L66】续算模式读取下一行旧结果；普通模式读取下一行偏置。右侧使用沿前值。
                        accumulator <= relu_en[1] ? results[row+1] : biases[row+1];
//【L67】结束当前 begin 分支或循环块。
                    end
//【L68】当前行尚未算完时继续累加。
                end else begin
//【L69】前进四列，将本周期新和存入累加器。
                    col <= col+4; accumulator <= next_acc;
//【L70】结束当前 begin 分支或循环块。
                end
//【L71】结束当前 begin 分支或循环块。
            end
//【L72】有有效请求且至少一个字节要写时处理写入。
            if (req && |wstrb) begin
//【L73】注释：执行期间的写请求不能改动当前计算。
                // Writes during execution never alter the active computation.
//【L74】忙时拒绝所有写入并置错误，但不停止原任务。
                if (busy) error <= 1;
//【L75】小于0x1000作为控制寄存器区域处理。
                else if (addr < 16'h1000) begin
//【L76】控制寄存器要求整32位写；部分字节写置错误。
                    if (wstrb != 4'b1111) error <= 1;
//【L77】按寄存器偏移分发写操作。
                    else case (addr)
//【L78】控制寄存器采用位标志，可同时设置多个位。
                        16'h0000: begin
//【L79】bit1清除done和error；固件写3会同时清标志及启动。
                            if (wdata[1]) begin done<=0; error<=0; end
//【L80】bit2只清任务统计，不清除缓存或partial_valid。
                            if (wdata[2]) completed<=0;
//【L81】bit0发起任务。
                            if (wdata[0]) begin
//【L82】检查输入/输出数及模式范围。
                                if (n==0 || n>256 || m==0 || m>32 || relu_en>3 ||
//【L83】续算还必须有有效旧结果，且旧结果行数与当前m相同。
                                    (relu_en[1] && (!partial_valid || partial_rows!=m))) error<=1;
//【L84】参数合法才启动。
                                else begin
//【L85】置busy，清done与当前任务周期计数；此句没有自动清error。
                                    busy<=1; done<=0; cycles<=0;
//【L86】从本地第0行、第0列开始。
                                    row<=0; col<=0;
//【L87】新任务第一行以旧结果或偏置为初值，这是跨输入块累加的关键。
                                    accumulator<=relu_en[1] ? results[0] : biases[0];
//【L88】消耗有效标志，防止任务未完成时被视作可续算；结果数组的值此时仍保留。
                                    partial_valid<=0;
//【L89】结束当前 begin 分支或循环块。
                                end
//【L90】结束当前 begin 分支或循环块。
                            end
//【L91】结束当前 begin 分支或循环块。
                        end
//【L92】设置本次输入块的宽度n。
                        16'h0008: n<=wdata;
//【L93】设置本次输出组的行数m。
                        16'h000c: m<=wdata;
//【L94】设置模式，合法性在启动时检查。
                        16'h0010: relu_en<=wdata;
//【L95】未实现的控制寄存器写入置错误。
                        default: error<=1;
//【L96】结束地址选择。
                    endcase
//【L97】输入缓冲区地址范围为256字节。
                end else if (addr >= 16'h1000 && addr < 16'h1100) begin
//【L98】遍历32位写数据的四个字节位置。
                    for (j=0;j<4;j=j+1)
//【L99】按掩码写对应输入字节；地址低两位清零，再加字节编号。
                        if (wstrb[j]) inputs[(addr-16'h1000 & 16'hfffc)+j] <= wdata[8*j+:8];
//【L100】权重缓冲区覆盖8192字节。
                end else if (addr >= 16'h2000 && addr < 16'h4000) begin
//【L101】遍历四个字节通道。
                    for (j=0;j<4;j=j+1)
//【L102】将使能字节写入本地权重矩阵。
                        if (wstrb[j]) weights[(addr-16'h2000 & 16'hfffc)+j] <= wdata[8*j+:8];
//【L103】偏置缓冲区覆盖128字节。
                end else if (addr >= 16'h4000 && addr < 16'h4080) begin
//【L104】遍历四个字节通道。
                    for (j=0;j<4;j=j+1)
//【L105】先选第几个32位偏置，再更新它的指定字节。
                        if (wstrb[j]) biases[(addr-16'h4000)>>2][8*j+:8] <= wdata[8*j+:8];
//【L106】其余写地址报错，例如直接写结果区。
                end else error<=1;
//【L107】结束当前 begin 分支或循环块。
            end
//【L108】结束当前 begin 分支或循环块。
        end
//【L109】结束当前 begin 分支或循环块。
    end
//【L110】结束模块定义。
endmodule
```

## 13. attention.sv 完整逐句注释

```systemverilog
//【L1】单头因果注意力数据通路；Softmax和归一化仍由 CPU 处理。
// Single-head causal attention datapath. CPU performs softmax/normalization.
//【L2】每个词元追加一次K/V，随后QK与AV复用硬件历史。
// Append K/V once per token, then QK and AV commands reuse hardware-resident history.
//【L3】声明注意力模块。
module attention (
//【L4】输入时钟、低有效复位、一次有效总线访问请求。
    input wire clk, resetn, req,
//【L5】模块内部字节偏移，不含0x2100这个基址高位。
    input wire [15:0] addr,
//【L6】CPU 的32位写数据。
    input wire [31:0] wdata,
//【L7】四位字节写使能；全0为读。
    input wire [3:0] wstrb,
//【L8】组合读数据，交给SoC选择后返回CPU。
    output reg [31:0] rdata,
//【L9】忙、完成、错误状态。
    output reg busy, done, error
//【L10】结束端口或实例连接列表。
);
//【L11】当前Q，以及尚未追加到历史的新K、新V，各最多64维INT8。
    reg signed [7:0] query[0:63], new_k[0:63], new_v[0:63];
//【L12】K/V历史各2048字节，布局为32个词元×每词元64格。
    reg signed [7:0] keys[0:2047], values[0:2047];
//【L13】32个无符号16位指数权重；实际尚未归一化。
    reg [15:0] probability[0:31];
//【L14】QK最多32个INT32分数；AV最多64个INT32输出和。
    reg signed [31:0] scores[0:31], sums[0:63];
//【L15】dim是维数，count是有效历史条数，另有任务周期及完成统计。
    reg [31:0] dim, count, cycles, completed;
//【L16】行列计数器；QK和AV对它们的解释不同，参见第6节。
    reg [31:0] row, col;
//【L17】当前运算类型用两位记录：追加、QK或AV。
    reg [1:0] op; // 1 append, 2 QK, 3 AV
//【L18】跨周期累加器、四路组合和、下一周期待存和。
    reg signed [31:0] acc, lane_sum, next_acc;
//【L19】QK的8位有符号乘8位有符号，乘积16位。
    reg signed [15:0] dot_product;
//【L20】AV以17位有符号表示非负权重，再乘8位有符号V，乘积25位。
    reg signed [24:0] weighted_product;
//【L21】用于写缓冲区的字节循环变量。
    integer j;
//【L22】组合运算与读数据选择。
    always @* begin
//【L23】默认清零各临时量，避免保留旧值。
        lane_sum=0; dot_product=0; weighted_product=0;
//【L24】四路并行处理当前这一组数据。
        for(integer k=0;k<4;k=k+1) begin
//【L25】QK模式且当前维度存在时，该路参与。
            if(busy && op==2 && col+k<dim) begin
//【L26】Q的一个维度乘第row个历史K的同一维度；历史槽位跨度固定64。
                dot_product=$signed(query[col+k])*$signed(keys[row*64+col+k]);
//【L27】16位乘积符号扩展到32位，加入四路和。
                lane_sum=lane_sum+{{16{dot_product[15]}},dot_product};
//【L28】结束当前 begin 分支或循环块。
            end
//【L29】AV模式且该历史词元有效时，该路参与。
            if(busy && op==3 && col+k<count) begin
//【L30】注释提醒先补零，不能把无符号权重误读为负数。
                // Unsigned 16-bit probability is zero-extended before signed multiply.
//【L31】第col+k个词元的权重，乘它V向量的第row维。
                weighted_product=$signed({1'b0,probability[col+k]})*$signed(values[(col+k)*64+row]);
//【L32】将25位乘积符号扩展为32位，加入四路和。
                lane_sum=lane_sum+{{7{weighted_product[24]}},weighted_product};
//【L33】结束当前 begin 分支或循环块。
            end
//【L34】结束当前 begin 分支或循环块。
        end
//【L35】当前行旧累加器加本组四路和。
        next_acc=acc+lane_sum;
//【L36】未匹配读地址默认返回0。
        rdata=0;
//【L37】按内部偏移选择返回值。
        case(addr)
//【L38】状态位低三位依次为忙、完成、错误。
            16'h0004:rdata={29'b0,error,done,busy};
//【L39】返回配置的向量维数。
            16'h0008:rdata=dim;
//【L40】返回已经完整追加的历史条数。
            16'h000c:rdata=count;
//【L41】返回最近任务核心忙周期。
            16'h0010:rdata=cycles;
//【L42】返回完成任务数，APPEND也算一个任务。
            16'h0014:rdata=completed;
//【L43】除控制寄存器外，还可以读两块结果数组。
            default:begin
//【L44】QK结果区128字节，按32位字返回scores。
                if(addr>=16'h2000 && addr<16'h2080)rdata=scores[(addr-16'h2000)>>2];
//【L45】AV结果区256字节，按32位字返回sums。
                if(addr>=16'h3000 && addr<16'h3100)rdata=sums[(addr-16'h3000)>>2];
//【L46】结束当前 begin 分支或循环块。
            end
//【L47】结束地址选择。
        endcase
//【L48】结束当前 begin 分支或循环块。
    end
//【L49】时钟上升沿更新状态及存储数组。
    always @(posedge clk) begin
//【L50】低电平复位。
        if(!resetn)begin
//【L51】清状态、配置与统计；count为0代表历史为空。
            busy<=0;done<=0;error<=0;dim<=0;count<=0;cycles<=0;completed<=0;
//【L52】清位置、操作类型与累加器。
            row<=0;col<=0;op<=0;acc<=0;
//【L53】正常运行分支。
        end else begin
//【L54】如果沿前正在执行，就推进一步。
            if(busy)begin
//【L55】每个忙周期计数加1。
                cycles<=cycles+1;
//【L56】操作1为追加，仅复制K/V，不做点积。
                if(op==1)begin
//【L57】一次复制最多四个维度；不足四维屏蔽尾部。
                    for(integer k=0;k<4;k=k+1)if(col+k<dim)begin
//【L58】将新K的本组数据放到count指定的新历史槽位。
                        keys[count*64+col+k]<=new_k[col+k];
//【L59】同时复制对应V；不是先复制完整K再复制V。
                        values[count*64+col+k]<=new_v[col+k];
//【L60】结束当前 begin 分支或循环块。
                    end
//【L61】最后一组复制完成，历史数加1，完成标志和统计同时更新。
                    if(col+4>=dim)begin count<=count+1;busy<=0;done<=1;completed<=completed+1;end
//【L62】未复制完则维度位置前进4。
                    else col<=col+4;
//【L63】QK遍历dim个维度；AV遍历count个词元。判断这一组是否为当前行末尾。
                end else if(col+4 >= (op==2 ? dim : count))begin
//【L64】QK将本行和写scores；AV将本行和写sums。
                    if(op==2)scores[row]<=next_acc;else sums[row]<=next_acc;
//【L65】每行都是独立求和，换行时列位置及累加器清0。
                    col<=0;acc<=0;
//【L66】QK有count行，AV有dim行；最后一行结束后置完成。
                    if(row+1 >= (op==2 ? count : dim))begin busy<=0;done<=1;completed<=completed+1;end
//【L67】否则进入下一行。
                    else row<=row+1;
//【L68】当前行未结束则前进四项并保存部分和。
                end else begin col<=col+4;acc<=next_acc;end
//【L69】结束当前 begin 分支或循环块。
            end
//【L70】只有有效写访问才进入寄存器/缓冲区写逻辑。
            if(req && |wstrb)begin
//【L71】忙时拒绝写并置错误，原任务仍继续执行。
                if(busy)error<=1;
//【L72】控制地址区。
                else if(addr<16'h1000)begin
//【L73】十进制15等于二进制1111，要求控制寄存器整字写入。
                    if(wstrb!=15)error<=1;
//【L74】根据控制偏移分发。
                    else case(addr)
//【L75】偏移0为命令寄存器；wdata在这里作为完整命令值解释。
                        0:begin
//【L76】0清标志，4清历史，1/2/3启动三种任务。
                            // command 0 clear flags; 4 clear KV history; 1/2/3 start.
//【L77】命令0仅清done/error，不清历史和任务统计。
                            if(wdata==0)begin done<=0;error<=0;end
//【L78】命令4清有效历史数与任务统计；不擦数组、不清dim或cycles。
                            else if(wdata==4)begin count<=0;done<=0;error<=0;completed<=0;end
//【L79】非法命令或维数为0/大于64时不能启动。
                            else if(wdata>3 || dim==0 || dim>64 ||
//【L80】历史满32条时不能再追加；历史为空时不能做QK/AV。
                                    (wdata==1 && count==32) || (wdata!=1 && count==0))error<=1;
//【L81】合法任务启动：清旧完成及错误，记住操作，清该任务计数和遍历状态。
                            else begin busy<=1;done<=0;error<=0;op<=wdata[1:0];cycles<=0;row<=0;col<=0;acc<=0;end
//【L82】结束当前 begin 分支或循环块。
                        end
//【L83】偏移8写dim。
                        8:begin
//【L84】注释说明：有历史时改维数会破坏对缓存的解释。
                            // Changing dimension with cached vectors would reinterpret history.
//【L85】只要count非0就拒绝写dim，即使写入值与原值一样。
                            if(count!=0)error<=1;else dim<=wdata;
//【L86】结束当前 begin 分支或循环块。
                        end
//【L87】其余控制地址不支持写，置错误。
                        default:error<=1;
//【L88】结束地址选择。
                    endcase
//【L89】Q输入区64字节。
                end else if(addr>=16'h1000 && addr<16'h1040)begin
//【L90】地址对齐后按掩码拆成四个字节写Q。
                    for(j=0;j<4;j=j+1)if(wstrb[j])query[((addr-16'h1000)&16'hfffc)+j]<=wdata[8*j+:8];
//【L91】新K输入区64字节。
                end else if(addr>=16'h1100 && addr<16'h1140)begin
//【L92】将CPU写数据拆成字节写入new_k，尚未进入历史。
                    for(j=0;j<4;j=j+1)if(wstrb[j])new_k[((addr-16'h1100)&16'hfffc)+j]<=wdata[8*j+:8];
//【L93】新V输入区64字节。
                end else if(addr>=16'h1200 && addr<16'h1240)begin
//【L94】将CPU写数据拆成字节写入new_v。
                    for(j=0;j<4;j=j+1)if(wstrb[j])new_v[((addr-16'h1200)&16'hfffc)+j]<=wdata[8*j+:8];
//【L95】指数权重区64字节，可容纳32个16位权重。
                end else if(addr>=16'h1300 && addr<16'h1340)begin
//【L96】逐一检查四个字节是否使能。
                    for(j=0;j<4;j=j+1)if(wstrb[j])
//【L97】字节地址除以2选择权重项；j为偶数写低8位、奇数写高8位；一次32位写对应两个权重。
                        probability[(((addr-16'h1300)&16'hfffc)+j)>>1][8*(j%2)+:8]<=wdata[8*j+:8];
//【L98】其余写地址均报错。
                end else error<=1;
//【L99】结束当前 begin 分支或循环块。
            end
//【L100】结束当前 begin 分支或循环块。
        end
//【L101】结束当前 begin 分支或循环块。
    end
//【L102】结束模块定义。
endmodule
```

## 14. main.c 怎样把上面三份电路真正用起来

这里要区分两套“执行”：电脑上的 main.cpp 推动模拟时钟；模拟出来的 PicoRV32 读取固件机器码，执行 main.c 编译后的指令。不是电脑直接执行 main.c 替硬件算答案。

### 14.1 一行 C 怎样变成硬件写请求

```c
#define MMIO(a) (*(volatile uint32_t *)(uintptr_t)(a))
MMIO(A+8)=width;
MMIO(A+12)=rows;
MMIO(A+16)=first?2:0;
MMIO(A)=3;
```

`MMIO` 将一个数字地址视作可读写的32位位置；`volatile` 告诉编译器，这些访问有设备副作用，不能当成无用的普通内存读写删掉。

以上四句依次配置本块输入数、输出数、起始累加模式，最后清标志并启动。编译后的 CPU 存储指令通过 soc.sv 把访问送到 tiled_fc。写命令本身只是在通知设备开始；矩阵结果不会在这句C结束时立即算好。

```c
wait_done(A);
mac_cycles+=MMIO(A+0x14);
```

`wait_done` 反复读取状态的bit1，直到DONE出现。CPU轮询与加速器运算发生在同一段时间里。当前没有中断，也没有在等待期间安排别的模型任务。

驱动中的错误处理还有一个值得读懂的边界：当前 `wait_done` 只在DONE尚未置位时检查ERROR；若读到DONE与ERROR同时为1，会直接退出循环。正常任务验证通过不等于驱动已经覆盖所有错误恢复场景。理解代码时，不要把它当成工业级完善驱动。

### 14.2 三个注意力命令为什么必须分开

以下摘出实际调用中的关键语句，省略上传循环和统计：

```c
MMIO(AT)=1;wait_done(AT);  // 当前K/V准备好后，追加历史
MMIO(AT)=2;wait_done(AT);  // 当前Q准备好后，取得QK分数
// CPU读取dots、计算指数权重、写入AT+0x1300
MMIO(AT)=3;wait_done(AT);  // 使用刚写入的权重计算AV
```

不能把1、2、3连续写入而不等待：前一个任务忙时，后续写入会被拒绝。也不能跳过QK与AV之间的软件步骤，因为AV需要的指数权重尚未产生。

每个新请求开始时，`main()` 先做：

```c
MMIO(AT)=4;
MMIO(AT+8)=DIM;
```

先把有效历史归零，再配置维数。否则之前已有历史时写DIM会报错。这里清历史针对整次新输入；生成同一个回答的下一个字符时不能清，否则会丢掉上下文。

C里也能看到 `keys`、`values` 数组。它们用于软件参考/软件注意力路径，源码还会保存当前K/V；这不改变硬件路径中的事实：attention自身拥有独立历史，后续命令读取自己的缓存。检查版用CPU数组重新计算，才能与硬件结果对照。

## 15. 哪些新文件负责验证，应该怎样阅读

### 15.1 attention_test.cpp：绕过CPU，单独检查注意力电路

先读其中驱动时钟、写寄存器、读寄存器的辅助函数，再读构造输入和比较结果的部分。它在电脑上直接驱动被Verilator转换的attention模块，不需要先启动整个PicoRV32。

这种测试能针对不同dim、历史长度、正负输入及接口边界出题。特别值得关注的是无符号权重与负V相乘、最后不足四路的情况，以及缓存与命令的错误处理。只测试最后生成的汉字，难以定位这些问题。

### 15.2 fc_test.cpp：重点检查“接着上一次结果算”

阅读时沿着“写第一块→启动→写第二块→续算→比较完整矩阵结果”这条线。还要看没有有效部分和、行数不匹配、结果裁剪等条件下的行为。它验证的不只是乘法数值，还包括新增状态能否约束命令。

### 15.3 test.py：把CPU、C程序、两台设备和参数串成系统

系统验证会运行不同固件变体，并与Python参考模型比较生成的logits和token，还会核对中间状态摘要。摘要是排错与一致性检查手段，并不等于逐元素的数学证明。checked固件额外使用C重算线性层与注意力原始输出，逐项比较。

已有 [verification_report.json](../verification_report.json) 记录如下；这是项目现有测试结果，本次编写导读没有重新运行仿真：

| 检查层级 | 已有记录 |
|---|---|
| attention独立测试 | 674个任务，12,162次检查，通过 |
| tiled_fc独立测试 | 306个任务，7,230次检查，通过 |
| 整体生成 | 11个输入用例，报告通过 |
| `你好：` checked固件 | 16,835项线性结果、476项注意力结果比较，match=true |
| 外部等待 | 等待配置0、3、7，输出一致 |

需要自己复跑时，在仓库根目录执行：

```bash
make unit  # 单独运行两份RTL模块测试
make test  # 构建并运行模块及系统验证，更新验证报告
```

## 16. 看懂周期报告：有了新硬件，不代表整机立刻快很多

对已有 `你好：` 用例：

| 配置 | 生成计算的CPU周期数 |
|---|---:|
| 线性层和注意力都使用硬件 | 16,842,555 |
| 线性层使用硬件、注意力由CPU算 | 16,921,043 |
| 都由CPU计算 | 70,088,556 |

整个硬件方案相对纯CPU约4.16倍加速；在已经使用线性硬件的基础上，新增注意力硬件使总周期再减少约0.46%。不能把4.16倍全部归功于attention.sv。

原因在于当前模型上下文很短，前馈层很宽，线性层参数搬运和软件控制占据大量时间。`accelerator_cycles=491568` 只是乘加器内部忙周期；`hardware_linear_cycles=14413007` 还包含CPU上传、查询和取回。四路乘法本身快，不代表每一步喂数据都快。

外部等待量从0增加到7时，总周期差为：

```text
18,812,411 - 15,365,163 = 3,447,248
492,464次外部访问 × 7个额外等待周期 = 3,447,248
```

这也是检查新增SoC等待握手是否真正影响运行的一种直观证据。这里统计的是仿真CPU周期，不是芯片实测频率；电脑运行仿真的墙钟时间又是另一回事。报告计时包含prefill和decode及其搬运、归一化等，排除输入UTF-8解析和最终JSON输出。

## 17. 读完后，你应该能准确说明自己的设计做到了什么

1. **分块线性运算**：扩展原四路INT8模块，以硬件结果数组保存部分和，支持超过256维输入的线性层；CPU负责切块和搬运。
2. **注意力数据通路**：增加QK、AV与KV缓存，用不同遍历方式复用行列控制思路，由CPU完成两者之间的Softmax步骤。
3. **SoC集成**：增加第二个设备的地址译码，并给外部参数访问加入等待握手，让权重大于原内部RAM的模型仍能完成生成。
4. **验证**：结合模块级定向/随机检查、C与Python参考、系统输出及周期对照，检查结果和接口行为。

仍需分清几个边界：PicoRV32来自开源项目；外部存储是仿真模型；没有DMA；没有新增Softmax或RMSNorm电路；模型是单层单头的微型演示，生成正确训练样例不代表通用问答能力。

RTL描述了每组四项的计算行为，但未做综合与布局布线，所以不能仅凭for循环断言最终恰好使用四个物理乘法器、面积多少或最高频率多少。尤其attention的QK和AV写有不同位宽的乘法表达式，是否共享物理运算资源需看综合结果。INT32累加也不是任意长度下无限精度求和；继续扩展维数与上下文时，应重新分析位宽、溢出和缓存容量。

建议第一遍只读第1～9节，把数据走向弄懂；第二遍对照第11～13节看状态更新；第三遍打开main.c和测试文件，追踪一条实际命令。如果能独立回答“这次row到底指词元还是通道”“结果在哪个时钟沿保存”“下一块从哪里拿初值”，就已经读到了这次新增设计的核心。
