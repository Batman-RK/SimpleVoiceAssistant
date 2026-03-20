# 🎙️ Android TV 錄音存檔與 AEC 聲學回音消除實戰 (Level 4)

這份筆記記錄了我們如何讓語音助手不僅能「聽見聲音」，更能「專注聽使用者說話」，並透過無損錄音檔的落地，建立日後評估語音模型（STT）的「黃金基準樣本（Golden Sample）」。

---

## 為什麼智慧電視 (Smart TV) 極度需要 AEC？

**AEC (Acoustic Echo Cancellation, 聲學回音消除)** 是遠場語音（Far-field Voice）技術的核心。
電視本身是一個巨大的發聲體。當電視正在大聲播放電影，而使用者同時對著電視下達語音指令時，麥克風錄到的聲音會是 `[電影聲 + 人聲]`。
如果不經過 AEC 處理，STT 引擎會把它當作一團噪音而完全辨識失敗。AEC 的作用，就是利用軟硬體算法，把「本機發出的聲音（Echo）」從麥克風收音中精準減去，只留下乾淨的「人聲」。

---

## 一、 AOSP 底層開通 AEC 支援 (Platform Level)

許多 Android TV 開發板預設沒有開啟硬體或軟體的 AEC 模組。必須從 AOSP source code 的 `audio_effects.xml` 打通任督二脈：

### 📍 修改路徑與配置
編輯 `frameworks/av/media/libeffects/data/audio_effects.xml`（或廠商特定的 overlay 目錄，如 `vendor/etc/audio_effects.xml`）：

```xml
<audio_effects_conf version="2.0">
    <libraries>
        <!-- 引入負責前處理 (Pre-processing) 的 Library -->
        <library name="pre_processing" path="libaudiopreprocessing.so"/>
        ...
    </libraries>
    
    <effects>
        <!-- 宣告 AEC 效果器並綁定對應的 Library 與 UUID -->
        <effect name="aec" library="pre_processing" uuid="bb392ec0-8d4d-11e0-a896-0002a5d5c51b"/>
        ...
    </effects>
</audio_effects_conf>
```
重新 Full Build 系統大包並燒錄後，呼叫 `android.media.audiofx.AcousticEchoCanceler.isAvailable()` 就會回傳 `true`，代表系統級的 DSP 或軟體回音消除已就緒。

---

## 二、 App 層掛載 AEC 與錄音存檔實作 (`MyVoiceInteractionSession.java`)

在我們的 Session 中，我們做了兩件大事：
1. **動態掛載/卸載 AEC 效果器**
2. **將 PCM 音軌即時寫成 `.wav` 音檔**

### 1. 核心變數宣告
```java
// AEC 效果器
private android.media.audiofx.AcousticEchoCanceler mAec;

// 檔案寫入相關
private java.io.File mAudioFile;
private java.io.FileOutputStream mFileOutputStream;
private long mTotalAudioLen = 0;
```

### 2. 啟動錄音 (`startRecording`)
在 `AudioRecord` 初始化成功後，我們先建立檔案流，並且**立刻為這個 AudioSession 綁定 AEC**：

```java
// 1. 初始化檔案與預留 WAV 44 bytes 標頭
File dir = new File("/storage/emulated/0/Recordings");
mAudioFile = new File(dir, "voice_test_" + System.currentTimeMillis() + ".wav");
mFileOutputStream = new FileOutputStream(mAudioFile);
mFileOutputStream.write(new byte[44]); // 先寫入空白佔位

// 2. 建立麥克風物件
mAudioRecord = new AudioRecord(MediaRecorder.AudioSource.MIC, 16000, 
        AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, minBufferSize);

// 3. 檢查與掛載 AEC (關鍵！)
if (android.media.audiofx.AcousticEchoCanceler.isAvailable()) {
    mAec = android.media.audiofx.AcousticEchoCanceler.create(mAudioRecord.getAudioSessionId());
    if (mAec != null) {
        mAec.setEnabled(true);
        Log.d(TAG, "AEC successfully enabled!");
    }
}
```

### 3. 持續寫入檔案 (`recordLoop`)
在背景迴圈中，將 `short[]` 轉換為 `byte[]` 並透過 `FileOutputStream` 寫入儲存空間：

```java
int readBytes = mAudioRecord.read(buffer, 0, buffer.length);
if (readBytes > 0) {
    byte[] byteBuf = shortToByte(buffer, readBytes);
    mFileOutputStream.write(byteBuf);
    mTotalAudioLen += byteBuf.length; // 累積音軌長度，結尾寫 WAV Header 會用到
}
```

### 4. 結束錄音與封裝 WAV (`stopRecording`)
錄音結束時，除了關閉 `AudioRecord`，更必須**手動釋放 AEC 資源**以避免 Memory Leak，並將 44 bytes 的標準 WAV Header (包含精確的取樣率、聲道數與 `mTotalAudioLen`) 從檔案起點覆蓋回寫。

```java
// 釋放 AEC
if (mAec != null) {
    mAec.release();
    mAec = null;
    Log.d(TAG, "AEC released");
}

// 關閉 Stream 並回寫 Header
if (mFileOutputStream != null) {
    mFileOutputStream.close();
    updateWavHeader(mAudioFile, mTotalAudioLen); // 寫入 RIFF/WAVE 等二進位標頭
    Log.d(TAG, "Audio file saved to: " + mAudioFile.getAbsolutePath());
}
```

---

## 三、 黑客級除錯與測試流 (測試小撇步)

### 不想 Full Build 的神速安裝法與「權限自動豁免」
如果你在 Image 中沒有預載這個舊版的系統 App，你可以直接用 `pm install -r` 安裝你隨時 `mm` 編譯出來的新包。
**💡 神奇的系統金鑰與 UID 1000：**
因為我們在 `AndroidManifest.xml` 中宣告了 `sharedUserId="android.uid.system"`，且我們編譯用的簽名金鑰（Platform Key）跟目前燒錄的系統映像檔完全吻合。在這種「乾淨」的狀態下安裝，Android 的安全機制會**自動批准**所有的危險權限（包含 `RECORD_AUDIO` 與讀寫外部儲存空間）。
你**完全不需要**手動輸入 `pm grant` 來賦予權限，安裝後直接呼叫 `show` 就能存檔與錄音！

### 查看 AEC 與錄音狀態指令
1. 彈出助手 (觸發存檔與 AEC 掛載)：
   `cmd voiceinteraction show`
2. 關閉助手 (觸發寫入檔頭並結案)：
   `cmd voiceinteraction hide`
3. 查看專屬 Debug 即時 Log：
   `logcat SimpleAssistantSession:D *:S`

未來，這些保存在 `/storage/emulated/0/Recordings/` 且經過 AEC 淨化處理的 `.wav` 檔案，就是我們用來餵給 Wake-word 引擎或雲端 LLM 認知中心的最重要素材！
