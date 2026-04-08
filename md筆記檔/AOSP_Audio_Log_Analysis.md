# Android TV 語音助手音訊 Log 深度分析報告

本檔案針對 `aaa.log` 進行詳細解析，協助診斷為何 AEC 插件已掛載但格式不正確的問題。

---

## 1. 關鍵疑問解答

### Q1: 為什麼 pcm_config_mic_capture 已改 16-bit，Log 仍顯示 32-bit？
**更新：已解決！** 
透過修改 `AlsaInputStream.cpp/AudioInputStreamConfigs.cpp` 中的 `kCapsInputStream` 定義，成功將 `HAL format` 與 `Processing format` 降回 **0x1 (16-bit)**。

### Q2: Log 中的多個 I/O Handle 代表什麼？(IO: 54 還是 46？)
`I/O Handle` 是系統動態分配的。
*   **最新狀況**：`dumpsys` 與 `audio_hw` Log 已確認 `Echo Reference` 被請求為 **16-bit**。
*   **關鍵瓶頸**：HAL 的 `openInputStream` 已回傳 **0 (成功)**，但系統隨即執行 `closeInputStream`。這代表框架雖然「開門了」，但因為某些原因（如採樣率不匹配）旋即將門關上。

### Q3: `AecSetDevice 80000004` 是內建麥克風嗎？
**是的。** 在 Android AOSP 原始碼中：
*   `AUDIO_DEVICE_IN_BUILTIN_MIC` 定義為 `0x80000004`。
這行 Log (第 206 行) 代表系統已經成功通知 AEC 模組：現在的聲音來源是物理上的內建麥克風。

---

## 2. 如何判斷 AEC 是否啟動與硬體狀態

### 確定硬體資訊（Input Thread 46）
查看 Log 第 16-30 行：
*   **Sample rate: 16000 Hz**：正確，語音識別標準頻率。
*   **Input device: 0x80000004**：代表目前硬體正在開啟內建麥克風。
*   **Audio source: 7 (VOICE_COMMUNICATION)**：代表 App 請求的是具備回音消除功能的通訊模式。

### 確定 AEC 是否成功掛載
查看 Log 第 195-208 行（這是最重要的一段）：
1.  **`EffectCreate: uuid: bb392ec0 session 73 IO: 46`**：
    *   `bb392ec0` 是 AOSP AEC 的 ID。
    *   `session 73` 是錄音 App 的 ID。
    *   `IO: 46` 代表它精準地掛在我們的錄音串流上。**這步是成功的！**
2.  **`AecInit`**：代表 AEC 演算法模組初始化成功。
3.  **`setEnabled(true) returned: 0`**：代表 App 成功啟動了回音消除功能。
4.  **`Session_SetProcEnabled ... enabled 1`**：代表系統已將處理流程切換到「啟用」狀態。

---

## 3. 目前的瓶頸與建議

**目前的瓶頸：**
1.  **16-bit 握手成功**：`DEBUG_AEC` Log 證實系統已發出 16-bit 請求且 HAL 回應成功。
2.  **異常啟閉循環**：系統反覆開啟 16kHz 與 48kHz 的 Echo Reference 但隨即關閉，導致 Patch 無法穩定建立。
3.  **無數據流量**：`PreProcessing` 層級目前依然收不到資料。

**建議下一步 (Debug 攻關)：**
1.  **強效 Log 診斷**：在 `PreProcessing.cpp` 的 `Process` 函數改用 `ALOGE` 強制輸出的 `AEC_FLOW` 標籤，確認即便 Patch 消失，是否有任何數據碎片進入。
2.  **HAL 開放 48kHz**：在 `AudioInputStreamConfigs.cpp` 為 `ECHOREF` 開放 48000Hz 支援，以匹配當前的 Speaker 輸出頻率，減少系統因 Sample Rate 轉換問題而關閉路徑的可能性。
3.  **檢查 Patch Manager**：追蹤 `rtk_audio_hw_patch` 為什麼沒為此路徑建立連線。

---
*記錄時間：2026-04-01*
*分析人：Android 系統架構師 (Antigravity AI)*
