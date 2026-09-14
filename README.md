# RISC-V INT8 中文指令助手

基于 **PicoRV32 RV32IM + 自研四路 INT8 乘加加速器** 的全 RTL 仿真项目。在浏览器输入中文，仿真中的 RISC-V 执行文本特征提取，加载真实训练权重，分别执行软件与硬件推理，再根据意图调用 C 工具并返回回复。

无需 FPGA 开发板、在线模型服务或 GPU。训练使用本机 NumPy；网页后端只传输输入和仿真输出，不代替 RISC-V 完成推理。

## 快速运行

当前电脑已经完成构建与验证：

```sh
cd /Users/kerman/university/pku/work/riscv-int8-assistant
make serve
```

浏览器打开 **http://127.0.0.1:8765**。按 `Ctrl+C` 关闭服务。

可试：`你好`、`你能做什么`、`帮我算一下12加30`、`查看运行状态`、`介绍一下你的模型`、`清零统计`。

```sh
make demo    # 终端输出固件产生的 JSON
make test    # 加速器随机/边界测试 + 完整 SoC 三方差分测试 + 搬运优化对照
make train   # 固定种子重新训练和量化，导出模型、C 权重头文件及分类报告
make trace   # 为“你好”生成 build/inference.vcd，可用 GTKWave 查看
```

每条网页请求启动一个新的仿真 SoC：可重复比较相同初始状态下的性能，不保留跨请求对话记忆。清零命令真实写入硬件计数寄存器，页面显示该请求内工具执行前后的值。

## 实测结果

详细机器可读报告见 [系统验证](docs/verification_results.json) 和 [模型评估](model/training_report.json)。以下结果来自当前电脑构建，换编译器或修改实现后应重新运行测试。

| 指标 | 结果 |
| --- | ---: |
| 模型结构 | 256 → 32 → 6 |
| INT8 权重 / INT32 偏置 | 8,384 / 152 字节 |
| 软件两层推理 | 253,300 CPU 周期 |
| 硬件路径，优化前逐字节拼装 | 189,038 CPU 周期 |
| 硬件路径，32 位对齐搬运 | 49,866 CPU 周期 |
| 相对纯软件的推理加速 | **5.08 倍** |
| 硬件路径搬运优化收益 | **3.79 倍** |
| 两层乘加核心计算 | 2,096 周期 |
| 加速器验证 | 387 组任务、9,182 项动态检查通过 |
| 系统验证 | 70 个用例通过，另验证输入长度拒绝和基线对照 |
| 自建测试集分类正确 | 36 / 36，FP 浮点与 INT8 均如此 |
| 范围外测试输入拒识 | 11 / 12 |

**计时口径：**同一 PicoRV32 配置开启硬件快速乘法，RAM 为仿真零等待。软件路径包含两层推理；硬件路径包含全部输入和权重搬运、寄存器操作、完成轮询、激活转换及结果读回。两者均排除文本特征提取、工具处理和显示。2,096 是加速器内部计算周期，不能用它替代 49,866 来宣称端到端加速比。网页还单列文本特征周期与电脑运行仿真所用的毫秒数。

没有实际 FPGA 上板、时序收敛或 ASIC 流片结果，不声称芯片工作频率、面积或功耗。

## 项目结构

```text
rtl/int8_accel.sv       四路 INT8 MAC、INT32 累加、截断激活、MMIO 控制和局部缓存
rtl/soc.sv              PicoRV32、256 KiB RAM、输入信箱和仿真输出接口
firmware/main.c        特征提取、CPU 推理、加速器驱动、逐层自检、工具和 JSON 输出
firmware/start.S       RISC-V 启动、BSS 清零、栈初始化
firmware/model_weights.h  训练后导出的实际权重与量化偏置
model/dataset.json     人工编写、显式划分的训练/验证/测试语料
model/weights.json     Python 整数参考模型使用的权重
scripts/train.py       NumPy MLP 训练与 INT8 量化
scripts/model_common.py 特征和整数计算规范
tests/accel_test.cpp   独立加速器随机、极值、尾部和控制边界测试
tests/system_test.py  Python ↔ RISC-V 软件 ↔ RTL 的差分验证
sim/main.cpp           Verilator 宿主，加载输入和捕获固件输出
scripts/server.py     标准库 HTTP 服务，调用真实 RTL 仿真
web/index.html        中文交互、周期图、激活图与寄存器状态
vendor/picorv32/       固定版本原始 CPU 和 ISC 许可证
docs/                 架构、结果、简历条目与面试说明
```

## 环境与可复现性

已验证环境：macOS arm64，Verilator 5.034，LLVM Clang + LLD，Python 3 + NumPy。Icarus Verilog 并非此工作流必需。

其他机器需要：Verilator、C++ 编译器、Make、支持 RISC-V 的 Clang/LLD、Python 和 NumPy。没有 NumPy 时先创建虚拟环境并安装：

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
make PYTHON=.venv/bin/python RVCC=clang LLD=/path/to/ld.lld test
```

模型权重已附带，正常构建不需要重新训练。没有 RISC-V GCC 也可以通过 LLVM 编译裸机 RV32IM ELF。`RVCC` 可显式指定 LLVM Clang 路径；`LLD` 指向 LLD 可执行文件。

可选浏览器测试需要 Node.js、Playwright/Playwright Core 和 Chromium 浏览器。设置 `PLAYWRIGHT_MODULE` 为模块路径、`BROWSER_EXECUTABLE` 为浏览器路径后运行 `make ui-test`；测试使用独立的 8766 端口。

## 能力范围

这是六类中文意图识别加工具调用，回复由 C 固件模板组织，不是通用语言模型。计算工具识别两个非负十进制整数（每个不超过 100000）和单一加减乘除，除法取整数商；拒绝除零、过大乘积、负输入和多个操作数。

语料由项目人工编写：训练 132 条、验证 18 条、测试 36 条，范围外校准 6 条、范围外测试 12 条。训练过程检查归一化输入不跨集合重复；验证集用于选择拒识阈值，测试集只报告结果。该规模只说明演示任务可行，不能推广为中文理解能力。当前测试中的“推荐一本小说”会被误接受为帮助意图，这一局限保留在报告中。

CPU 复用 [PicoRV32](https://github.com/YosysHQ/picorv32)，固定提交与许可证见 [UPSTREAM.md](vendor/picorv32/UPSTREAM.md)。新开发内容集中于加速器、SoC 集成、固件、模型和验证，不宣称 CPU 核心为原创设计。
