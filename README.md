# UE-FishSchool

面向十万量级实时鱼群的 Unreal Engine C++ 项目，围绕局部邻域查询、并行行为更新和实例渲染提交进行优化。

本仓库展示当前 CPU 仿真与 ISM 渲染的核心实现。未包含完整 UE 工程、关卡、蓝图及第三方模型、材质和 VAT 资源，不能直接运行。

## 技术要点

| 方向 | 实现 |
| --- | --- |
| 数据组织 | 鱼以数组索引表示，位置、朝向等属性分别存储；位置与朝向使用双缓冲 |
| 邻域查询 | 均匀网格内搜索自身及相邻共 27 格，维护局部 KNN；分组轮换刷新邻居缓存 |
| 并行更新 | ParallelFor 分段计算；通过任务私有计数、前缀和及 Scatter 构建连续网格索引 |
| 行为分级 | CPU 粗可见性分类与距离档位；组合 KNN、简化环绕和目标点回归行为 |
| 实例渲染 | 单 ISM 全量提交与 Chunk ISM 分级提交；完整场景通过资源侧 VAT 和模型 LOD 表现动画及几何层级 |

鱼由一个 Manager 集中更新，每条鱼不单独创建 Actor。当前代码同时保留单 ISM 和 Chunk ISM 两条渲染路径。

## 阅读代码

~~~text
Source/UEFishSchool/
├── Public/
│   ├── FishSchool/FishSchoolManagerICO.h
│   └── Stats/FishSchoolStats.h
└── Private/
    └── FishSchool/FishSchoolManagerICO.cpp
~~~

- [FishSchoolManagerICO.h](Source/UEFishSchool/Public/FishSchool/FishSchoolManagerICO.h)：核心数据结构、参数和 Manager 接口。
- [FishSchoolManagerICO.cpp](Source/UEFishSchool/Private/FishSchool/FishSchoolManagerICO.cpp)：从 HandleStartFishSchool、Tick 和 UpdateGrid 阅读初始化与帧更新流程。
- [FishSchoolStats.h](Source/UEFishSchool/Public/Stats/FishSchoolStats.h)：由 Manager 引用的统计结构。

调用链、复杂度与已知问题见 [Architecture.md](docs/Architecture.md)；优化演进见 [OptimizationHistory.md](docs/OptimizationHistory.md)。早期逐鱼 Actor 原型不包含在本仓库中。

## 历史性能记录

开发阶段测试记录：**RTX 2060、i7-10875H、100000 条鱼，高压场景约 23 FPS → 33 FPS**。该数据对应当时完整场景的整体优化，包含行为精度、可见性及渲染提交策略调整；表现随视角、鱼群密度和资源配置变化。

## 代码依赖

实现依赖 Unreal Engine 的 Core、CoreUObject、Engine 和 Enhanced Input 接口。移植到完整 UE 工程时，需要配置模块与输入依赖、生成反射头文件，并接入玩家、摄像机及自有网格/材质资源。

## 许可

源码采用 [PolyForm Noncommercial 1.0.0](LICENSE)，供非商用使用；商业使用需另行获得作者授权。Unreal Engine 及第三方资源遵循各自许可。

作者：[Vorsel](https://github.com/vorsell)。
