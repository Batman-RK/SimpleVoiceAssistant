# Android AOSP PreProcessing 函式庫分析 (WebRTC AEC)

本文件詳細說明 `libaudiopreprocessing.so` (對應 AOSP 中的 `PreProcessing.cpp`) 的運作邏輯、主要函式流程以及 AEC 如何被啟動。

## 1. 核心結構
`PreProcessing.cpp` 是一個包裝函式庫 (Wrapper)，它封裝了 Google 的 **WebRTC AudioProcessing (APM)** 引擎，並將其暴露給 Android 的 `AudioFlinger` 使用。

- **Session (會話)**: 每個 `AudioRecord` 對應一個 Session。同一個 Session 可以包含 AEC, NS, AGC 三個效果，但共用同一個 WebRTC APM 實例。
- **Effect (效果)**: Session 中的每一個具體處理流程 (如 AEC)。

## 2. 關鍵函式與流程

### A. 載入與創建 (Creation Phase)
1. **`PreProcessingLib_Create`**: 
   - `AudioFlinger` 在收到 `AcousticEchoCanceler.create()` 請求時第一個呼叫的進入點。
   - 根據 UUID 找到是對應 AEC、NS 還是 AGC。
   - 呼叫 `PreProc_GetSession` 來獲取或建立一個會話。
2. **`Session_CreateEffect`**:
   - 如果是該 Session 的第一個效果，會呼叫 `session->ap_builder.Create()` 建立 WebRTC APM 核心引擎。
   - 呼叫 `AecCreate` / `AecInit` 初始化 WebRTC 的 AEC 模組。

### B. 配置與啟動 (Command Phase)
1. **`PreProcessingFx_Command`**:
   - 處理各種指令。當您在 Java 呼叫 `mAec.setEnabled(true)` 時，會發送 `EFFECT_CMD_ENABLE` 指令。
   - 轉由 `Effect_SetState` 切換狀態。
2. **`Effect_SetState`**:
   - 將狀態從 `CONFIG` 切換到 `ACTIVE`。
   - 呼叫 `Session_SetProcEnabled` 並將 `enabledMsk` 指向 AEC。

### C. 音訊處理 (Processing Phase) - **核心關鍵**
1. **`PreProcessingFx_Process` (正向路徑)**:
   - 處理 **麥克風 (Mic)** 收到的原始音訊。
   - 當所有開啟的效果 (AEC, NS, AGC) 都標記為 `processed` 後，呼叫 `session->apm->ProcessStream()`。
   - 這是最後輸出的「乾淨」人聲來源。
2. **`PreProcessingFx_ProcessReverse` (反向路徑/回音參考)**:
   - **這是 AEC 成功的關鍵！**
   - 接收來自系統喇叭 (Speaker) 正在播放的訊號。
   - 呼叫 `session->apm->ProcessReverseStream()`。
   - AEC 引擎就是透過這個函式「聽見」電視在放什麼，進而從麥克風訊號中把這部分消掉。

---

## 3. 已加入的 Debug Log 說明

為了排錯，我們在 `PreProcessing.cpp` 加入了以下觀察點：

### 1. `AEC_DEBUG_MIC: RMS=...` (位於 `PreProcessingFx_Process`)
- **目的**: 確認「麥克風」的音訊是否真的有進到 AEC 處理迴圈。
- **指標**: 數字越代表聲音越大。如果為 0，代表 AEC 沒拿到人聲。

### 2. `AEC_DEBUG_SPEAKER_REF: RMS=...` (位於 `PreProcessingFx_ProcessReverse`)
- **目的**: 確認「喇叭播放的聲音」是否有成功繞回來給 AEC 當參考。
- **指標**: 
  - **如果找不到這行 log**: 代表系統完全沒有進行 Loopback 轉送，AEC 永遠消不掉聲音。
  - **如果 RMS 永遠為 0**: 代表參考音源是靜音的。

## 4. 完整的運作邏輯圖 (Flowchart)

```mermaid
graph TD
    A[Java App: AcousticEchoCanceler.create] --> B[AudioFlinger: 載入 PreProcessing 庫]
    B --> C{是否為 Session 第一個效果?}
    C -- 是 --> D[Session_CreateEffect: 建立 WebRTC APM 引擎]
    C -- 否 --> E[加入現有 Session]
    D --> F[AecInit: 初始化 AEC 參數]
    E --> F
    
    F --> G[Java App: setEnabled true]
    G --> H[Effect_SetState: 切換至 ACTIVE 狀態]
    
    subgraph 錄音處理迴圈 (每 10ms 觸發)
        I[喇叭送出聲音] --> J[PreProcessingFx_ProcessReverse: 接收回音參考]
        J --> K[WebRTC: ProcessReverseStream]
        
        L[麥克風收到聲音] --> M[PreProcessingFx_Process: 接收原始錄音]
        M --> N[WebRTC: ProcessStream 執行消音]
        K -.-> N
        N --> O[輸出乾淨的人聲]
    end
```

## 5. 實際 Log 逐行解析

以下是您所提供的 Log 逐行翻譯與底層意義，以 `Session 73` (這是一個流水號，每個 AudioRecord 啟動時分配) 為例：

