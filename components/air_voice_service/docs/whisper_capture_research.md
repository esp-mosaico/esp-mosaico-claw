# 气声拾音调研与实现说明

- 适用：ESP-MOSAICO（ESP32-S31 + ES8311）上的 Air Voice 测试功能。
- 目标距离：**3–5 cm**，从嘴唇量到麦克风开孔。
- 更新：2026-10-11。
- 代码位置：

| 内容 | 位置 |
|---|---|
| 服务与算法 | `components/air_voice_service/` |
| HTTP 接口 | `components/http_server/http_server_air_voice_api.c` |
| 网页 | 侧边栏"气声拾音"，`frontend_source/src/pages/AirVoicePage.tsx` |
| 设备界面 | `components/mosaic_ui/apps/air_voice/` |

## 1. 需求

- **场景：**在公共场合、办公室里，记录不方便说出口的内容。用户会捂住嘴，对着设备麦克风用气声说话：既不暴露口型，也不让旁人察觉在说话。
- **目标：**旁人听不到，但录下来的内容清楚、可懂、可转写。
- **距离：**3–5 cm 是主场景。原来 5–15 cm 的远讲预设已删除，20 cm 以上不考虑。
- **测试：**设备实时处理并保存处理后音轨，同时保存原始麦克风音轨，供离线回放、对比和算法迭代。

## 2. 气声的声学特点

以下归纳自 [1]–[4]、[7]。具体数值因语言、元音和说话人而异。

| 特点 | 说明 | 对算法的影响 |
|---|---|---|
| 无声带振动 | 激励是声门处的湍流，没有基频和谐波 | 依赖基频或谐波的 VAD、降噪、增强会失效。气声在频谱上"像噪声" |
| 声压低 | 明显低于正常说话。SilentWhisper 在 30–50 mm 处测得普通气声 50–70 dB，"微弱气声" 40–60 dB [7] | 需要较高的模拟增益，麦克风自噪声和环境底噪更显眼 |
| 频谱平、低频弱 | 500 Hz 以下滚降，500–2000 Hz 比正常语音平 [1]。F2 与 F1 强度接近，能量向 F3 偏移 [2] | 可懂度信息主要在 1–5 kHz；500 Hz 以下多半不是语音，可以更激进地处理 |
| 共振峰上移 | F1 平均上移约 147 Hz，带宽变宽约 59 Hz [2]。普通话气声同样上移、变宽 [3] | 语音判决频带取 600 Hz–5 kHz |
| 气流量大 | 气声时声门开放，经喉气流通常比正常发声大、声门阻力更低 [4]。这一规律因人和气声类型而异 | 近讲时气流直接冲击麦克风开孔，产生低频湍流噪声（第 3 节） |

## 3. 3–5 cm 的关键问题：气流冲击麦克风

### 3.1 距离系列实测

录音为 av_20261011_122111 … 122427，使用旧预设（增益 30+6 dB）。下表统计的是原始音轨，可用 `test/host/eval_takes.py` 复现。

| 录音 | 距离（约） | 语音 p90 | 底噪 | SNR | 低频盖过中频的帧 | 频谱倾斜 | 峰值 | 主观 |
|---|---|---|---|---|---|---|---|---|
| 122111 | 3 cm | −31.3 dBFS | −70.4 | 39.1 dB | 63.6% | −9.8 dB | −13.1 dBFS | 最差 |
| 122210 | 5 cm | −42.7 | −71.5 | 28.8 | 42.2% | −2.1 | −14.5 | |
| 122309 | 10 cm | −45.4 | −70.3 | 24.9 | 38.9% | −1.7 | −14.5 | |
| 122427 | 15 cm | −46.9 | −70.7 | 23.9 | 7.9% | +1.5 | −29.5 | 最好 |

列的定义：

- **低频盖过中频的帧：**活跃帧中，20–250 Hz 总能量超过 0.5–4 kHz 总能量的比例。
- **频谱倾斜：**2–4 kHz 相对 0.5–1.5 kHz 的平均电平。负值表示声音发闷。

结论：

