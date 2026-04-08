# 🎙️ Android TV 語音助手生命週期與流程詳解

這份筆記詳細拆解了從 **電視開機**、**助手綁定**、**按鍵/指令觸發** 到 **語音監聽** 的完整程式碼執行順序與系統元件底層運作邏輯。

---

## 一、 核心流程時間軸：從開機到錄音

在 Android TV 中，語音助手並不是隨隨便便在背景跑的 `Service`，而是由 Android Framework 的 **`VoiceInteractionManagerService`** 統一調配。

以下是完整順序（由上而下）：

### 階段 1：系統開機（Boot-up）
1.  **Android 系統啟動**：
    *   Framework 載入時，會主動讀取系統參數（Secure Settings）中的 **`voice_interaction_service`**。
2.  **判斷預設助手**：
    *   如果該參數為空（`null`），助手功能不會被觸發。
    *   如果該參數為 `com.example.simplevoiceassistant/.MyVoiceInteractionService`（我們手動打或是出廠預設），系統會做下一步。
3.  **核心綁定：大腦甦醒**：
    *   系統會連線到你的大腦常駐服務：`MyVoiceInteractionService`。
    *   **程式碼進入點**：大腦內部觸發 **`onReady()`**。
    *   *此時，你的助手已經在後台「待命/註冊成功」，但尚未彈出畫面。*

---

### 階段 2：執行 `settings put secure ...` 時發生了什麼？
當你在 Console 敲下這個指令時：
```bash
settings put secure voice_interaction_service com.example.simplevoiceassistant/.MyVoiceInteractionService
```
1.  **通知管理器**：`SettingsProvider` 通知 `VoiceInteractionManagerService` 參數更新了。
2.  **滾動切換**：
    *   系統會先 **`unbind`（釋放）** 當前可能綁定的舊助手。
    *   解析你設定的 APK Manifest 檔案與 `res/xml` 下的組態檔案。
3.  **重新綁定**：
    *   連線到你的 `MyVoiceInteractionService` 並執行它的 **`onReady()`**。
    *   這跟你開機時載入的動作一模一樣，常用於動態切換、重啟或 Debug 的強制指派！

---

### 階段 3：執行 `cmd voiceinteraction show` (或遙控器觸發)
當按鍵觸發或者 Console 執行 `cmd voiceinteraction show` 時，流程會切換到 **UI 交互階段**：

## 📝 指令觸發後的「元件連動」四部曲表

當指令下達後，系統內部會像傳接球一樣，依序喚醒這四個元件：

### 1️⃣ 第一步：喚起門衛 (FrameWork Manager)
*   **觸發點**：你輸入了 `cmd voiceinteraction show` 或按下語音鍵。
*   **動作**：系統管理員（Manager）跑出來，去敲常駐在背景的 `MyVoiceInteractionService` 防波堤。
*   **白話文**：*「大腦，有人喊你了，準備幹活！」*

---

### 2️⃣ 第二步：派遣工廠 (MyVoiceInteractionSessionService)
*   **動作**：管理員解析 XML 設定，發現需要一個「視窗（Session）」。它會主動連線到 SessionService 手腳服務。
*   **呼叫函數**：**`onNewSession()`**
*   **白話文**：*建立一個全新的語音連線會話（Session 實例）。*

---

### 3️⃣ 第三步：打造畫布 (MyVoiceInteractionSession)
*   元件實例創立後，系統馬上調用它來畫出 UI 提示條。
*   **呼叫函數**：**`onCreateContentView()`**
*   **白話文**：*這時就在記憶體中畫出了「🎙️ Listening...」的藍色覆蓋方塊。*

---

### 4️⃣ 第四步：推上最前線 & 聽取指令 (onShow)
*   **動作**：系統調用 `onShow()` 通知視窗：**「你已經成功浮在電視螢幕最上層（Overlay）了」**。
*   **觸發功能**：**`startRecording()`** ➔ 建立 `AudioRecord` 背景線程。
*   **白話文**：*提示條冒出來，麥克風通道接通，開始動態連動更新文字！*

---

## 二、 函數階層與邏輯白話文

將上面圖面展開，你的程式碼實作上會有以下執行順序：

### 第一關：常駐在後台（常駐）
*   **📂 `MyVoiceInteractionService.java`**
    *   👉 **`onReady()`**：
        *   開機或設定完指令後**最先跑的人**。
        *   這是「狀態註冊」，只要它在 `dumpsys` 呈現 `mBound=true`，代表助手隨時能被喚醒。

### 第二關：按下按鍵後（動態連動）
*   **📂 `MyVoiceInteractionSessionService.java`**
    *   👉 **`onNewSession(Bundle args)`**：
        *   系統收到按鍵後，呼叫此處分配一個畫布與麥克風管理者。
*   **📂 `MyVoiceInteractionSession.java`**（這裏最重要）：
    1.  👉 **`onCreateContentView()`**：
        *   由系統主線程調用，渲染覆蓋在螢幕底下的 TextView（如藍色彈窗）。
    2.  👉 **`onShow(Bundle args, int showFlags)`**：
        *   系統在畫面渲染完、正式浮現後拋出的通知。
        *   我們在裡面加載了 **`startRecording()`** 開始撈 D-MIC 的錄音流。
    3.  👉 **`recordLoop()` (背景 Thread 連續讀取)**：
        *   當 `startRecording()` 開始後，這段迴圈會在背景跑：
        *   **`mAudioRecord.read(...)`** ➔ 算出音量 RMS ➔ **`Handler.post()`** 更新 TextView 的「在接收聲音...」提示。
    4.  👉 **`onHide()`**：
        *   如果你放開遙控器按鍵、或是調用 `hide()`，就斷開錄音防止洩漏。

---

## 💡 最終總結

*   **開機自動啟動？**
    *   是的！只要 `settings` 被指好，`MyVoiceInteractionService` 在背景常駐待命是自動的。
*   **按鍵按下去跑哪？**
    *   按鍵會讓系統去呼叫 `MyVoiceInteractionSessionService -> onNewSession()`，最後在 `MyVoiceInteractionSession` 裡的 `onShow()` 開始真正執行畫圖與聽力。

這套架構的核心精神是：**視覺視窗（Session）是動態創立、用完即丟的；只有 VIS 在背景永恆伴隨系統。**
---

## 三、 常見 Q&A 與 進階輔助說明

### Q1：為什麼開機設定會失效？（Persistence 觀念）
*   **現象**：每次重開機，都要重新輸入 `settings put secure ...` 輔助助手才有動作。
*   **原因**：AOSP 的預設設定檔（Overlay）在啟動時會覆蓋此參數。另外若開發板的 `/data` 是掛載在 RAMDisk，重開機也會遺失。
*   **AOSP 終極解法**：修改 `frameworks/base/core/res/res/values/config.xml`（或產品 overlay）下的 **`config_defaultVoiceInteractionService`**：
    ```xml
    <string name="config_defaultVoiceInteractionService">com.example.simplevoiceassistant/.MyVoiceInteractionService</string>
    ```

### Q2：`cmd voiceinteraction show` 底層是如何映射的？
當下達此指令時，背後是透過以下三層串接：
1.  **`voiceinteraction`**：系統服務路由器 (`VoiceInteractionManagerService`) 的註冊名稱。
2.  **`show`**：指令參數。解析後底層執行系統 Java API `showSessionForActiveService(...)`。
3.  **調度路徑**：管理員會從 `voice_interaction_service.xml` 中提取 **`android:sessionService`** 宣告的全類別名稱，進而綁定連線並喚醒你的 Session UI 佈局。

