# 技术架构

核心类为 AFishSchoolManagerICO：Manager 作为场景 Actor 管理整组鱼，鱼本身以数组索引表示。源码包括 [Manager 头文件](../Source/UEFishSchool/Public/FishSchool/FishSchoolManagerICO.h)、[Manager 实现](../Source/UEFishSchool/Private/FishSchool/FishSchoolManagerICO.cpp)和 [统计结构](../Source/UEFishSchool/Public/Stats/FishSchoolStats.h)。

## 初始化与帧更新

| 阶段 | 函数与实现文件行号 | 作用 |
| --- | --- | --- |
| 输入绑定 | BeginPlay，41 | 在首个 PlayerController 的 Enhanced Input 组件上绑定启动及数据导出 Action |
| 启动 | HandleStartFishSchool，2189 | 初始化数组；加载 CSV 或随机生成；建立网格、距离参数和渲染实例 |
| 数据初始化 | InitFishSchool，111 | 分配双缓冲、鱼 ID 和 N×K 邻居缓存，缓存初值为 INDEX_NONE |
| 帧入口 | Tick，74 | 更新玩家、距离档位和行为预计算，再执行仿真与渲染提交 |
| 仿真入口 | TraversalGridFishGroup，281 | 交换缓冲、更新网格、并行计算行为并轮换 KNN 刷新组 |
| 网格构建 | UpdateGrid，329 | Count → Prefix → Scatter → CPU 粗可见性分类 |
| 邻域与行为 | GridFindKNearestNeighbour，894；UpdateFishKNN，469 | 查询局部 K 个近邻，计算目标、分离和一致性等行为 |
| 单 ISM | InitRenderInstances，178；RenderAllInstances，201 | 初始化实例，每 Tick 批量提交全部鱼的变换 |
| Chunk ISM | InitRenderChunks，948；RenderChunkedInstances，1091 | 构建 Chunk 鱼列表、更新状态并按策略提交变换 |

CSV 为可选初始状态。无对应文件时使用随机初始化，经过 warmup 后启用粗可见性分类。

行为分为三条路径：网格外及边缘鱼走目标点回归；内部格的 VisibleHigh 鱼走 KNN；其他内部格走 UpdateFishOrbitCross 简化环绕。深度关闭或本帧可见性数据无效时，内部格回退 KNN。所有行为路径均逐 Tick 执行。

## 数据布局与双缓冲

FFishSchool 将位置、朝向、格 ID、实例 ID 和邻居缓存分开存储。位置和朝向分别是连续的 FVector3f 数组，并各有两份缓冲。

每帧行为从 PreviousBuffer 读取，向 NextBuffer 写入，避免单鱼更新顺序改变其他鱼读到的状态。渲染同样读取 PreviousBuffer，当前提交相对本轮行为输出滞后一 Tick。

FGrid 通过 CellStarts/CellCounts 描述连续 SortedFishIds 中每格的鱼区间。网格坐标使用 uint8 缓存；网格原点在初始化时由目标点计算，运行时不自动随 Manager 位移重建。

## 并行网格构建

1. UpdateFishInGrid（1462）：任务分别处理不重叠的鱼区间，写鱼的 CellId，并累加任务私有的格计数。
2. 前缀和（1282 / 1326）：计算格起点及每个任务在格内的独占写入区间；并行版本采用正向、反向两个扫描分支。
3. UpdateFishIds（1490）：使用任务私有游标，将鱼 ID Scatter 到预分配区间。

各阶段等待 ParallelFor 完成后才进入下一阶段。行为任务通过排序后的不重叠鱼区间写 NextBuffer；KNN 临时距离数组按任务切片。密集格可见性阶段先写任务私有深度缓冲，再统一合并。

设 N 为鱼数、C 为格数、T 为鱼遍历任务数、K 为邻居缓存数量，网格构建主要工作量为 O(N + C×T)，任务私有计数和游标占用 O(C×T) 空间。

## 局部 KNN

查询范围固定为自身及相邻共 27 格。遍历其中 m 个候选，排除自身并插入维护 K 个最近候选，单次查询约为 O(m×K)。分组轮换刷新减少每帧查询次数，其他帧复用缓存。

该查询在局部范围内选择邻居，稀疏分布时可能找不满 K 个；集中分布时总查询仍可退化至 O(N²×K)。邻居缓存的刷新间隔则在计算成本和信息时效之间取舍。

## CPU 粗可见性与行为分级

流程为相机投影 → 密集格任务私有屏幕深度桶 → MergeDepthBuffer（1616） → 稀疏格分类 → DilateGrid（1651）邻格膨胀。

深度桶记录网格代表深度，不读取 GPU 场景深度。合并阶段选择全局最近格，同时降低落选局部赢家的状态；邻格膨胀扩展高精度行为区域。bHasValidVisibilityData 每帧先清除，完成分类后才置真，行为和渲染共同以该标志决定是否消费本帧分类。

距离自适应包含 VeryNear、Near、Mid、Far 四档，调整深度分辨率、密度阈值、膨胀、行为权重和 Chunk 提交参数。VeryNear 按玩家到鱼群目标点的距离判断，其他档位使用网格外接球距离。

这些策略以网格粒度近似可见性，在行为精度、显示数量和计算开销之间取舍。

## ISM、Chunk 与资源动画

单 ISM 每帧构造并提交 N 个变换，裁剪项用零缩放隐藏。初始化写入两个自定义浮点值：随机动画时间偏移和统一播放速度；资源材质负责读取这些值。

Chunk 默认按 5×5×5 划分为 125 个 ISM。BuildRenderChunkLists（1019）逐帧扫描全部鱼；Visible 块每帧提交，LowRender 块按间隔提交，状态或鱼数变化可触发提前提交，Hidden 块清理旧实例。容量只增不减，旧尾槽通过零缩放隐藏，因此分配槽数可能超过鱼数。

Chunk 使用更多组件换取更细的空间提交粒度，收益取决于块分布、可见数量及提交频率。当前 Chunk 的 NumCustomDataFloats 为 0，尚未接入逐鱼随机 VAT 相位与播放速度数据。VAT 形变和模型 LOD 由网格、材质和纹理资源提供。

## 已知问题

| 问题 | 当前影响 | 改进方向 |
| --- | --- | --- |
| 参数校验不完善 | 部分可编辑参数缺少范围约束和初始化检查；错误的 K、任务幂次、网格/Chunk 尺寸或鱼原长配置可能导致除零或无效数组访问 | 补充编辑器约束，并在初始化时统一校验正数、幂次和尺寸关系 |
| Chunk 未同步根 ISM 的材质覆盖 | 网格自带材质可正常使用，但根组件额外指定的 OverrideMaterials 未复制到 Chunk | 在 Chunk 初始化时同步材质覆盖 |
| 统计字段未完整维护 | DenseCellCount 和 KNNCalcCount 当前未在 Tick 中填充，保留默认值 | 补齐统计更新，并明确鱼数、分配槽数及实际提交数的口径 |

## 后续方向

- 为低精度行为增加更新频率控制；当前仅 Chunk 渲染提交支持低频策略。
- 利用激活格构建 Chunk 鱼列表，减少逐帧全量遍历。
- 接入 Chunk 逐鱼 VAT 自定义数据，完善跨块迁移时的动画连续性。
- 完善独立捕食者发现与管理；当前捕食者位置取自玩家。
- 评估 GPU 仿真或 Mass/ECS 路线；当前实现为 CPU 仿真。