- 四条录音都是 0 削波、0 丢帧。3 cm 的信噪比最高，主观却最差。
- 原因是气流。3 cm 时，64% 的活跃帧里低频能量超过了语音频带。
- 气流帧和同一录音里的干净帧相比：
  - 150–250 Hz 高出约 27 dB；
  - 400 Hz 高出约 22 dB；
  - 1 kHz 仍高出约 12 dB；
  - 1.5 kHz 以上基本不受影响。
- 听感上，低频"呼呼"声盖住了语音，整体发闷（倾斜 −9.8 dB）。
- 所以 3–5 cm 下电平和信噪比都够用，瓶颈是气流噪声，而不是增益或底噪。

### 3.2 摆放与手势建议

以下是经验建议，尚未在本设备上逐项测量。

- 距离按嘴唇到麦克风开孔算，保持 3–5 cm。
- 让开孔偏离嘴前的气流方向约 30–45°，例如放在唇线的侧下方。估计只损失 1–3 dB 高频，却能避开大部分气流。
- 捂嘴时，手掌留出侧向出口，让气流从侧面散掉。不要在嘴和麦克风之间围成一个直冲开孔的密闭腔。
- 条件允许时，在开孔上加一层防风网或海绵。
- 设备显示 `BREATH HITS MIC - TILT IT`（网页提示"气流打到麦克风"）时，把设备稍微侧转。

## 4. 增益结构

- ES8311 有两级增益：模拟 PGA 0–30 dB（3 dB 一档），数字 ADC 增益 0–42 dB（6 dB 一档）。
- 出厂设置是 30 + 42 = 72 dB。
  - 最早那段录音 `recording_188313387.wav`（27.6 s）里有 94,181 个采样削波，约占 21%，整体 RMS −4.6 dBFS。
  - 所以它听起来"几乎全是噪音"：问题不是麦克风，而是增益太大、全程饱和。
- 3–5 cm 预设现在用 30 + 0 = 30 dB。按 3.1 的实测推算：
  - 3 cm 时峰值约 −19 dBFS，语音 p90 约 −37 dBFS；
  - 气流冲击还有 15 dB 以上的余量；
  - 电平由后级 AGC 补齐（目标 −20 dBFS，最大 +30 dB）。
- 降低数字增益不损失信噪比：
  - 数字增益在 ADC 之后，信号和噪声被同比例缩放。
  - 30 dB 增益下底噪约 −76 dBFS，仍比 16 bit 量化底噪高 20 dB 以上。

## 5. 算法调研

### 5.1 当前固件的处理链

处理链是纯 C 实现，不依赖 ESP-IDF，主机上可以用同一份代码逐位复现设备结果（见 `air_voice_dsp.h`）。算法延迟 32 ms。

```
int16 → 高通（4 阶 Butterworth）→ STFT 512/256（sqrt-Hann）
      → MCRA 噪声估计 + 决策导向 Wiener 降噪 → 气流抑制 → 存在感 EQ → ISTFT
      → 语音门控 AGC（含语句间门限）→ 峰值限幅 → int16
旁路分析：600 Hz–5 kHz 频带 VAD；4 kHz 以下自相关的周期度（区分气声与正常发声）
```

| 模块 | 做法 | 理由 |
|---|---|---|
| 高通 | 120 Hz（嘈杂预设 150 Hz） | 去直流、手部摩擦声和部分气流。气声在 500 Hz 以下本来就弱 |
| 噪声估计 | MCRA [19]，外加约 4–5 s 长窗最小值上限（偏置 5） | 气声像噪声且持续时间长，普通 MCRA 会逐渐把它"学"进噪声。实测曾漂到 −40 dBFS。长窗最小值把估计钉在真实底噪附近 |
| 降噪 | 决策导向先验 SNR 的 Wiener 增益 [20]，先验 SNR 设下限抑制音乐噪声 [21]。最大衰减 12 dB（嘈杂 18 dB） | 计算量小，不依赖训练数据，对没有基频的气声同样成立 |
| 气流抑制 | 见 5.2 | 3–5 cm 的主要干扰 |
| 存在感 EQ | 1.5–6.5 kHz 钟形提升，2.5–4.5 kHz 平顶，+4 dB | 补偿气声高频，提高可懂度 |
| AGC | 只在 VAD 判为语音的帧上调整。目标 −20 dBFS，最大 +30 dB，起控 50 ms，释放 2 s | 停顿时不抬高底噪。ESP-SR AFE 的连续 AGC 有放大房间噪声、导致 VAD 误触发的报告 [13] |
| 门限 | 语句间额外衰减 6 dB（嘈杂 10 dB），64 ms 拖尾 | 停顿更干净，同时不切掉词尾 |
| 限幅 | 峰值限幅，上限 −1 dBFS | AGC 之后不削波 |
| VAD | 600 Hz–5 kHz 频带 SNR，阈值 3 dB（嘈杂 4 dB） | 避开气流所在的低频，覆盖气声共振峰 |
| 清浊判别 | 周期度 < 0.45 判为气声，≥ 0.6 判为正常发声 | 统计气声占比，确认用户确实在用气声；也用于气流抑制的保护 |

