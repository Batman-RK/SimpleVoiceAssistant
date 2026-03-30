# Android Audio Policy Configuration 分析筆記

這份筆記詳細解釋了 `audio_policy_configuration.xml` 的作用與內部結構。這個檔案是 Android 系統中決定「聲音該怎麼走」的大腦，它定義了所有的硬體設備、音訊格式以及軟體與硬體之間的橋樑。

---

## 1. 核心概念與角色

在解析 XML 之前，必須先了解 Audio Policy 中的幾個核心名詞：

*   **`module` (硬體模組)**: 對應底層加載的一個 HAL (Hardware Abstraction Layer) 庫，例如 `primary` (主機板內建音效)、`a2dp` (藍牙)、`usb` (外接 USB 音效)。
*   **`devicePort` (硬體端)**: 真正的物理硬體裝置。
    *   `role="source"`: 提供聲音的硬體 (如：`AUDIO_DEVICE_IN_BUILTIN_MIC`, `USB Device In`)
    *   `role="sink"`: 接收聲音的硬體 (如：`AUDIO_DEVICE_OUT_SPEAKER`, `USB Device Out`)
*   **`mixPort` (軟體端/混音端)**: 代表 Android 系統 `AudioFlinger` 中的混音器或音軌端點。這是 Java App 與硬體溝通的虛擬窗口。
    *   `role="sink"`: 接收來自硬體錄音的資料 (對應 App 的錄音行為)。
    *   `role="source"`: 提供資料給硬體播放 (對應 App 的播放行為)。
*   **`route` (路由)**: 負責將 `mixPort` 與 `devicePort` 連接起來的「水管」。

👉 **簡單來說，錄音的流程是： `devicePort (硬體麥克風)` -> `route (路由)` -> `mixPort (軟體介面)` -> `AudioFlinger` -> `Java App (AudioRecord)`**

---

## 2. 逐段解析您的 `audio_policy_configuration.xml`

### A. 全局與模組宣告
```xml
<audioPolicyConfiguration version="7.1">
    <modules>
        <module name="primary" halVersion="2.0">
```
*   這宣告了主要音效模組 (`primary`)，通常對應開機時最先載入的 HAL 庫 (`audio.primary.default.so` 或 Realtek 專屬的 `audio.primary.rtk.so`)。後續所有定義都在這個大框架下。

### B. `mixPorts` (定義軟體能力)
您可以看到許多 `<mixPort>` 區段，它們定義了 Android 系統「宣稱」支援什麼格式。
```xml
<mixPort name="primary input" role="sink"> ... </mixPort>
<mixPort name="usb_device_input" role="sink"/>
<mixPort name="built-in mic" role="sink"> ... </mixPort>
<mixPort name="echo reference" role="sink"> ... </mixPort>
```
*   **`primary input`**: 最標準的錄音通道，通常處理 16-bit, 48kHz 等標準格式。
*   **`built-in mic`**: 專門為內建麥克風開的通道，這裡特別定為 32-bit, 16kHz (這通常是硬體 DSP 要求的規格)。
*   **`echo reference`**: **這是 AEC 的靈魂所在！** 這個虛擬的 mixPort 專門用來接收系統的回音參考音(也就是喇叭正在播出的聲音)，供 AEC 演算法對消。
*   **`usb_device_input`**: 專門為了 USB 麥克風開的接收端。

### C. `devicePorts` (定義實體硬體)
系統列出了它認識的實體裝置：
```xml
<devicePort tagName="Speaker" role="sink" type="AUDIO_DEVICE_OUT_SPEAKER"> ... </devicePort>
<devicePort tagName="Built-In Mic" role="source" type="AUDIO_DEVICE_IN_BUILTIN_MIC"> ... </devicePort>
<devicePort tagName="USB Device In" role="source" type="AUDIO_DEVICE_IN_USB_DEVICE"/>
<devicePort tagName="Echo Reference" role="source" type="AUDIO_DEVICE_IN_ECHO_REFERENCE"> ... </devicePort>
```
*   系統會根據這些標籤 (`tagName`) 去尋找接上的硬體。例如當插上 USB 麥克風時，它會被歸類為 `USB Device In`。

### D. `routes` (連連看：接通管線)
這是最重要的一環，決定了哪個硬體可以把聲音送到哪個軟體通道。
```xml
<route type="mix" sink="primary input" sources="Wired Headset Mic"/>
<route type="mix" sink="built-in mic" sources="Built-In Mic"/>
<route type="mix" sink="usb_device_input" sources="USB Device In,USB Headset In"/>
<route type="mix" sink="echo reference" sources="Echo Reference"/>
```
*   **第 3 行**：把實體硬體 `USB Device In` 接到了軟體介面 `usb_device_input`。
*   👉 這代表當您使用 USB 麥克風錄音時，聲音是走 `usb_device_input` 這條水管進入系統的。

---

## 3. 為什麼 `MIC` Source 會導致 AEC 失敗？(與 XML 的關聯)

您之前遇到「改成 `MIC` 能啟動 AEC，但沒消音效果 (沒有呼叫 ProcessReverse)」的問題，其根源通常就藏在 Audio Policy 的選徑規則中。

在 Android 的 `AudioPolicyManager` (C++ 核心) 處理這份 XML 時，會有一套隱藏的規則來決定是否要給這條 `mixPort` 提供 `Echo Reference` (回音參考)。

1. **`VOICE_COMMUNICATION` 的特權**：
   當 App 指定 Source 為 `VOICE_COMMUNICATION` 時，AudioPolicy 會在尋找 `route` 的過程中，**強制**將 `echo reference` 這條管線的分支「接」到當前的錄音通道上。這就是為什麼正常的 VoIP App 一定得用這個 Source。

2. **`MIC` 的捷徑 (Fast Capture)**：
   當您使用一般的 `MIC` Source 時，AudioPolicy 認為您只是想錄下環境音 (例如使用錄音機 App)，它只會尋找最直接、最短的管線。對於接在 `usb_device_input` 的 USB 麥克風，系統判斷它不需要經過複雜的音效處理 (甚至可能啟動了所謂的 Fast Track)，因此**絕對不會**把 `echo reference` 給接過來。

### 結論
這份 XML 清楚地將 `USB Device In` 和 `Built-In Mic` 與 `Echo Reference` 分成三條不同的管線接進系統。
- 您若要消除電視聲，就必定需要這三條管線能在軟體層匯合。
- 唯一能讓 Android 主動去把 `Echo Reference` 匯合進來的咒語，就是把 Java 的 AudioSource 設為 **`VOICE_COMMUNICATION`**。
- 若 `VOICE_COMMUNICATION` 會因為 `audio_effects.xml` 的自動掛載而當機/回傳 `null`，唯一的解法就是去 `audio_effects.xml` 把自動掛載那段 (`<preprocess>...`) 殺掉，然後全權交由 Java `mAec.setEnabled(true)` 控制。
