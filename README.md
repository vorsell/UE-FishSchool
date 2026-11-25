# UE-FishSchool

面向十万量级实时鱼群的 Unreal Engine C++ 项目。一个 Manager 维护整群鱼的数据，使用 CPU 并行更新行为，再通过 Instanced Static Mesh（ISM）批量提交实例变换。

这是源码展示仓库。完整 UE 工程、测试关卡、蓝图、第三方模型、材质和 VAT 资源均未包含，仓库不能直接运行。

## 核心实现

| 模块 | 实现 |
| --- | --- |
| 网格 | 均匀网格。各任务独立计数，经前缀和计算写入区间，再将鱼 ID 并行写入连续数组 |
| 仿真 | 位置、朝向分别存储并使用双缓冲。内部格做 27 格局部 KNN，按鱼 ID 分组轮换刷新邻居缓存 |
| 可见性 | CPU 将 Cell 投影到屏幕深度桶，结合距离档位选择 KNN、简化环绕及渲染策略 |
| 玩家交互 | 玩家进入近距离档位后，在局部通道内调整鱼的游动方向 |
| 渲染 | 单 ISM 全量批量提交，以及空间 Chunk ISM 分级提交。VAT 动画与模型 LOD 依赖项目资源 |

## 阅读源码

从 [Manager 的启动和逐帧更新](Source/UEFishSchool/Private/FishSchool/SimulationV2/FishSchoolManagerV2.cpp)读起。各模块的接口位于 [Public/FishSchool/SimulationV2](Source/UEFishSchool/Public/FishSchool/SimulationV2)，实现在 [Private/FishSchool/SimulationV2](Source/UEFishSchool/Private/FishSchool/SimulationV2)。

[架构与调用链](docs/Architecture.md) · [优化过程](docs/OptimizationHistory.md) · [旧 ICO 版本](https://github.com/vorsell/UE-FishSchool/tree/56c2aecbec7aa50962dfdba5b4ccbbf9190a0a06)

## 历史性能记录

早期完整场景在 **RTX 2060、i7-10875H、100000 条鱼**的高压场景中记录到约 **23 FPS → 33 FPS**。这个结果来自当时行为精度、粗可见性与渲染提交策略的综合调整，包含可见鱼数量的变化。V2 是基于 ICO 的模块拆分，本仓库未给出 V2 的 FPS 对照数据。

## 依赖与许可

源码基于 UE 5.8，依赖 Core、CoreUObject、Engine 和 Enhanced Input。接入完整工程时，需要由 UE 生成反射头，并配置输入、场景和网格材质资源。

作者 [Vorsel](https://github.com/vorsell)。源码采用 [PolyForm Noncommercial 1.0.0](LICENSE)，商业使用需另行获得作者授权。
