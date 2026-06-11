from __future__ import annotations

from pathlib import Path
import warnings

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import sympy as sp
from sklearn.exceptions import ConvergenceWarning
from sklearn.linear_model import LinearRegression, Ridge
from sklearn.metrics import mean_absolute_error, mean_squared_error, r2_score
from sklearn.model_selection import LeaveOneOut
from sklearn.preprocessing import PolynomialFeatures, StandardScaler

warnings.filterwarnings("ignore", category=ConvergenceWarning)


def sanitize_name(name: str) -> str:
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in name)


def rmse(y_true: np.ndarray, y_pred: np.ndarray) -> float:
    return float(np.sqrt(mean_squared_error(y_true, y_pred)))


def load_and_clean_data(
    input_file: Path,
    sheet_name: str,
    feature_cols: list[str],
    target_col: str,
    header_row: int = 7,
) -> pd.DataFrame:
    """
    Load Excel data and keep only the required columns.

    header_row=7 means the real column header is on Excel row 8.
    """
    df = pd.read_excel(input_file, sheet_name=sheet_name, header=header_row)

    required_cols = feature_cols + [target_col]
    missing = [c for c in required_cols if c not in df.columns]
    if missing:
        raise ValueError(f"Missing required columns: {missing}")

    return df[required_cols].dropna().reset_index(drop=True)


def setup_output_dir(output_root: Path, input_file: Path, suffix: str) -> Path:
    out_dir = output_root / f"results_{sanitize_name(input_file.stem)}_{suffix}"
    out_dir.mkdir(parents=True, exist_ok=True)
    return out_dir


def get_loocv_predictions(
    X: np.ndarray,
    y: np.ndarray,
    model_specs: dict[str, dict],
    fit_predict_fn,
) -> tuple[pd.DataFrame, pd.DataFrame]:
    """
    Generic LOOCV runner.

    fit_predict_fn signature:
        fit_predict_fn(model_name, spec, X_train, y_train, X_val) -> dict

    Required return keys:
        {
            "y_pred": np.ndarray,
            "y_std": np.ndarray or np.nan array
        }
    """
    loo = LeaveOneOut()
    prediction_rows = []
    summary_rows = []

    for model_name, spec in model_specs.items():
        y_true_all = []
        y_pred_all = []

        for fold, (train_idx, val_idx) in enumerate(loo.split(X), start=1):
            X_train = X[train_idx]
            y_train = y[train_idx]
            X_val = X[val_idx]
            y_val = y[val_idx]

            result = fit_predict_fn(model_name, spec, X_train, y_train, X_val)

            pred = float(result["y_pred"][0])
            std = float(result["y_std"][0]) if not np.isnan(result["y_std"][0]) else np.nan
            truth = float(y_val[0])

            y_true_all.append(truth)
            y_pred_all.append(pred)

            prediction_rows.append({
                "fold": fold,
                "model": model_name,
                "true": truth,
                "pred": pred,
                "pred_std": std,
                "abs_error": abs(truth - pred),
                "sq_error": (truth - pred) ** 2,
            })

        y_true_arr = np.array(y_true_all)
        y_pred_arr = np.array(y_pred_all)

        summary_rows.append({
            "model": model_name,
            "RMSE": rmse(y_true_arr, y_pred_arr),
            "MAE": float(mean_absolute_error(y_true_arr, y_pred_arr)),
            "R2": float(r2_score(y_true_arr, y_pred_arr)),
            "n_samples": len(y_true_arr),
        })

    pred_df = pd.DataFrame(prediction_rows)
    summary_df = pd.DataFrame(summary_rows).sort_values(
        by=["RMSE", "MAE"],
        ascending=[True, True],
    ).reset_index(drop=True)

    return pred_df, summary_df


