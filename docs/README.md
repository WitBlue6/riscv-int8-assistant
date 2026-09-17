# 当前项目文档

[项目总览与运行方法](../README.md) · [IC设计源码导读](新增IC设计源码导读.md) · [验证报告](../verification_report.json)

当前工作区只保留最新 Transformer SoC，历史版本见Git记录。

建议先阅读项目README的运行流程，再读源码导读第1～9节的连接、握手与算例，最后对照第11～13节的三份完整RTL注释。导读中“原设计”“新增”等用语说明设计演进，不代表工作区还有其他可运行版本。

| 关注点 | 源码入口 |
|---|---|
| CPU怎样访问存储和设备 | [soc.sv](../soc.sv) |
| 大矩阵如何分块续算 | [tiled_fc.sv](../tiled_fc.sv) |
| KV缓存及QK、AV | [attention.sv](../attention.sv) |
| 软件如何调度硬件 | [main.c](../main.c) |
| 模块独立验证 | [attention_test.cpp](../attention_test.cpp)、[fc_test.cpp](../fc_test.cpp) |
| 完整生成与等待扫描 | [test.py](../test.py) |

在仓库根目录运行 `make unit` 或 `make test` 复核结果；报告中的CPU周期与仿真宿主耗时是不同指标。