另一种噪声估计是最小统计 [22]。它和 MCRA 同属"跟踪最小值"一类，但对持续的气声同样会泄漏，所以没有采用。

### 5.2 气流抑制（本版新增）

- **检测：**每帧比较两个频带的平均功率：
  - 125–250 Hz 中超出噪声估计的部分；
  - 1.5–4 kHz。

  气流参考量 = 低频超出量 − 0.5 × 高频；0.5 是干净气声在这两个频带的比值上限。低频超过高频的 4 倍时，记为"气流帧"。
- **外推：**按 (180 Hz / f)^1.8，即 −5.4 dB/倍频程，把参考量外推到各频点。
  - 斜率由 3–15 cm 的录音拟合。
  - 外推量上限为 30 倍。
  - 1.0–1.5 kHz 之间余弦渐隐，1.5 kHz 以上不处理。
- **抑制：**功率谱减，过减系数 2.5，最大衰减 24 dB（嘈杂 30 dB）。气流增大时立即跟上，减小时每帧按 0.5 释放。
- **保护：**周期度 ≥ 0.6 的正常发声帧不做气流抑制，避免削掉浊音的基频和 F1。
- **放弃的第一版：**它用 250–450 Hz 做参考，并把高通提到 200 Hz，结果连气声 F1 和浊音低频一起削掉了。

### 5.3 神经网络降噪（可选方向）

| 方案 | 现状 | 用于气声 |
|---|---|---|
| ESP-SR NSNet2 | 16 kHz，帧长 64 ms、帧移 32 ms，量化模型；支持 ESP32-S3/S31/P4 [11] | 用正常语音训练，可能把气声当噪声压掉 |
| ESP-SR NSNet3 | float32，4.82 万参数，512/256 帧；已在 S31、P4 上实测，P4 @400 MHz 约占单核 20% [11] | 风险同上；帧长与本链路一致，最值得先试 |
| RNNoise | 48 kHz 下约 40 MFLOPS [14]；Raspberry Pi Pico 2（M33 @150 MHz）每 10 ms 帧耗时 22 ms，只能实时处理 16 kHz [15] | 依赖基音滤波，对气声无效，需要用气声数据重训 |

建议：

1. 把 NSNet3 作为可选降噪后端接入。
2. 在同一批原始音轨上，与现有 MCRA/Wiener 做 A/B，比较对比度、气流残留和主观可懂度。
3. 气声上达不到现有链路水平，就保留现有链路。

### 5.4 气声检测

- Amazon Alexa 的 whisper mode 在远场设备上用 LSTM + 对数滤波器组能量（LFBE）检测气声 [9][10]。
- 该研究还发现：数据足够时，只用 LFBE 的 LSTM 就能达到"LFBE + 人工特征"的效果。
- 本固件用更轻的周期度判别，只用于统计、提示和气流抑制的保护。

### 5.5 下游：识别与转换

- **气声识别：**通用 ASR 处理气声效果差，需要用气声数据微调。
  - WavLM 微调 [16]。
  - AISHELL6-Whisper 的音视频基线，在其测试集上气声 CER 4.13%，正常语音 1.11% [17]。
  - SilentWhisper 用 HuBERT，加入使用者本人的气声数据后，WER 13.9%、CER 6.4% [6]。
- 如果目标是转写，建议把原始或轻处理的音轨送给经气声适配的 ASR，避免过强的降噪损伤高频细节。
- **气声转正常语音：**
  - WESPER（CHI 2023），零样本、可实时 [5]；
  - WhisperVC，普通话，基于 AISHELL6-Whisper 训练 [18]。
  
  这类方法适合回放时还原正常音色，但计算量不适合放在 S31 上，应在手机或服务端完成。
