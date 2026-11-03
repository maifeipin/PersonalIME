# 基础词典资源规范与元数据

本文档记录 PersonalIME Stage 2 交付的基础词典资源来源、开源许可证、版本、条目数量及转换步骤。

---

## 1. 86 版五笔基础词典 (`data/dictionaries/wubi86.dict`)

- **资源名称**：Rime 86 五笔词库 (`wubi86.dict.yaml`)
- **原始仓库**：[rime/rime-wubi](https://github.com/rime/rime-wubi)
- **固定不可变提交 (Commit SHA)**：`152a0d3f3efe40cae216d1e3b338242446848d07`
- **直接下载源**：`https://raw.githubusercontent.com/rime/rime-wubi/152a0d3f3efe40cae216d1e3b338242446848d07/wubi86.dict.yaml`
- **开源许可证**：LGPL v2.1
- **词典版本**：v0.7
- **条目数量**：**136,916** 条有效单字与词组映射
- **输入原始哈希 (Raw YAML SHA256)**：`F833D86B72341FE82E069A425B6625F29EF85F1BC0F34F6FB7975FE514888B5A`
- **输出词典哈希 (Output TSV SHA256)**：`A420ADDA6A5E36DEB7DA052C2FC43512F28DD4AFC698757CA0CCE4386932FA6F`
- **编码范围**：规范化小写字母 `a-y`（1~4 码），包含一级简码、二级简码及全码。
- **字段规范 (TSV)**：
  ```text
  词条文本 \t 五笔编码 \t 基础权重
  工\ta\t99
  了\tb\t1000
  中文\tkhyg\t10
  ```

---

## 2. 全拼基础词典 (`data/dictionaries/pinyin.dict`)

- **资源名称**：Rime 简化字拼音词库 (`pinyin_simp.dict.yaml`)
- **原始仓库**：[rime/rime-pinyin-simp](https://github.com/rime/rime-pinyin-simp)
- **固定不可变提交 (Commit SHA)**：`0c6861ef7420ee780270ca6d993d18d4101049d0`
- **直接下载源**：`https://raw.githubusercontent.com/rime/rime-pinyin-simp/0c6861ef7420ee780270ca6d993d18d4101049d0/pinyin_simp.dict.yaml`
- **开源许可证**：Apache 2.0 / LGPL
- **词典版本**：v0.1
- **条目数量**：**65,125** 条有效单字与词组映射
- **输入原始哈希 (Raw YAML SHA256)**：`E341598343A0F0F2035BB1AAFC34A7F3BB7887DEEECB3F60796262AAA2983E6B`
- **输出词典哈希 (Output TSV SHA256)**：`30B139BB7B3CA1F39BF05052820B4C6B2D124ACC4C9BB67047A03170ED2B5F97`
- **编码范围**：现代汉语全拼规范音节（支持多音字、多音节词组）
- **字段规范 (TSV)**：
  ```text
  词条文本 \t 连续编码 \t 音节切分 \t 基础权重
  中\tzhong\tzhong\t536
  文\twen\twen\t389
  中文\tzhongwen\tzhong wen\t114
  西安\txian\txi an\t35
  先\txian\txian\t210
  ```

---

## 3. 转换与生成步骤

1. **原始资源获取**：
   通过自动化下载脚本获取官方 Rime 仓库的 YAML 字典文件：
   - `wubi86.raw.yaml`
   - `pinyin_simp.raw.yaml`
2. **清洗与标准化**：
   运行 `python scripts/build_dictionaries.py`：
   - 过滤注释行（以 `#` 开头）与 YAML 头部元数据（`...` 之前）。
   - 规范化小写编码与合法 ASCII 字符过滤。
   - 提取多音节拼音的音节切分串与连续无空格编码。
   - 对词频权重做对数/平滑缩放映射，限制在合理基准分区间内（1 ~ 1000）。
3. **输出目标**：
   生成紧凑、高效的 UTF-8 TSV 词典：
   - `data/dictionaries/wubi86.dict` (136,916 条，约 1.98 MB)
   - `data/dictionaries/pinyin.dict` (65,125 条，约 1.81 MB)
   总条目数：**202,041** 条，提供即开即用的完备中文输入体验。

