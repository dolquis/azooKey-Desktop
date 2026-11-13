# Rich Features 仕様（横断テーマ X-1〜X-4）の転送表

本書は、横断テーマ X-1〜X-4（ライブ変換・AI 予測・誤変換訂正のリッチ化）の項を、所有する spec へ移した後の入口である。
各項の契約は移動先が正典で、本書は内容を持たない。`X-N-M` 形の参照は、次の表で移動先の節に解決する。
各テーマの実装タイミングは `plans/windows-port-roadmap.md`「横断テーマと Phase の対応」が正典である。

## 転送表

| 旧節 | 移動先 |
|---|---|
| X-1-1 | `docs/legacy-parity-spec.md` §2.6 |
| X-1-2 | `docs/copilot-pc-backend-spec.md` §11.1 |
| X-1-3 | `docs/copilot-pc-backend-spec.md` §11.2 |
| X-2-1 | `docs/legacy-parity-spec.md` §3.8 |
| X-2-2 | `docs/legacy-parity-spec.md` §3.8 |
| X-2-3 | `docs/legacy-parity-spec.md` §3.9 |
| X-2-6 | `docs/app-profile-spec.md` §6 |
| X-2-7 | `docs/learning-data-management-spec.md` §15 |
| X-3-1 | `docs/typo-correction-learning-spec.md` §12.4.6 |
| X-3-2 | `docs/user-learning-enhancement-spec.md` §4.3 |
| X-3-3 | `docs/ai-backend-spec.md` §4.3 |
| X-3-4 | `docs/proofread-view-spec.md` §1 |
| X-3-5 | `docs/user-learning-enhancement-spec.md` §4.2 |
| X-3-6 | `docs/proofread-view-spec.md` §2 |
| X-3-7 | `docs/typo-correction-learning-spec.md` §1 |
| X-4-1 | `docs/copilot-pc-backend-spec.md` §11.3 |
| X-4-4 | `docs/privacy-and-secure-input-spec.md` §4.2 |
| X-4-5 | `docs/conversion-quality-benchmark-spec.md` §3.1 |

LLM 予測（`PredictWithLLM`）、ストリーミング予測の push（`PredictStreamChunk`）、片方向 push IPC は、
コードと担当の Issue を持たない設計案なので、どの spec にも移していない。