- **硬件路线对比：**
  - NAM 体传导麦克风，贴在耳后或下颌皮肤上 [12]；
  - WhisperMask 面罩式麦克风 [8]；
  - SilentWhisper 领口指向性麦克风，后续改用 30–50 mm 头戴麦克风 [6][7]。
  
  本项目是手持设备加普通麦克风，对应的做法是"近讲 + 摆放 + 算法"。

### 5.6 数据集

| 数据集 | 内容 | 备注 |
|---|---|---|
| AISHELL6-Whisper [17] | 普通话，气声与平行正常语音各约 30 h，167 人，48 kHz，含正脸视频 | CC BY-NC-SA 4.0，仅限非商用 |
| wTIMIT | 英语 TIMIT 句子的正常/气声平行录音 | SilentWhisper 用它和 CSTR 语料训练基线模型 [6] |
| CHAINS | 英语，含同一批说话人的正常与气声朗读 | 常用于说话人与发声方式研究 |

这些数据都是在常规距离下录的，不含 3–5 cm 捂嘴时的气流。所以本设备的原始音轨是不可替代的调参数据。

## 6. 当前实现

### 6.1 预设

| 预设 | 场景 | 增益 | 高通 | 降噪 | 门限 | VAD | 气流抑制 | 原始音轨 |
|---|---|---|---|---|---|---|---|---|
| `whisper_near`<br>近讲气声 3–5 cm | 办公室、安静环境 | 30+0 dB | 120 Hz | 12 dB | 6 dB | 3 dB | 开，24 dB | 保存 |
| `whisper_noisy`<br>嘈杂 3–5 cm | 嘈杂环境 | 30+0 dB | 150 Hz | 18 dB | 10 dB | 4 dB | 开，30 dB | 保存 |
| `raw`<br>旁路 | 对照 | 30+0 dB | 关 | 关 | 关 | — | 关 | 不单独保存（处理音轨就是原始信号） |

两个气声预设都启用 +4 dB 存在感 EQ、AGC（目标 −20 dBFS，最大 +30 dB）和限幅器。

旧版本保存的设置（v1，含 5–15 cm 远讲预设）在首次启动时会换成新默认值，日志显示 `saved settings predate v2 presets; using new defaults`。

### 6.2 存储与界面

- **格式：**16 kHz、单声道、16 bit WAV，存放在 `<DATA>/air_voice/`。
- **文件头：**
  - 录音时写流式 WAV 头（长度字段为 0xFFFFFFFF），停止时只需关闭文件并重命名。
  - 旧做法是回写文件开头，会触发 LittleFS 写时复制、重写整个文件，停止后要等约 7 s。
  - 网页下载时，由 HTTP 接口填入真实长度。
- **断电恢复：**断电留下的 `.part` 文件在下次启动时恢复。
- **网页（侧边栏"气声拾音"）：**
  - 实时电平和历史曲线；
  - 指标：SNR、AGC、气声占比、气流占比等；
  - 录音、试听、设备播放、分析、下载、删除；
  - 设置和日志。
- **设备界面：**
  - 主按钮：PLAY / REC / PROC·RAW；
  - 副按钮：VOL− / MODE / DEL / VOL+；
  - 底部提示行，含气流提示。

## 7. 算法回放结果

方法：

- 用 3.1 的四条原始音轨，在主机上分别跑旧链路和新链路。
- 旧链路是上一版的 `whisper_near`：36 dB 采集，无气流抑制，EQ +3 dB。
- 新链路是本版的 `whisper_near`，用 `pre_gain_db=-6` 模拟 30 dB 采集。
- 打分用 `eval_takes.py`。
- 设备上旧固件录下的处理音轨，与主机"旧链路"的气流残留、倾斜相差不超过 0.3 dB，说明主机回放能代表设备行为。

