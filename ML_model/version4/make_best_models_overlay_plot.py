from __future__ import annotations

from pathlib import Path
import joblib
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

# ============================================================
# User settings
# ============================================================
BASE_DIR = Path(__file__).resolve().parent

# Results folders produced by your two main scripts
GPR_RESULTS_DIR = BASE_DIR / "results_finalv4b_gpr_loocv"
POLY_RESULTS_DIR = BASE_DIR / "results_finalv4b_polynomial_loocv"

# Exported best-model files produced by your two main scripts
BEST_GPR_MODEL_FILE = GPR_RESULTS_DIR / "best_gpr_final_model.joblib"
BEST_POLY_MODEL_FILE = POLY_RESULTS_DIR / "best_polynomial_final_model.joblib"

# Original dataset
INPUT_FILE = BASE_DIR / "finalv4b.xlsx"
SHEET_NAME = "Data"
HEADER_ROW = 7

FEATURE_COLS = ["Collagen_mg_mL", "UV_min"]
TARGET_COL = "Shear_kPa"

# Output
OUTPUT_DIR = BASE_DIR / f"results_{INPUT_FILE.stem}_best_models_overlay"
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

plt.rcParams.update({
    "font.size": 12,
    "axes.titlesize": 13,
    "axes.labelsize": 12,
    "xtick.labelsize": 11,
    "ytick.labelsize": 11,
    "legend.fontsize": 10,
})


def load_and_clean_data(input_file: Path, sheet_name: str) -> pd.DataFrame:
    df = pd.read_excel(input_file, sheet_name=sheet_name, header=HEADER_ROW)
    df = df[FEATURE_COLS + [TARGET_COL]].dropna().reset_index(drop=True)
    return df


def predict_with_gpr(artifact: dict, X: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    X_s = artifact["x_scaler"].transform(X)
    y_pred_s, y_std_s = artifact["model"].predict(X_s, return_std=True)
    y_pred = artifact["y_scaler"].inverse_transform(y_pred_s.reshape(-1, 1)).ravel()
    y_std = y_std_s * float(artifact["y_scaler"].scale_[0])
    return y_pred, y_std


def predict_with_polynomial(artifact: dict, X: np.ndarray) -> np.ndarray:
    X_s = artifact["x_scaler"].transform(X)
    X_poly = artifact["poly"].transform(X_s)
    y_pred_s = artifact["model"].predict(X_poly)
    y_pred = artifact["y_scaler"].inverse_transform(y_pred_s.reshape(-1, 1)).ravel()
    return y_pred


def build_overlay_dataframe(df: pd.DataFrame, gpr_artifact: dict, poly_artifact: dict) -> pd.DataFrame:
    X = df[FEATURE_COLS].to_numpy(dtype=float)
    y_true = df[TARGET_COL].to_numpy(dtype=float)

    gpr_pred, gpr_std = predict_with_gpr(gpr_artifact, X)
    poly_pred = predict_with_polynomial(poly_artifact, X)

    gpr_df = pd.DataFrame({
        "sample_id": np.arange(1, len(df) + 1),
        "model": gpr_artifact["model_name"],
        "family": "GPR",
        "true": y_true,
        "pred": gpr_pred,
        "pred_std": gpr_std,
        "abs_error": np.abs(y_true - gpr_pred),
        "sq_error": (y_true - gpr_pred) ** 2,
    })

    poly_df = pd.DataFrame({
        "sample_id": np.arange(1, len(df) + 1),
        "model": poly_artifact["model_name"],
        "family": "Polynomial",
        "true": y_true,
        "pred": poly_pred,
        "pred_std": np.nan,
        "abs_error": np.abs(y_true - poly_pred),
        "sq_error": (y_true - poly_pred) ** 2,
    })

    return pd.concat([gpr_df, poly_df], ignore_index=True)


def plot_overlay_true_vs_pred(overlay_df: pd.DataFrame, out_path: Path) -> None:
    gpr_df = overlay_df[overlay_df["family"] == "GPR"].copy()
    poly_df = overlay_df[overlay_df["family"] == "Polynomial"].copy()

    true_vals = overlay_df["true"].values
    vmin = float(np.min(true_vals))
    vmax = float(np.max(true_vals))
    pad = 0.10 * (vmax - vmin if vmax > vmin else 1.0)

    plt.figure(figsize=(7, 7))
    plt.scatter(gpr_df["true"], gpr_df["pred"], s=70, label=gpr_df["model"].iloc[0])
    plt.scatter(poly_df["true"], poly_df["pred"], s=70, label=poly_df["model"].iloc[0])

    plt.plot(
        [vmin - pad, vmax + pad],
        [vmin - pad, vmax + pad],
        "k--",
        linewidth=1.5,
        label="Ideal: y = x",
    )

    plt.xlim(vmin - pad, vmax + pad)
    plt.ylim(vmin - pad, vmax + pad)

    plt.xlabel("True shear modulus (kPa)")
    plt.ylabel("Predicted shear modulus (kPa)")
    plt.title("Overlay: Best GPR vs Best Polynomial")
    plt.legend()
    plt.grid(True)
    plt.tight_layout()
    plt.savefig(out_path, dpi=300)
    plt.close()


def main() -> None:
    if not BEST_GPR_MODEL_FILE.exists():
        raise FileNotFoundError(f"Best GPR model file not found: {BEST_GPR_MODEL_FILE}")
    if not BEST_POLY_MODEL_FILE.exists():
        raise FileNotFoundError(f"Best Polynomial model file not found: {BEST_POLY_MODEL_FILE}")
    if not INPUT_FILE.exists():
        raise FileNotFoundError(f"Input file not found: {INPUT_FILE}")

    df = load_and_clean_data(INPUT_FILE, SHEET_NAME)

    gpr_artifact = joblib.load(BEST_GPR_MODEL_FILE)
    poly_artifact = joblib.load(BEST_POLY_MODEL_FILE)

    overlay_df = build_overlay_dataframe(df, gpr_artifact, poly_artifact)
    overlay_df.to_csv(OUTPUT_DIR / "best_models_predictions_overlay.csv", index=False)

    plot_overlay_true_vs_pred(
        overlay_df,
        OUTPUT_DIR / "Fig_best_gpr_vs_best_polynomial_overlay_true_vs_pred.png",
    )

    summary_df = (
        overlay_df.groupby(["family", "model"], as_index=False)
        .agg(
            RMSE=("sq_error", lambda s: float(np.sqrt(np.mean(s)))),
            MAE=("abs_error", lambda s: float(np.mean(s))),
        )
    )
    summary_df.to_csv(OUTPUT_DIR / "best_models_overlay_summary.csv", index=False)

    notes = [
        f"Input file: {INPUT_FILE.name}",
        f"GPR model file: {BEST_GPR_MODEL_FILE.name}",
        f"Polynomial model file: {BEST_POLY_MODEL_FILE.name}",
        "These predictions are generated by the final exported best models fitted on the full dataset.",
        "This is not LOOCV prediction data.",
        "Output CSV: best_models_predictions_overlay.csv",
        "Output figure: Fig_best_gpr_vs_best_polynomial_overlay_true_vs_pred.png",
    ]
    (OUTPUT_DIR / "run_notes.txt").write_text("\\n".join(notes), encoding="utf-8")

    print(f"Done. Results saved to: {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
