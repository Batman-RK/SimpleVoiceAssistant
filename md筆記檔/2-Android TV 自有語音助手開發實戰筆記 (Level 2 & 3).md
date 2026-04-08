# 🎙️ Android TV 自有語音助手開發實戰筆記 (Level 2 & 3)

這份筆記記錄了我們在外接 **D-MIC**、打通 **Audio Pipeline (音頻管道)** 並且透過**聲音強度（RMS）連動更新 UI 文字**的實作過程與核心知識。

---

## 一、 系統架構：Audio Pipeline 控制鏈

在 Android Voice Interaction Framework 中，當視窗（Session）跳出來後，讀取錄音並非透過一般 Activity，而是直接在 `VoiceInteractionSession` 內部操作：

## 📝 Audio Pipeline 運作流水線

當會話（Session）視窗浮現後，麥克風的聲音資料會依序經過以下四個關卡：

### 📥 1. 建立錄音管道 (AudioRecord)
*   **觸發點**：`onShow()` 生命週期被調用（提示視窗浮現在最上層）。
*   **動作**：建立 `AudioRecord` 物件，正式與板子上的 D-MIC（麥克風）對接建立專屬通路。

---

### 🧵 2. 啟動背景線程 (Background Thread)
*   **動作**：向系統借用一條獨立 Thread（線程），在運作期間內，持續利用 `mAudioRecord.read(buffer)` 將 PCM 聲音切片（16-bit）讀入記憶體快取。

---

### 📊 3. 計算短波能量 (RMS 均方根)
*   **動作**：對讀進來的數據進行平方、加總後開根號。
*   **作用**：算出這極短時間內（例如 0.1 秒）聲音的**整體平均強度**。

---

### 🔄 4. 調用 Handler 連動 UI
*   **判定層**：
    *   **大於門檻值 (有聲音)** ➔ 聯動主線程，切換文字為 **「🎙️ 正在接收聲音...」**
    *   **小於門檻值 (無聲狀態)** ➔ 文字維持為 **「🎙️ 正在傾聽中...」**。

---

## 二、 核心程式碼解析：`MyVoiceInteractionSession.java`

這是本次實作最關鍵的類別，我們對其進行了以下改造：

### 1. 宣告 AudioRecord 配置與控制 Flag
```java
// 語音識別標準參數：16kHz, 單聲道, 16-bit PCM
private static final int SAMPLE_RATE = 16000;
private static final int CHANNEL = AudioFormat.CHANNEL_IN_MONO;
private static final int ENCODING = AudioFormat.ENCODING_PCM_16BIT;

private AudioRecord mAudioRecord;
private boolean mIsRecording = false; // 控制背景 thread 迴圈開關
private Thread mRecordThread;
```

### 2. 生命週期管理（啟動與釋放資源）
我們在 `onShow` 與 `onHide` 當中實現錄音資源的動態索取與釋放，這是為了**避免語音助手退到背景後持續佔用麥克風**：

```java
@Override
public void onShow(Bundle args, int showFlags) {
    super.onShow(args, showFlags);
    startRecording(); // 顯示時立即啟動錄音
}

@Override
public void onHide() {
    super.onHide();
    stopRecording(); // 隱藏時關閉錄音，釋放 AudioRecord 
}
```

### 3. `startRecording()`：初始化錄音管道
```java
private void startRecording() {
    int minBufferSize = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL, ENCODING);
    // 使用 MIC 作為 Input Source
    mAudioRecord = new AudioRecord(MediaRecorder.AudioSource.MIC, 
            SAMPLE_RATE, CHANNEL, ENCODING, minBufferSize);

    mAudioRecord.startRecording();
    mIsRecording = true;
    
    // 開啟背景 Thread 處理耗時的 I/O 讀取
    mRecordThread = new Thread(this::recordLoop);
    mRecordThread.start();
}
```

### 4. `recordLoop()`：動態音量計算與 UI 連動 (重點)
錄音執行緒持續讀取 PCM 緩衝區，並使用 **均方根 (RMS, Root Mean Square)** 計算聲音能量：

```java
private void recordLoop() {
    int bufferSize = AudioRecord.getMinBufferSize(SAMPLE_RATE, CHANNEL, ENCODING);
    short[] buffer = new short[bufferSize / 2]; // 16-bit 需讀取為 short

    while (mIsRecording) {
        int readBytes = mAudioRecord.read(buffer, 0, buffer.length);
        if (readBytes > 0) {
            // 計算均方根強度
            long sum = 0;
            for (int i = 0; i < readBytes; i++) {
                sum += buffer[i] * buffer[i];
            }
            double rms = Math.sqrt(sum / readBytes);

            // 當振幅大於設定好的 Noise Threshold，驅動 UI 貼換
            if (rms > NOISE_THRESHOLD) {
                mMainHandler.post(() -> mStatusTextView.setText("🎙️ 正在接收聲音..."));
            } else {
                mMainHandler.post(() -> mStatusTextView.setText("🎙️ 正在傾聽中..."));
            }
        }
    }
}
```

---

## 💡 三、 AOSP 平台踩坑與排查秘笈 (打通關節)

在我們從無聲到有聲的打通過程中，我們解決了兩大 AOSP TV 專屬坑位：

### 1. `voice_interaction_service.xml` 必須帶有完整類別名稱 (FQCN)
*   **狀況**：`cmd` 啟動時報 `showSessionForActiveService() returned false`。
*   **因由**：有些 AOSP 版本在解析自定義 XML 時不會自動把包名補在點號（`.Service`）前面。
*   **修復方式**：將 XML 宣告從相對點號改為絕對全名：
    ```xml
    android:sessionService="com.example.simplevoiceassistant.MyVoiceInteractionSessionService"
    ```

### 2. 按鍵與助手繫連
除了寫入 `settings put secure voice_interaction_service` 設定外，如果要處理實體 Remote 按鍵映射或者透過 `input keyevent 231` 驅動，需要確保 **`settings put secure assist`** 內也包含對應的助手服務位址，AOSP 的 Keyevent 分發才會完全打通。

---

**下一階段：預期進入 Level 4 語音轉文字（STT）套接階段！**
我們可以直接在這個 pipeline 迴圈中，將 `buffer` 結構拋給本地端 NLU 推理引擎，或是連續傳送 byte-chunk 到後台 Cloud LLM API。