| 录音 | 对比度 旧→新 | 气流残留 旧→新 | 频谱倾斜 旧→新 |
|---|---|---|---|
| 3 cm | 42.9 → 45.7 dB | 16.0 → 1.5 dB | −6.8 → +1.3 dB |
| 约 5 cm | 41.0 → 41.2 | 15.7 → −0.2 | +1.7 → +7.0 |
| 约 10 cm | 37.7 → 36.6 | 18.6 → 7.4 | +3.3 → +8.0 |
| 15 cm | 37.1 → 38.7 | 7.1 → 2.5 | +7.4 → +8.8 |

指标定义：

- **对比度：**语音帧与停顿帧在 1–4 kHz 的电平差，相当于不受 AGC 影响的 SNR。
- **气流残留：**气流帧上 150–600 Hz 相对 1.5–4 kHz 的电平，越低越好。
- **频谱倾斜：**同 3.1 的定义。

结论：

- 3–5 cm 的气流残留从约 16 dB 降到 2 dB 以内，"发闷"被纠正，对比度没有损失。
- 合成信号回归（`gen_test_signals.py` + `eval_output.py`）：

  | 信号 | 对比度 |
  |---|---|
  | 近讲气声 | 36.9 dB |
  | 低信噪比气声（压力测试） | 24.0 dB |
  | 正常发声 | 46.5 dB |
  | 带喷麦的气声 | 36.7 dB |

  全部 0 削波。
- 全部处理关闭时，输出与输入逐位一致。

以上是主机回放结果，真机验证见第 9 节。

## 8. 测试方法

### 8.1 设备与网页

1. 浏览器打开设备地址，进入侧边栏"气声拾音"。
2. 选择预设（默认"近讲气声 3–5 cm"），保持"保存原始音轨"开启。
3. 每次录音固定距离、角度和手势（捂嘴或不捂、开孔正对或偏轴），读同一段文字：
   - 每条 15–30 s；
   - 前后各留 2 s 静音，用于估计底噪。
4. 在录音列表里：
   - 点"分析"，看语音 p90、底噪、峰值、削波数；
   - 点"试听"，对比处理音轨和原始音轨；
   - 下载 WAV 做离线分析。
5. 在日志面板确认 `dropped 0`，并查看保存耗时 `closed in N ms`。

### 8.2 主机回放

主机回放用的是与设备同一份 C 代码。

```bash
cd components/air_voice_service/test/host
cc -O2 -std=c11 -Wall -Wextra -I../.. -o av_dsp_host air_voice_dsp_host.c ../../air_voice_dsp.c -lm
./av_dsp_host take_raw.wav take_new.wav pre_gain_db=-6        # 未写的参数取 whisper_near
./av_dsp_host take_raw.wav take_old.wav airflow=0 eq_presence_db=3
python3 eval_takes.py take_raw.wav take_old.wav take_new.wav  # 设备下载的处理音轨前加 --aligned
python3 gen_test_signals.py /tmp/av_sig                        # 带标注的合成测试信号
```

### 8.3 判定标准（建议）

- 0 丢帧、0 削波。3–5 cm 下语音峰值 ≤ −6 dBFS。
- 对比度：安静环境 ≥ 30 dB，嘈杂环境 ≥ 20 dB。
- 气流残留 ≤ 6 dB，频谱倾斜 ≥ 0 dB。
- 主观：处理音轨可懂，没有明显的"呼呼"声和音乐噪声。

## 9. 后续

1. **真机验证本版改动：**
   - 预设迁移；
   - 停止后的保存耗时；
   - 气流指标和提示；
   - 下载文件头；
   - 分析页不再整页刷新。
2. **测量处理链在设备上的 CPU 占用。**目前还没有测。
3. **用原始音轨对比 ESP-SR NSNet3 与现有链路。**
4. **扩充调参数据：**采集更多 3–5 cm 捂嘴气声，覆盖不同的人、手势和办公室噪声，用于修正气流模型的斜率和阈值。
5. **对接转写：**如需转写，对接经气声微调的 ASR，例如用 AISHELL6-Whisper 微调（注意它仅限非商用）。

## 参考资料

