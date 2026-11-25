# 技术架构

[AFishSchoolManagerV2](../Source/UEFishSchool/Public/FishSchool/SimulationV2/FishSchoolManagerV2.h) 是接入关卡与蓝图的 Actor。鱼由数组索引标识，由 `FishSchool::V2` 下的网格、可见性、行为和渲染模块统一处理。

## 从初始化到渲染提交

| 阶段 | 入口 | 作用 |
| --- | --- | --- |
| 初始化 | [Manager](../Source/UEFishSchool/Private/FishSchool/SimulationV2/FishSchoolManagerV2.cpp#L129) · `HandleStartFishSchool` | 以 Manager 初始位置为鱼群目标点，初始化网格和双缓冲，尝试用 CSV 恢复鱼群，失败时随机初始化，然后建立渲染实例 |
| 帧级协调 | [Manager](../Source/UEFishSchool/Private/FishSchool/SimulationV2/FishSchoolManagerV2.cpp#L78) · `Tick` | 更新玩家位置和距离档位，准备绕流，交换缓冲后依次执行建网格、可见性分类、行为更新和渲染 |
| 建网格 | [Grid](../Source/UEFishSchool/Private/FishSchool/SimulationV2/FishSchoolGrid.cpp#L62) · `FGrid::Build` | 每个任务统计自己那段鱼的 Cell 归属，经前缀和分配写入区间，再并行写入鱼 ID |
| 可见性 | [Visibility](../Source/UEFishSchool/Private/FishSchool/SimulationV2/FishSchoolVisibility.cpp#L65) · `FVisibility::Update` | 并行更新密集格的私有深度桶，合并最近深度，再分类稀疏格并扩张高精度区域 |
| 鱼群行为 | [Simulation](../Source/UEFishSchool/Private/FishSchool/SimulationV2/FishSchoolSimulation.cpp#L112) · `FSimulation::Step` | 并行处理排序后的鱼 ID，写入下一份状态缓冲 |
| 玩家绕流 | [PlayerFlow](../Source/UEFishSchool/Private/FishSchool/SimulationV2/FishSchoolPlayerFlow.cpp#L179) · `FPlayerFlow::GetCorrection` | 在 Near 和 VeryNear 档位，只对影响通道内的候选鱼修正游动方向 |
| 渲染 | [Renderer](../Source/UEFishSchool/Private/FishSchool/SimulationV2/FishSchoolRenderer.cpp#L71) · `FRenderer::Render` | 从本帧读取缓冲生成实例变换，通过单 ISM 或 Chunk ISM 提交 |
| 数据与统计 | [CSV](../Source/UEFishSchool/Private/FishSchool/SimulationV2/FishSchoolCsv.cpp) · [统计结构](../Source/UEFishSchool/Public/Stats/FishSchoolStats.h) | 读写鱼群状态，导出网格和 Chunk 统计 |

## 并行网格与局部 KNN

`SortedFishIds` 每条鱼占一个槽。计数阶段记录每条鱼的 `CellId`，并将计数写入任务自己的数组。前缀和确定 `CellStarts`，也为同一 Cell 内的各任务划出互不重叠的写入区间。Scatter 复用第一阶段的归属记录，各任务写入鱼 ID。网格外的鱼集中在末尾的桶中。设鱼数为 N、格数为 C、鱼遍历任务数为 T，建表主要工作量为 O(N + C×T)，任务私有计数与游标占用 O(C×T)。

KNN 查询检查当前 Cell 及相邻共 27 格。遍历每格的鱼 ID，读取当前帧位置，按距离平方维护最近的 K 条鱼。候选数为 m 时，插入维护的单次查询开销上界为 O(m×K)。本轮未刷新的分组复用缓存的鱼 ID，用本帧状态重算邻居位置和朝向。

各行为任务从同一份位置和朝向缓冲读取，只写自己负责的鱼在下一缓冲中的槽位。网格、可见性和渲染也读取本轮的输入状态，因此本轮算出的行为结果在下一 Tick 参与渲染。

## 可见性和渲染

CPU 深度桶存储投影后的 Cell 代表深度。密集格先写任务私有槽，合并时保留最近的 Cell 并降低落选格的状态，再用合并后的结果处理稀疏格。当深度判断关闭、仍在预热或拿不到相机投影时，网格内部鱼回退到 KNN。有效分类下，内部格的 `VisibleHigh` 鱼走 KNN，其余采用简化环绕。网格外及边缘鱼向目标点回归。

单 ISM 每帧批量更新全部变换，以零缩放隐藏被筛除的实例。Chunk 路径每帧扫描鱼 ID 并按空间分块。可见块每帧提交，低频块按间隔或状态、鱼数变化提交，隐藏块清理旧槽。可见性无效时，网格外鱼显示在最近的边界块。

单 ISM 写入逐鱼 VAT 时间偏移和播放速度。Chunk 复用根组件的静态网格，尚未同步根 ISM 的材质覆盖，也尚未写入逐实例 VAT 数据。VAT 动画和几何 LOD 由配套资源提供。

## 已知边界

- 网格原点在启动时根据目标点确定，运行时还没有随鱼群移动和旋转的网格。
- Chunk 提交支持降频，仿真行为仍逐 Tick 更新。Chunk 鱼列表仍扫描全部鱼。
- 统计结构中的 `DenseCellCount`、`KNNCalcCount` 尚未填充，`VisibleHighFishCount` 当前取自 Chunk 统计。
