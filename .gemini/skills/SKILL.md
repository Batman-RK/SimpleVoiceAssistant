# Skill: Android APK 逆向與安全性分析助手

---
Description: 此 Skill 讓 Antigravity 具備深入分析 Android APK 檔案的能力，包含反編譯引導、資訊清單稽核、代碼邏輯追蹤以及安全性漏洞掃描。
---

## Context & Triggers
- **File Extensions**: `.apk`, `.dex`, `.arsc`
- **File Names**: `AndroidManifest.xml`, `build.gradle`, `proguard-rules.pro`
- **Keywords**: "decompile", "reverse engineer", "apk analysis", "permission check"

## Commands

### /analyze_manifest
**Intent**: 分析 AndroidManifest.xml 的安全性與組件。
**Actions**:
1. 檢查 `exported="true"` 的組件是否存在安全風險。
2. 列出所有要求權限 (Uses-permissions) 並標註「危險權限」。
3. 找出所有 Deep Link 與 Intent Filter 進入點。

### /list_sensitive_assets
**Intent**: 搜尋資源資料夾中的敏感資訊。
**Actions**:
1. 掃描 `res/values/*.xml` 內的 API Keys 或硬編碼字串。
2. 檢查 `assets/` 目錄下是否有設定檔、私鑰或憑證。
3. 查找 `google-services.json` 內容。

### /trace_logic <class_name_or_keyword>
**Intent**: 追蹤特定業務邏輯（如支付、登入）。
**Actions**:
1. 在 `classes.dex` 反編譯後的程式碼中搜尋目標。
2. 解釋該類別的核心運作邏輯。
3. 判斷是否存在 R8/ProGuard 混淆並嘗試理解意圖。

### /security_audit
**Intent**: 執行 OWASP Mobile 基礎檢查。
**Actions**:
1. 檢查是否存在不安全的 `WebView` 設定（如 `setJavaScriptEnabled(true)`）。
2. 掃描是否使用過時的加密演算法（如 ECB 模式）。
3. 檢查是否存在硬編碼的伺服器網址（HTTP vs HTTPS）。

## Rules & Constraints
- **精確性**：在分析 smali 或是反編譯代碼時，必須區分推測與事實。
- **階層化**：分析結果優先呈現「高風險漏洞」，再呈現「常規資訊」。
- **工具建議**：若環境缺少工具，主動建議使用 `apktool` 進行拆解或 `jadx` 進行閱讀。

## Output Template
1. **APK 概覽** (Package Name, Version, Min/Target SDK)
2. **關鍵權限分析**
3. **安全性亮點/風險**
4. **具體建議與步驟**