1. "Acoustic analysis and recognition of whispered speech", IEEE. <https://ieeexplore.ieee.org/document/5743736>
2. 气声与正常发声元音的对比分析，DTIC 技术报告 ADA163307。<https://apps.dtic.mil/sti/tr/pdf/ADA163307.pdf>
3. "Formant Comparison between Whispered and Voiced Vowels in Mandarin". <https://www.researchgate.net/publication/233542104_Formant_Comparison_between_Whispered_and_Voiced_Vowels_in_Mandarin>
4. E. T. Stathopoulos et al., "Respiratory and Laryngeal Function During Whispering", J. Speech Hear. Res. 34(4), 1991. <https://pubs.asha.org/doi/10.1044/jshr.3404.761>。个体差异见 "Whisper and Phonation: Aerodynamic Comparisons Across Adduction and Loudness", J. Voice, 2017. <https://pubmed.ncbi.nlm.nih.gov/28366247/>
5. J. Rekimoto, "WESPER: Zero-shot and Realtime Whisper to Normal Voice Conversion for Whisper-based Speech Interactions", CHI 2023. <https://doi.org/10.1145/3544548.3580706>, <https://arxiv.org/abs/2303.01639>
6. H. Hiraki, J. Rekimoto, "SilentWhisper: faint whisper speech using wearable microphone", UIST 2022 Adjunct. <https://dl.acm.org/doi/10.1145/3526114.3558715>
7. "SilentWhisper: inaudible faint whisper speech input for silent speech interaction", CHI 2025 EA. <https://dl.acm.org/doi/10.1145/3706599.3721185>
8. "WhisperMask: a noise suppressive mask-type microphone for whisper speech", AHs 2024. <https://dl.acm.org/doi/10.1145/3652920.3652925>
9. Z. Raeesy et al., "LSTM-based Whisper Detection", IEEE SLT 2018. <https://arxiv.org/abs/1809.07832>
10. Amazon Science, "Whisper to Alexa, and she'll whisper back". <https://www.amazon.science/blog/whisper-to-alexa-and-shell-whisper-back>
11. ESP-SR 文档：
    - NSNet：<https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/nsnet/README.html>
    - AFE：<https://docs.espressif.com/projects/esp-sr/en/latest/esp32/audio_front_end/README.html>
12. Y. Nakajima et al., "Non-audible murmur recognition input interface using stethoscopic microphone attached to the skin", ICASSP 2003. <https://www.researchgate.net/publication/224741372_Non-audible_murmur_recognition_input_interface_using_stethoscopic_microphone_attached_to_the_skin>；T. Toda et al., Interspeech 2009. <https://www.isca-archive.org/interspeech_2009/toda09_interspeech.html>
13. 用户报告（非官方）：ESP-SR AFE 的 AGC 放大底噪导致 VAD 误触发。<https://github.com/dlgreenwald/lifelog/issues/104>
14. J.-M. Valin, "A Hybrid DSP/Deep Learning Approach to Real-Time Full-Band Speech Enhancement", MMSP 2018. <https://arxiv.org/abs/1709.08243>
15. Arm, RNNoise examples for Raspberry Pi Pico 2. <https://github.com/ArmDeveloperEcosystem/rnnoise-examples-for-pico-2>
16. "Leveraging Self-Supervised Models for Automatic Whispered Speech Recognition", arXiv 2407.21211. <https://arxiv.org/abs/2407.21211>
17. C. Li et al., "AISHELL6-whisper: A Chinese Mandarin Audio-visual Whisper Speech Dataset with Speech Recognition Baselines", arXiv 2509.23833. <https://arxiv.org/abs/2509.23833>；数据：<https://huggingface.co/datasets/SMIIP-lab/AISHELL6-Whisper>
18. "WhisperVC: Target Speaker-Controllable Mandarin Whisper-to-Speech Conversion", arXiv 2511.01056. <https://arxiv.org/abs/2511.01056>
19. I. Cohen, B. Berdugo, "Noise estimation by minima controlled recursive averaging for robust speech enhancement", IEEE SPL 9(1), 2002.
20. Y. Ephraim, D. Malah, "Speech enhancement using a minimum mean-square error short-time spectral amplitude estimator", IEEE TASSP 32(6), 1984；同作者 log-spectral amplitude 估计器，IEEE TASSP 33(2), 1985。
21. O. Cappé, "Elimination of the musical noise phenomenon with the Ephraim and Malah noise suppressor", IEEE TSAP 2(2), 1994.
22. R. Martin, "Noise power spectral density estimation based on optimal smoothing and minimum statistics", IEEE TSAP 9(5), 2001.
