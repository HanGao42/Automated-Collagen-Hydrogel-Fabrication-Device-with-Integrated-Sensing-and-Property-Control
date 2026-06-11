from __future__ import annotations

from pathlib import Path
import joblib
import numpy as np
import pandas as pd
from sklearn.linear_model import LinearRegression, Ridge
from sklearn.preprocessing import PolynomialFeatures, StandardScaler

from common_surrogate_utils import (
    fit_full_polynomial_model,
    get_loocv_predictions,
    load_and_clean_data,
    plot_metric_bar,
    plot_predicted_vs_true,
    polynomial_coefficients_to_raw_basis,
    sanitize_name,
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

RIDGE_ALPHA_DEG4 = 1.0


def build_polynomial_models() -> dict[str, dict]:
    return {
        "Poly1_Linear": {"degree": 1, "reg": "linear", "alpha": None},
        "Poly2_Linear": {"degree": 2, "reg": "linear", "alpha": None},
        "Poly3_Linear": {"degree": 3, "reg": "linear", "alpha": None},
        "Poly4_Ridge": {"degree": 4, "reg": "ridge", "alpha": RIDGE_ALPHA_DEG4},
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

    poly = PolynomialFeatures(degree=spec["degree"], include_bias=False)
    X_train_poly = poly.fit_transform(X_train_s)
    X_val_poly = poly.transform(X_val_s)

    if spec["reg"] == "linear":
        model = LinearRegression()
    elif spec["reg"] == "ridge":
        model = Ridge(alpha=float(spec["alpha"]))
    else:
        raise ValueError(f"Unknown reg type: {spec['reg']}")

    model.fit(X_train_poly, y_train_s)
    y_pred_s = model.predict(X_val_poly)
    y_pred = y_scaler.inverse_transform(y_pred_s.reshape(-1, 1)).ravel()

    return {
        "y_pred": y_pred,
        "y_std": np.full(shape=(len(y_pred),), fill_value=np.nan),
    }


def export_full_fit_outputs(
    X: np.ndarray,
    y: np.ndarray,
    model_specs: dict[str, dict],
    summary_df,
    out_dir: Path,
) -> None:
    for model_name, spec in model_specs.items():
        full_fit = fit_full_polynomial_model(X, y, spec["degree"], spec["reg"], spec["alpha"])

        poly = full_fit["poly"]
        model = full_fit["model"]
        x_scaler = full_fit["x_scaler"]
        y_scaler = full_fit["y_scaler"]

        coef_std = model.coef_.ravel()
        intercept_std = float(model.intercept_)

        std_df = pd.DataFrame({
            "term_std_basis": list(poly.get_feature_names_out(FEATURE_COLS)) + ["Intercept"],
            "coefficient_std_basis": list(coef_std) + [intercept_std],
        })
        std_df.to_csv(
            out_dir / f"{sanitize_name(model_name)}_coefficients_standardized_basis.csv",
            index=False,
        )

        raw_df = polynomial_coefficients_to_raw_basis(
            poly=poly,
            coef_std=coef_std,
            intercept_std=intercept_std,
            x_scaler=x_scaler,
            y_scaler=y_scaler,
            feature_names=FEATURE_COLS,
        )
        raw_df.to_csv(
            out_dir / f"{sanitize_name(model_name)}_coefficients_raw_basis.csv",
            index=False,
        )

        (
            raw_df.assign(abs_coeff=raw_df["coefficient_raw_units"].abs())
            .sort_values(by="abs_coeff", ascending=False)
            .drop(columns="abs_coeff")
            .to_csv(
                out_dir / f"{sanitize_name(model_name)}_coefficients_raw_basis_sorted.csv",
                index=False,
            )
        )

    best_row = summary_df.sort_values(by="RMSE").iloc[0]
    best_name = best_row["model"]
    best_spec = model_specs[best_name]

    best_fit = fit_full_polynomial_model(
        X, y,
        best_spec["degree"],
        best_spec["reg"],
        best_spec["alpha"],
    )

    artifact = {
        "model_name": best_name,
        "feature_cols": FEATURE_COLS,
        "target_col": TARGET_COL,
        "x_scaler": best_fit["x_scaler"],
        "y_scaler": best_fit["y_scaler"],
        "poly": best_fit["poly"],
        "model": best_fit["model"],
        "degree": best_spec["degree"],
        "regularization": best_spec["reg"],
        "alpha": best_spec["alpha"],
    }
    joblib.dump(artifact, out_dir / "best_polynomial_final_model.joblib")


def main() -> None:
    if not INPUT_FILE.exists():
        raise FileNotFoundError(f"Input file not found: {INPUT_FILE}")

    out_dir = setup_output_dir(OUTPUT_ROOT, INPUT_FILE, "polynomial_loocv")

    data = load_and_clean_data(INPUT_FILE, SHEET_NAME, FEATURE_COLS, TARGET_COL)
    data.to_csv(out_dir / "cleaned_model_data.csv", index=False)

    X = data[FEATURE_COLS].to_numpy(dtype=float)
    y = data[TARGET_COL].to_numpy(dtype=float)

    model_specs = build_polynomial_models()
    pred_df, summary_df = get_loocv_predictions(X, y, model_specs, fit_predict_single_fold)

    pred_df.to_csv(out_dir / "cv_predictions_polynomial.csv", index=False)
    summary_df.to_csv(out_dir / "cv_summary_polynomial.csv", index=False)

    plot_metric_bar(
        summary_df,
        "RMSE",
        "Polynomial models: RMSE comparison (LOOCV)",
        out_dir / "Fig_Polynomial_RMSE.png",
    )
    plot_metric_bar(
        summary_df,
        "MAE",
        "Polynomial models: MAE comparison (LOOCV)",
        out_dir / "Fig_Polynomial_MAE.png",
    )
    plot_predicted_vs_true(
        pred_df,
        "Polynomial models: Predicted vs true (LOOCV)",
        out_dir / "Fig_Polynomial_predicted_vs_true.png",
    )

    export_full_fit_outputs(X, y, model_specs, summary_df, out_dir)

    best_row = summary_df.sort_values(by="RMSE").iloc[0]
    save_run_notes(out_dir, [
        f"Input file: {INPUT_FILE.name}",
        f"Number of valid rows used: {len(data)}",
        "Validation method: LOOCV",
        f"Features: {FEATURE_COLS}",
        f"Target: {TARGET_COL}",
        "Scaling approach: fold-wise StandardScaler on X and y during LOOCV.",
        "Polynomial coefficient files are saved in both standardized basis and raw-unit basis.",
        f"Best Polynomial model by RMSE: {best_row['model']} (RMSE={best_row['RMSE']:.6f}, MAE={best_row['MAE']:.6f}, R2={best_row['R2']:.6f})",
        "Exported model file: best_polynomial_final_model.joblib",
    ])

    print(f"Done. Results saved to: {out_dir}")


if __name__ == "__main__":
    main()