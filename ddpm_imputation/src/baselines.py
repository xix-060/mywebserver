"""
基线插补方法
===========
- 均值插补 (Mean Imputation)
- KNN 插补
- MICE（多重插补链式方程）
"""
import numpy as np
from sklearn.impute import SimpleImputer, KNNImputer
from sklearn.experimental import enable_iterative_imputer  # noqa
from sklearn.impute import IterativeImputer


def mean_impute(X_masked):
    imp = SimpleImputer(strategy='mean')
    return imp.fit_transform(X_masked)


def knn_impute(X_masked, n_neighbors=5):
    imp = KNNImputer(n_neighbors=n_neighbors)
    return imp.fit_transform(X_masked)


def mice_impute(X_masked, max_iter=20, random_state=42):
    imp = IterativeImputer(max_iter=max_iter, random_state=random_state, sample_posterior=False)
    return imp.fit_transform(X_masked)


BASELINES = {
    'Mean': mean_impute,
    'KNN': knn_impute,
    'MICE': mice_impute,
}
