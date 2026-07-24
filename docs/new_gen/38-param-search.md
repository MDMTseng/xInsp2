# 38 — Param Search(參數自動最佳化)— DESIGN ONLY

Status: **設計記錄,尚未實作**(2026-07-24 對話產出)
Prereq reading: 37-pluginlet-model.md(controls plet / `$schema`)

## 動機

檢驗參數(threshold、blurriness、caliper settings…)目前靠人工調。
已有人工設定的起始參數時,想要自動搜尋:**margin 最大化 / 誤檢率最小化**。

## 核心洞察:`$schema` 就是現成的搜尋空間定義

參數搜尋器最麻煩的是「哪些參數可動、範圍、步進、連續或離散」。
controls plet 的宣告已經把這些說完了,optimizer 是「宣告一次」的
**第四個消費者**(build、webui、runtime 之後):

| `$schema` widget | 搜尋空間解讀 |
|---|---|
| slider / numpad / stepper | 連續或格點變數,bounds = `min/max`,格點 = `step` |
| dropdown / radio | categorical(`options`) |
| toggle | binary |
| range(key + key2) | 帶 `low ≤ high` 約束的變數對 |
| readout / view / 純展示 | **不是**變數,自動排除 |

任何用了 controls plet 的 plugin,`get_instance_def` 回傳的 `$schema`
就是機器可讀的搜尋空間 —— 不需要 per-plugin 額外配合。native `set_def`
會 clamp/驗證,搜尋器丟出界的值也不會弄壞狀態(37 的「約束宣告一次、
兩側執行」在這裡第三次派上用場)。

## 兩條 param 來源的分界(使用者提出的疑慮 → 其實剛好不衝突)

plugin 的參數有兩條來源:

1. **def 那條**(webui / `set_instance_def`)— 人在調的
2. **隨資料輸入那條**(exchange / 輸入 payload)— 上游 per-run 決定的

要最佳化的是 ①,而 ① 正好是有 API、有 schema、有驗證的那條 ——
`set_instance_def` 任何 client 都能呼叫,不限 webui。② 本來就不是
「可調參數」而是「輸入資料」:在搜尋裡它屬於 dataset(固定重放),
不是變數。分界線:**def = 可搜尋的,input = 固定重放的**。

## 搜尋迴圈(概念)

```
snapshot 原始 def(人工起始參數)
loop:
  candidate ← optimizer 提議(從 $schema 建的空間)
  set_instance_def(instance, candidate)
  對 labeled dataset 重放每張圖 → 收 report/結果
  score = 誤檢率 / margin(離判定門檻的距離)
optimizer 更新 → 收斂或超過 budget
restore 或 commit 最佳 def
```

演算法:有人工起始點 + 低維(通常 <10 參數)+ 評估昂貴(每 candidate
跑整個 dataset)→ **Nelder-Mead 或 Bayesian optimization(Optuna)**,
不是 grid search。`step` 格點與 categorical Optuna 原生支援。

## 形態決定:第一版是 script/tool,不是 plugin

使用者原始構想是一個「param search plugin」(有輸入輸出關係、輸出
設定給其他 plugin)。第一版**不**建議做成 plugin:

- 搜尋器是**指揮者**(設別的 instance 的 def、觸發 run、收結果);
  plugin 目前沒有跨 instance 控制權,開這個權力是不小的架構決定。
- 節奏是「離線跑幾百次實驗」,不是 pipeline 節點。
- Python SDK(xinsp2)今天就有全部所需 API:get/set_instance_def、
  run、結果讀取 → 一個 `xi param-search` driver 馬上能動。

證明有價值後,再考慮升格為 service 內建功能或 extension(屆時再
處理權限模型)。

## 注意事項 / 未決

- **live def 會被改動**:要嘛 snapshot/restore,要嘛在複製的 project
  上跑搜尋。不 sandbox 就跑等於拿產線設定做實驗。
- **objective 需要 labeled dataset**(NG/OK 標註)——格式、來源未定,
  這是唯一 controls plet 幫不上忙的部分。
- margin 的定義(離判定門檻的距離如何從 report 取得)未定。
- 多 instance 聯合搜尋(搜尋空間 = 多個 plugin 的 def 串接)是自然
  延伸,第一版先單 instance。
