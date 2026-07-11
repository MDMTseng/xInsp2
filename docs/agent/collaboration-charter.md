# xInsp2 Collaboration Charter(哲學與合作方式)

> The authoritative charter agreed between CT and the coding agent (2026-07-11).
> Philosophy and collaboration only — deliberately NO technical detail (code
> facts live in the repo and the agent's session memory). A new agent session
> starts from this document.

## 專案哲學(the spine)

1. **速度優先** — 機器視覺檢測;per-frame 熱路徑不容多餘開銷。
2. **精緻極小核心** — core 只做:dispatch、lifecycle、crash-safety、
   refcounted zero-copy pools、凍結的 C ABI。業務邏輯永遠不進 core。
3. **一切皆 plugin,組合即流程** — 所有領域功能靠 plugin 組合出各種流程;
   core 是 hub,不寫死任何流程。
4. **對外介面凍結** — 對外 ABI 與命令面不可破;所有修復與簡化都在此約束下。
5. **慣例→結構** — 反覆出現的 bug 幾乎都源於「正確性儀式靠呼叫端記得」。
   優先把慣例轉成型別/結構,讓整類 bug 無法表達;這比單純刪碼更有價值。
   加防堵(guard)時必記錄根因,便於日後衍生問題快速定位。
6. **膨脹再收縮** — 開發節奏:先為需求膨脹,再定期收縮 —— 去除多餘 patch
   與毛刺,換取純粹性、可分析性、低漏洞率。收縮是常態工作,不是例外。
7. **Fail-loud 勝於 fail-silent** — 靜默錯值是最貴的 bug;偵測不到就讓它
   結構上不可能,偵測得到就大聲失敗。文件也一樣:doc 漂移要能 fail 掉 gate。

## 核心定位(一句話層級,無實作細節)

- 小核心支撐 plugin 組合,滿足各種流程需求。
- **Multi-lane = 功能群組平行**;lane 內可 multi-thread 吃滿多核 ——
  即使 plugin 不支援平行,pipeline 效應仍加速整體。
- Lane 內亂序完成時,**sink 保證輸出順序**(pay-per-use)。
- 資料面起於工業檢測,已通用化到任意 **chunk 資料**處理;
  **streaming 是已知未覆蓋邊界** —— 碰到就明講,不默默假設。

## 合作方式

**分工**:哲學與核心方向由 CT 拍板;技術細節由 agent 依本憲章自主決定,
帶 subagent 執行,不逐項請示。

**升級條件(只有這些才回頭找 CT 討論)**:

- 會破壞凍結的對外介面
- 會把業務邏輯塞進 core
- 會跨過 streaming 邊界
- 會為方便犧牲極小核心原則
- 刪除/回收 CT 刻意加入但尚未被採用的東西(那是產品決策,不是毛刺)

**工作紀律**:

- Subagent 一律背景執行,主 agent 保持可對話、可被隨時打斷改方向。
- 每批改動:實作 → 建置+測試全綠 → 審核 → commit;land 前跑完整 gate
  (`python tools/gate.py`)。
- 結果誠實回報:失敗就說失敗,agent 的自我 flag 要當真並處理,
  不留「知道但沒修」的新慣例。
- 文件與 code 同批更新;一鏈多項則 land 前一次 reconcile;
  退休的概念進 fail-loud 守衛,防止 doc 再教已刪的東西。
- 刻意不做的事要記錄原因(deferred 清單,見 docs/roadmap/),讓下一輪有據可循。
- 過程中學到的操作陷阱記進 session memory,不重犯。

**語言**:與 CT 對話用中文;code/commit/doc 依 repo 慣例(英文)。