def plot_metric_bar(summary_df: pd.DataFrame, metric: str, title: str, out_path: Path) -> None:
    df = summary_df.sort_values(by=metric, ascending=True)

    plt.figure(figsize=(8, 4.8))
    plt.bar(df["model"], df[metric])
    plt.ylabel(metric)
    plt.title(title)
    plt.xticks(rotation=30, ha="right")
    plt.tight_layout()
    plt.savefig(out_path, dpi=300)
    plt.close()


def plot_predicted_vs_true(pred_df: pd.DataFrame, title: str, out_path: Path) -> None:
    plt.figure(figsize=(7.0, 5.5))

    true_vals = pred_df["true"].values
    vmin = float(np.min(true_vals))
    vmax = float(np.max(true_vals))
    pad = 0.10 * (vmax - vmin if vmax > vmin else 1.0)

    for model in pred_df["model"].unique():
        sub = pred_df[pred_df["model"] == model]
        plt.scatter(sub["true"], sub["pred"], label=model, s=45)

    plt.plot([vmin - pad, vmax + pad], [vmin - pad, vmax + pad], "k--", linewidth=1)

    plt.xlim(vmin - pad, vmax + pad)
    plt.ylim(vmin - pad, vmax + pad)

    plt.xlabel("True shear modulus (kPa)")
    plt.ylabel("Predicted shear modulus (kPa)")
    plt.title(title)
    plt.legend(fontsize=8)
    plt.tight_layout()
    plt.savefig(out_path, dpi=300)
    plt.close()


def fit_full_polynomial_model(
    X: np.ndarray,
    y: np.ndarray,
    degree: int,
    reg: str,
    alpha: float | None,
) -> dict:
    x_scaler = StandardScaler()
    y_scaler = StandardScaler()

    X_s = x_scaler.fit_transform(X)
    y_s = y_scaler.fit_transform(y.reshape(-1, 1)).ravel()

    poly = PolynomialFeatures(degree=degree, include_bias=False)
    X_poly = poly.fit_transform(X_s)

    if reg == "linear":
        model = LinearRegression()
    elif reg == "ridge":
        model = Ridge(alpha=float(alpha))
    else:
        raise ValueError(f"Unknown reg type: {reg}")

    model.fit(X_poly, y_s)

    return {
        "x_scaler": x_scaler,
        "y_scaler": y_scaler,
        "poly": poly,
        "model": model,
    }


def polynomial_coefficients_to_raw_basis(
    poly: PolynomialFeatures,
    coef_std: np.ndarray,
    intercept_std: float,
    x_scaler: StandardScaler,
    y_scaler: StandardScaler,
    feature_names: list[str],
) -> pd.DataFrame:
    symbols = sp.symbols(feature_names)

    standardized_symbols = [
        (sym - float(x_scaler.mean_[i])) / float(x_scaler.scale_[i])
        for i, sym in enumerate(symbols)
    ]

    expr_std = sp.Float(intercept_std)
    for c, powers in zip(coef_std, poly.powers_):
        term = sp.Float(c)
        for z, p in zip(standardized_symbols, powers):
            term *= z ** int(p)
        expr_std += term

    expr_raw = sp.expand(float(y_scaler.scale_[0]) * expr_std + float(y_scaler.mean_[0]))
    poly_expr = sp.Poly(expr_raw, *symbols)

    rows = []
    for powers, coeff in sorted(poly_expr.terms(), key=lambda t: (sum(t[0]),) + t[0]):
        label_parts = []
        for feat, power in zip(feature_names, powers):
            if power > 0:
                label_parts.append(f"{feat}^{power}" if power > 1 else feat)

        rows.append({
            "term": " * ".join(label_parts) if label_parts else "Intercept",
            **{f"power_{feat}": p for feat, p in zip(feature_names, powers)},
            "coefficient_raw_units": float(coeff),
        })

    return pd.DataFrame(rows)


def save_run_notes(out_dir: Path, lines: list[str]) -> None:
    (out_dir / "run_notes.txt").write_text("\n".join(lines), encoding="utf-8")