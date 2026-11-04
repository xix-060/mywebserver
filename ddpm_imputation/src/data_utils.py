"""
数据准备与缺失机制模拟
======================
- 加载 Pima Indians 糖尿病数据集
- 数据标准化
- 模拟 MCAR（完全随机缺失）机制
"""
import numpy as np
from sklearn.datasets import fetch_openml
from sklearn.preprocessing import StandardScaler


def load_pima_diabetes():
    """加载 Pima Indians 糖尿病数据集（768样本，8特征，二分类）"""
    pima = fetch_openml(name='diabetes', version=1, as_frame=False, parser='auto')
    X = pima.data.astype(np.float64)
    y = (pima.target == 'tested_positive').astype(np.int64)
    feature_names = ['preg(怀孕次数)', 'plas(血糖)', 'pres(血压)',
                     'skin(皮褶厚度)', 'insu(胰岛素)', 'mass(BMI)',
                     'pedi(遗传函数)', 'age(年龄)']
    return X, y, feature_names


def standardize(X_train, X_test=None):
    """标准化：用训练集的均值和标准差"""
    scaler = StandardScaler()
    X_train_s = scaler.fit_transform(X_train)
    if X_test is not None:
        X_test_s = scaler.transform(X_test)
        return X_train_s, X_test_s, scaler
    return X_train_s, scaler


def mask_mcar(X, missing_rate, seed=42):
    """
    模拟 MCAR（完全随机缺失）机制：
    对每个数值特征以 missing_rate 的概率随机置为 NaN。
    
    返回：
        X_masked: 含缺失值的数据
        mask: 缺失掩码（1=缺失，0=观测）
    """
    rng = np.random.default_rng(seed)
    mask = (rng.random(X.shape) < missing_rate).astype(np.float32)
    # 确保每行至少保留一个观测值（极端情况兜底）
    row_all_missing = mask.sum(axis=1) == X.shape[1]
    if row_all_missing.any():
        for i in np.where(row_all_missing)[0]:
            keep = rng.integers(0, X.shape[1])
            mask[i, keep] = 0.0
    X_masked = X.copy().astype(np.float64)
    X_masked[mask == 1] = np.nan
    return X_masked, mask.astype(np.int32)


if __name__ == '__main__':
    X, y, names = load_pima_diabetes()
    print("数据 shape:", X.shape, "正负样本比:", y.mean())
    X_s, scaler = standardize(X)
    X_m, mask = mask_mcar(X_s, missing_rate=0.2)
    print("缺失率:", mask.mean().round(4))
    print("X_masked 前3行:", X_m[:3])