| Log 輸出 | 詳細解釋與底層意義 |
| :--- | :--- |
| `EffectCreate: uuid: bb392ec0 session 73 IO: 46` | `AudioFlinger` 收到 Java 請求，根據設定檔找到 `bb392ec0` 是 AEC 效果。它決定將這個效果建立在 `Session 73` (您的 App 錄音連線)，並綁定在 `IO 46` (代表這個錄音硬體執行緒的 ID)。 |
| `Session_CreateEffect procId 2, createdMsk 00` | 系統準備建立效果。`procId 2` 在源碼中代表 AEC (0=AGC, 1=AGC2, 3=NS)。`createdMsk 00` 代表這個 Session 目前是空的，還沒有掛載過任何 WebRTC 引擎。 |
| `Effect_SetState proc 2, new 1 old 0` | 狀態機切換。`new 1` 代表 `PREPROC_EFFECT_STATE_INIT` (初始化完畢，準備就緒)。 |
| `AecInit` | 呼叫 WebRTC APM 核心的初始化程式，配置預設的延遲與演算法參數。 |
| `Session_CreateEffect OK` | WebRTC APM 引擎成功在記憶體中建立！(如果之前發生 `ENOMEM` 就是卡在前面的步驟失敗)。|
| `Session_SetConfig sr 16000 cnl 0000000c` | 設定音效格式：`sr 16000` (Sample Rate 16kHz)，`cnl 0000000c` (Channel Mask，通常代表單聲道或立體聲設定)。這反映了您在 Java 層 `AudioRecord` 設定的參數。 |
| `Effect_SetState proc 2, new 2 old 1` | 狀態機切換。`new 2` 代表 `PREPROC_EFFECT_STATE_CONFIG` (參數配置完畢等待啟用)。|
| `SimpleAssistantSession: AEC successfully enabled!` | 這是您 App (Java 層) 的 Log，代表 `mAec.setEnabled(true)` 成功執行，沒有丟出 Exception。 |
| `AecSetDevice 80000004` | **這行非常關鍵！** 系統告訴 AEC 引擎現在的聲音路由設備是什麼。`80000004` (十六進位) 代表 `AUDIO_DEVICE_IN_WIRED_HEADSET`，也就是系統偵測到錄音來源是**有線麥克風或 USB 麥克風**。 |
| `Effect_SetState proc 2, new 3 old 2` | 狀態機最後切換。`new 3` 代表 `PREPROC_EFFECT_STATE_ACTIVE` (主動狀態)。 此時 AEC 已經完全準備好接收聲音了。 |
| `Session_SetProcEnabled proc 2, enabled 1 enabledMsk 00...` | 確認 AEC 成為啟用清單中的成員。`Msk 04` 代表第三個 Bit 被立起來 (2^2=4)，也就是 `procId 2` (AEC) 成功啟動！ |

---

## 6. 核心診斷：為什麼沒有印出 `AEC_DEBUG` Log？

從上面的 Log 可以確信：**AEC 的引擎已經 100% 成功在記憶體中啟動並進入 ACTIVE 狀態。** 

但是，您完全沒有看到 `PreProcessingFx_Process` (處理麥克風人聲) 或 `ProcessReverse` (處理喇叭回音) 的 Log，這究竟是為什麼？

### 💡 答案是：`AudioFlinger` 把整條錄音水流繞過了 AEC 特效區！

在 Android 架構中，`PreProcessing` 函式庫本身是被動的。它是等著 `AudioFlinger` 的 `AudioRecordThread` (負責錄音的系統執行緒，即前面提到的 IO:46) 定期 (大約每 10ms) 把錄到的聲音塞給它。

**沒有呼叫任何 `Process` 函式的核心原因：**

1. **AudioPolicy 的高速捷徑 (Fast Capture Bypass) - 最可能的原因**：
   - 當您宣告使用 `MIC` (Source=1) 時，Android 系統判斷這只是最單純的錄音需求。為了達到「最低延遲 (Low Latency)」，AudioPolicy 會為這此錄音開啟一條名為 **Fast Track (高速音軌)** 的走線。
   - **致命點**：在 Android 原始碼的設計中，**Fast Track 絕對不支援任何 Software Audio Effect (軟體音效)**！
   - 所以，儘管您的 Java 程式成功地為這個 Session 啟動了 AEC (`setEnabled(true)`)，但因為這股麥克風收到的真實聲音 (水流) 走的是高速捷徑，它直接從最底層傳到您的 App 裡，**整條水流完全沒流經 `PreProcessing` 這個需要耗費 CPU 運算的處理廠**。
   - 這就解釋了為什麼**您連處理人聲的 `PreProcessingFx_Process` Log 都沒看到**！雖然 AEC 啟動了，但它在旁邊乾等，連一滴聲音資料都沒收到，當然也不會收到喇叭的回音參考。

2. **硬體 HAL 層錄音失敗 (機率較低)**：
   - 如果底層麥克風根本沒錄到東西 (例如 ALSA driver 沒吐資料)，那 `AudioFlinger` 自然也沒東西可以餵給 `Process`。但如果您確信最後錄出來的 WAV 檔是有電視聲音和人聲的，那就代表硬體有收到音，100% 可以確定是第 1 點造成的。

### 結論與下一步方案
要讓 AEC 真正能處理聲音，核心原則就是**絕對不能讓聲音走 Fast Track 捷徑**。
- **最佳解法：** 換回 `VOICE_COMMUNICATION`。因為 `VOICE_COMMUNICATION` 被系統定位為需要深度處理的音效，它**絕對不會**走快速通道，必定會經過 `PreProcessing`，而且會自動把回音參考給掛上。
- **作法回顧：** 正如我們稍早提議的，把 `audio_effects.xml` 中的 `<preprocess>` 區塊清空 (避免 ENOMEM 衝突)，然後在 Java 裡面用 `VOICE_COMMUNICATION` 手動掛載 AEC。
