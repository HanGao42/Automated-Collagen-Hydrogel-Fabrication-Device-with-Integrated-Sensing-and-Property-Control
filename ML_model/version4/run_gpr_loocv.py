from __future__ import annotations

from pathlib import Path
import joblib
import numpy as np
from sklearn.gaussian_process import GaussianProcessRegressor
from sklearn.gaussian_process.kernels import ConstantKernel as C, Matern, RBF, RationalQuadratic
from sklearn.preprocessing import StandardScaler

from common_surrogate_utils import (
    get_loocv_predictions,
    load_and_clean_data,
    plot_metric_bar,
    plot_predicted_vs_true,
    save_run_notes,
    setup_output_dir,
)

# ============================================================
# User settings
# ============================================================
BASE_DIR = Path(__file__).resolve().parent
FILE_NAME = "finalv4b.xlsx"
INPUT_FILE = BASE_DIR / FILE_NAME
SHEET_NAME = "Data"
OUTPUT_ROOT = BASE_DIR

FEATURE_COLS = ["Collagen_mg_mL", "UV_min"]
TARGET_COL = "Shear_kPa"

RANDOM_STATE = 42
GPR_ALPHA = 1e-6
N_RESTARTS = 5


def build_gpr_models() -> dict[str, dict]:
    return {
        "GPR_RBF": {
            "kernel": C(1.0, (1e-3, 1e3)) * RBF(
                length_scale=[1.0, 1.0],
                length_scale_bounds=(1e-3, 1e3),
            )
        },
        "GPR_Matern05": {
            "kernel": C(1.0, (1e-3, 1e3)) * Matern(
                length_scale=[1.0, 1.0],
                length_scale_bounds=(1e-3, 1e3),
                nu=0.5,
            )
        },
        "GPR_Matern15": {
            "kernel": C(1.0, (1e-3, 1e3)) * Matern(
                length_scale=[1.0, 1.0],
                length_scale_bounds=(1e-3, 1e3),
                nu=1.5,
            )
        },
        "GPR_Matern25": {
            "kernel": C(1.0, (1e-3, 1e3)) * Matern(
                length_scale=[1.0, 1.0],
                length_scale_bounds=(1e-3, 1e3),
                nu=2.5,
            )
        },
        "GPR_RationalQuadratic": {
            "kernel": C(1.0, (1e-3, 1e3)) * RationalQuadratic(
                length_scale=1.0,
                alpha=1.0,
            )
        },
    }


def fit_predict_single_fold(
    model_name: str,
    spec: dict,
    X_train: np.ndarray,
    y_train: np.ndarray,
    X_val: np.ndarray,
) -> dict:
    x_scaler = StandardScaler()
    y_scaler = StandardScaler()

    X_train_s = x_scaler.fit_transform(X_train)
    X_val_s = x_scaler.transform(X_val)
    y_train_s = y_scaler.fit_transform(y_train.reshape(-1, 1)).ravel()

    model = GaussianProcessRegressor(
        kernel=spec["kernel"],
        alpha=GPR_ALPHA,
        normalize_y=False,
        n_restarts_optimizer=N_RESTARTS,
        random_state=RANDOM_STATE,
    )
    model.fit(X_train_s, y_train_s)

    y_pred_s, y_std_s = model.predict(X_val_s, return_std=True)
    y_pred = y_scaler.inverse_transform(y_pred_s.reshape(-1, 1)).ravel()
    y_std = y_std_s * float(y_scaler.scale_[0])

    return {
        "y_pred": y_pred,
        "y_std": y_std,
    }


def fit_and_export_best_gpr(
    X: np.ndarray,
    y: np.ndarray,
    summary_df,
    model_specs: dict[str, dict],
    out_dir: Path,
) -> None:
    best_row = summary_df.sort_values(by="RMSE").iloc[0]
    best_name = best_row["model"]
    best_spec = model_specs[best_name]

    x_scaler = StandardScaler()
    y_scaler = StandardScaler()

    X_s = x_scaler.fit_transform(X)
    y_s = y_scaler.fit_transform(y.reshape(-1, 1)).ravel()

    model = GaussianProcessRegressor(
        kernel=best_spec["kernel"],
        alpha=GPR_ALPHA,
        normalize_y=False,
        n_restarts_optimizer=N_RESTARTS,
        random_state=RANDOM_STATE,
    )
    model.fit(X_s, y_s)

    artifact = {
        "model_name": best_name,
        "feature_cols": FEATURE_COLS,
        "target_col": TARGET_COL,
        "x_scaler": x_scaler,
        "y_scaler": y_scaler,
        "model": model,
        "learned_kernel": str(model.kernel_),
    }
    joblib.dump(artifact, out_dir / "best_gpr_final_model.joblib")


def main() -> None:
    if not INPUT_FILE.exists():
        raise FileNotFoundError(f"Input file not found: {INPUT_FILE}")

    out_dir = setup_output_dir(OUTPUT_ROOT, INPUT_FILE, "gpr_loocv")

    data = load_and_clean_data(INPUT_FILE, SHEET_NAME, FEATURE_COLS, TARGET_COL)
    data.to_csv(out_dir / "cleaned_model_data.csv", index=False)

    X = data[FEATURE_COLS].to_numpy(dtype=float)
    y = data[TARGET_COL].to_numpy(dtype=float)

    model_specs = build_gpr_models()
    pred_df, summary_df = get_loocv_predictions(X, y, model_specs, fit_predict_single_fold)

    pred_df.to_csv(out_dir / "cv_predictions_gpr.csv", index=False)
    summary_df.to_csv(out_dir / "cv_summary_gpr.csv", index=False)

    plot_metric_bar(
        summary_df,
        "RMSE",
        "GPR kernels: RMSE comparison (LOOCV)",
        out_dir / "Fig_GPR_RMSE.png",
    )
    plot_metric_bar(
        summary_df,
        "MAE",
        "GPR kernels: MAE comparison (LOOCV)",
        out_dir / "Fig_GPR_MAE.png",
    )
    plot_predicted_vs_true(
        pred_df,
        "GPR kernels: Predicted vs true (LOOCV)",
        out_dir / "Fig_GPR_predicted_vs_true.png",
    )

    fit_and_export_best_gpr(X, y, summary_df, model_specs, out_dir)

    best_row = summary_df.sort_values(by="RMSE").iloc[0]
    save_run_notes(out_dir, [
        f"Input file: {INPUT_FILE.name}",
        f"Number of valid rows used: {len(data)}",
        "Validation method: LOOCV",
        f"Features: {FEATURE_COLS}",
        f"Target: {TARGET_COL}",
        "Scaling approach: fold-wise StandardScaler on X and y during LOOCV.",
        f"Best GPR model by RMSE: {best_row['model']} (RMSE={best_row['RMSE']:.6f}, MAE={best_row['MAE']:.6f}, R2={best_row['R2']:.6f})",
        "Exported model file: best_gpr_final_model.joblib",
    ])

    print(f"Done. Results saved to: {out_dir}")


if __name__ == "__main__":
    main()