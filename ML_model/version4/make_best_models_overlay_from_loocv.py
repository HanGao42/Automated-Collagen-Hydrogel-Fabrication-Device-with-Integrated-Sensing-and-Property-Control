from __future__ import annotations

from pathlib import Path
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

# Input result files from LOOCV runs
GPR_SUMMARY_FILE = GPR_RESULTS_DIR / "cv_summary_gpr.csv"
GPR_PRED_FILE = GPR_RESULTS_DIR / "cv_predictions_gpr.csv"

POLY_SUMMARY_FILE = POLY_RESULTS_DIR / "cv_summary_polynomial.csv"
POLY_PRED_FILE = POLY_RESULTS_DIR / "cv_predictions_polynomial.csv"

# Output
OUTPUT_DIR = BASE_DIR / "results_finalv4c_best_models_loocv_overlay"
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

plt.rcParams.update({
    "font.size": 12,
    "axes.titlesize": 13,
    "axes.labelsize": 12,
    "xtick.labelsize": 11,
    "ytick.labelsize": 11,
    "legend.fontsize": 10,
})


def load_best_model_name(summary_file: Path) -> str:
    df = pd.read_csv(summary_file)
    if df.empty:
        raise ValueError(f"Summary file is empty: {summary_file}")
    return str(df.sort_values(by=["RMSE", "MAE"], ascending=[True, True]).iloc[0]["model"])


def load_best_model_predictions(pred_file: Path, model_name: str, family_name: str) -> pd.DataFrame:
    df = pd.read_csv(pred_file)
    sub = df[df["model"] == model_name].copy()
    if sub.empty:
        raise ValueError(f"No predictions found for model {model_name} in {pred_file}")
    sub["family"] = family_name
    return sub


def build_overlay_dataframe(
    gpr_summary_file: Path,
    gpr_pred_file: Path,
    poly_summary_file: Path,
    poly_pred_file: Path,
) -> pd.DataFrame:
    best_gpr_name = load_best_model_name(gpr_summary_file)
    best_poly_name = load_best_model_name(poly_summary_file)

    gpr_df = load_best_model_predictions(gpr_pred_file, best_gpr_name, "GPR")
    poly_df = load_best_model_predictions(poly_pred_file, best_poly_name, "Polynomial")

    overlay_df = pd.concat([gpr_df, poly_df], ignore_index=True)
    return overlay_df


def plot_loocv_overlay_true_vs_pred(overlay_df: pd.DataFrame, out_path: Path) -> None:
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
    plt.title("Overlay: Best GPR vs Best Polynomial (LOOCV)")
    plt.legend()
    plt.grid(True)
    plt.tight_layout()
    plt.savefig(out_path, dpi=300)
    plt.close()


def compute_summary(overlay_df: pd.DataFrame) -> pd.DataFrame:
    summary_rows = []
    for (family, model), sub in overlay_df.groupby(["family", "model"]):
        rmse = float(np.sqrt(np.mean(sub["sq_error"].to_numpy(dtype=float))))
        mae = float(np.mean(sub["abs_error"].to_numpy(dtype=float)))
        y_true = sub["true"].to_numpy(dtype=float)
        y_pred = sub["pred"].to_numpy(dtype=float)

        ss_res = float(np.sum((y_true - y_pred) ** 2))
        ss_tot = float(np.sum((y_true - np.mean(y_true)) ** 2))
        r2 = float(1 - ss_res / ss_tot) if ss_tot > 0 else np.nan

        summary_rows.append({
            "family": family,
            "model": model,
            "RMSE": rmse,
            "MAE": mae,
            "R2": r2,
            "n_samples": len(sub),
        })

    return pd.DataFrame(summary_rows).sort_values(by=["RMSE", "MAE"], ascending=[True, True]).reset_index(drop=True)


def main() -> None:
    required_files = [
        GPR_SUMMARY_FILE, GPR_PRED_FILE,
        POLY_SUMMARY_FILE, POLY_PRED_FILE,
    ]
    for f in required_files:
        if not f.exists():
            raise FileNotFoundError(f"Required file not found: {f}")

    overlay_df = build_overlay_dataframe(
        GPR_SUMMARY_FILE,
        GPR_PRED_FILE,
        POLY_SUMMARY_FILE,
        POLY_PRED_FILE,
    )
    overlay_df.to_csv(OUTPUT_DIR / "best_models_loocv_predictions_overlay.csv", index=False)

    plot_loocv_overlay_true_vs_pred(
        overlay_df,
        OUTPUT_DIR / "Fig_best_gpr_vs_best_polynomial_loocv_overlay_true_vs_pred.png",
    )

    summary_df = compute_summary(overlay_df)
    summary_df.to_csv(OUTPUT_DIR / "best_models_loocv_overlay_summary.csv", index=False)

    best_gpr_name = overlay_df[overlay_df["family"] == "GPR"]["model"].iloc[0]
    best_poly_name = overlay_df[overlay_df["family"] == "Polynomial"]["model"].iloc[0]

    notes = [
        "This overlay plot compares the generalization performance of the best GPR model and the best Polynomial model.",
        "The data in this folder come from LOOCV prediction files, so each point is an out-of-sample prediction.",
        f"Best GPR model: {best_gpr_name}",
        f"Best Polynomial model: {best_poly_name}",
        "Output CSV: best_models_loocv_predictions_overlay.csv",
        "Output figure: Fig_best_gpr_vs_best_polynomial_loocv_overlay_true_vs_pred.png",
    ]
    (OUTPUT_DIR / "run_notes.txt").write_text("\n".join(notes), encoding="utf-8")

    print(f"Done. Results saved to: {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
