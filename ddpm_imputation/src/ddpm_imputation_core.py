"""
==========================================================================
  基于 DDPM 的医疗表格数据缺失值插补 —— 核心代码（整合版）
  包含：数据加载 / MCAR掩码 / DDPM模型(MLP+EMA+RePaint) / 训练 / 基线 / 评估
==========================================================================
运行方式：
  pip install -r requirements.txt
  python ddpm_imputation_core.py
"""
import os
import math
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
import torch.optim as optim
from copy import deepcopy
from sklearn.datasets import fetch_openml
from sklearn.preprocessing import StandardScaler
from sklearn.model_selection import train_test_split
from sklearn.impute import SimpleImputer, KNNImputer
from sklearn.experimental import enable_iterative_imputer  # noqa
from sklearn.impute import IterativeImputer
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import accuracy_score, mean_squared_error

DEVICE = 'cpu'
SEED = 42
np.random.seed(SEED)
torch.manual_seed(SEED)


# ======================== 1. 数据准备与 MCAR 缺失 ========================
def load_pima_diabetes():
    pima = fetch_openml(name='diabetes', version=1, as_frame=False, parser='auto')
    X = pima.data.astype(np.float64)
    y = (pima.target == 'tested_positive').astype(np.int64)
    feature_names = ['preg', 'plas', 'pres', 'skin', 'insu', 'mass', 'pedi', 'age']
    return X, y, feature_names


def standardize(X_train, X_test):
    scaler = StandardScaler()
    return scaler.fit_transform(X_train), scaler.transform(X_test), scaler


def mask_mcar(X, missing_rate, seed=42):
    """MCAR 机制：以 missing_rate 概率随机置 NaN；返回 (X_masked, mask)"""
    rng = np.random.default_rng(seed)
    mask = (rng.random(X.shape) < missing_rate).astype(np.float32)
    for i in np.where(mask.sum(axis=1) == X.shape[1])[0]:
        mask[i, rng.integers(0, X.shape[1])] = 0.0
    X_masked = X.copy().astype(np.float64)
    X_masked[mask == 1] = np.nan
    return X_masked, mask.astype(np.int32)


# ======================== 2. DDPM 模型（MLP 去噪网络 + EMA）========================
class SinusoidalPosEmb(nn.Module):
    def __init__(self, dim):
        super().__init__()
        self.dim = dim

    def forward(self, t):
        half_dim = self.dim // 2
        emb = math.log(10000) / (half_dim - 1)
        emb = torch.exp(torch.arange(half_dim, device=t.device) * -emb)
        emb = t[:, None] * emb[None, :]
        return torch.cat([emb.sin(), emb.cos()], dim=-1)


class DenoiserMLP(nn.Module):
    """MLP 去噪网络：输入 (x_t, t)，输出预测噪声 ε"""
    def __init__(self, input_dim, hidden_dim=256, n_layers=4, time_dim=128):
        super().__init__()
        self.time_mlp = nn.Sequential(
            SinusoidalPosEmb(time_dim),
            nn.Linear(time_dim, time_dim), nn.GELU(),
            nn.Linear(time_dim, time_dim),
        )
        in_dim = input_dim + time_dim
        layers = [nn.Linear(in_dim, hidden_dim), nn.GELU()]
        for _ in range(n_layers - 1):
            layers += [nn.Linear(hidden_dim, hidden_dim), nn.GELU()]
        layers += [nn.Linear(hidden_dim, input_dim)]
        self.net = nn.Sequential(*layers)

    def forward(self, x, t):
        return self.net(torch.cat([x, self.time_mlp(t)], dim=-1))


class EMA:
    """指数滑动平均：推理时使用 EMA 权重，显著提升生成质量"""
    def __init__(self, model, decay=0.999):
        self.decay = decay
        self.shadow = deepcopy(model).state_dict()
        for p in self.shadow.values():
            p.requires_grad_(False)

    @torch.no_grad()
    def update(self, model):
        for name, param in model.state_dict().items():
            if param.dtype.is_floating_point:
                self.shadow[name].mul_(self.decay).add_(param, alpha=1 - self.decay)

    def apply(self, model):
        model.load_state_dict(self.shadow)


class DDPM(nn.Module):
    def __init__(self, input_dim, n_steps=300, hidden_dim=256, n_layers=4, device='cpu'):
        super().__init__()
        self.n_steps = n_steps
        self.device = device
        # 余弦噪声调度
        s = 0.008
        t_arr = torch.linspace(0, n_steps, n_steps + 1, device=device) / n_steps
        ac = torch.cos(((t_arr + s) / (1 + s)) * math.pi * 0.5) ** 2
        ac = ac / ac[0]
        betas = torch.clamp(1 - (ac[1:] / ac[:-1]), 0, 0.999)
        alphas = 1.0 - betas
        alphas_cumprod = torch.cumprod(alphas, dim=0)
        self.register_buffer('sqrt_alphas_cumprod', torch.sqrt(alphas_cumprod))
        self.register_buffer('sqrt_one_minus_alphas_cumprod', torch.sqrt(1 - alphas_cumprod))
        self.register_buffer('betas', betas)
        self.register_buffer('sqrt_recip_alphas', torch.sqrt(1.0 / alphas))
        self.register_buffer('posterior_variance',
                             betas * (1. - alphas_cumprod) / (1. - alphas_cumprod))
        self.denoiser = DenoiserMLP(input_dim, hidden_dim, n_layers).to(device)

    def q_sample(self, x_0, t, noise=None):
        """正向加噪：x_t = sqrt(abar)*x_0 + sqrt(1-abar)*eps"""
        noise = noise if noise is not None else torch.randn_like(x_0)
        sa = self.sqrt_alphas_cumprod[t].view(-1, 1)
        soma = self.sqrt_one_minus_alphas_cumprod[t].view(-1, 1)
        return sa * x_0 + soma * noise, noise

    def train_step(self, x_0):
        """训练：随机 t，加噪，预测噪声，MSE loss"""
        t = torch.randint(0, self.n_steps, (x_0.shape[0],), device=self.device).long()
        x_t, noise = self.q_sample(x_0, t)
        pred_noise = self.denoiser(x_t, t.float())
        return F.mse_loss(pred_noise, noise)

    @torch.no_grad()
    def p_sample(self, x_t, t):
        """反向去噪一步"""
        t_tensor = torch.full((x_t.shape[0],), t, device=self.device, dtype=torch.long)
        pred_noise = self.denoiser(x_t, t_tensor.float())
        mean = self.sqrt_recip_alphas[t] * (x_t - self.betas[t] * pred_noise
                                            / self.sqrt_one_minus_alphas_cumprod[t])
        if t > 0:
            return mean + self.posterior_variance[t].sqrt() * torch.randn_like(x_t)
        return mean

    @torch.no_grad()
    def impute(self, x_masked, mask, n_repaint=2):
        """
        RePaint 风格条件插补：
        - mask: 1=缺失, 0=观测
        - 观测值沿固定噪声轨迹 z_obs 约束，缺失值由反向过程生成
        """
        n_steps = self.n_steps
        mask_obs = 1.0 - mask
        x_obs = torch.nan_to_num(x_masked, nan=0.0)
        z_obs = torch.randn_like(x_obs)

        def obs_at(t_idx):
            sa = self.sqrt_alphas_cumprod[t_idx]
            soma = self.sqrt_one_minus_alphas_cumprod[t_idx]
            return sa * x_obs + soma * z_obs

        x_t = obs_at(n_steps - 1) * mask_obs + torch.randn_like(x_obs) * mask
        for t in reversed(range(n_steps)):
            for _ in range(n_repaint):
                x_prev = self.p_sample(x_t, t)
                if t > 0:
                    z = torch.randn_like(x_prev)
                    x_t = (self.sqrt_alphas_cumprod[t] * x_prev
                           + self.sqrt_one_minus_alphas_cumprod[t] * z)
                    x_t = obs_at(t) * mask_obs + x_t * mask
                else:
                    x_t = x_prev
            x_t = (obs_at(t - 1) if t > 0 else x_obs) * mask_obs + x_t * mask
        return x_t


# ======================== 3. 训练 ========================
def train_ddpm(X_train, n_steps=300, epochs=20000, lr=2e-3, batch_size=128):
    model = DDPM(X_train.shape[1], n_steps=n_steps, device=DEVICE)
    ema = EMA(model, decay=0.999)
    optimizer = optim.Adam(model.parameters(), lr=lr)
    scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=epochs)
    X_t = torch.tensor(X_train, dtype=torch.float32, device=DEVICE)
    n = X_t.shape[0]
    for epoch in range(epochs):
        optimizer.zero_grad()
        idx = torch.randperm(n)[:batch_size]
        loss = model.train_step(X_t[idx])
        loss.backward()
        optimizer.step()
        ema.update(model)
        scheduler.step()
        if (epoch + 1) % 2000 == 0:
            print(f"  Epoch {epoch+1}/{epochs}, loss={loss.item():.5f}")
    ema.apply(model)
    return model


@torch.no_grad()
def ddpm_impute_multi(model, X_masked, mask, n_samples=10):
    """多样本平均插补：降低方差，逼近条件期望"""
    x_m = torch.tensor(X_masked, dtype=torch.float32, device=DEVICE)
    m = torch.tensor(mask, dtype=torch.float32, device=DEVICE)
    acc = np.zeros_like(X_masked)
    for _ in range(n_samples):
        acc += model.impute(x_m, m).cpu().numpy()
    return acc / n_samples


# ======================== 4. 基线方法 ========================
def mean_impute(X):
    return SimpleImputer(strategy='mean').fit_transform(X)


def knn_impute(X):
    return KNNImputer(n_neighbors=5).fit_transform(X)


def mice_impute(X):
    return IterativeImputer(max_iter=20, random_state=42).fit_transform(X)


# ======================== 5. 评估指标 ========================
def rmse_imputed(X_true, X_imp, mask):
    idx = mask == 1
    return np.sqrt(mean_squared_error(X_true[idx], X_imp[idx])) if idx.sum() > 0 else 0.0


def downstream_acc(X_train_imp, y_train, X_test_imp, y_test):
    clf = LogisticRegression(max_iter=2000, random_state=SEED)
    clf.fit(X_train_imp, y_train)
    return accuracy_score(y_test, clf.predict(X_test_imp))


# ======================== 6. 主实验 ========================
def main():
    print("加载 Pima Indians 糖尿病数据集...")
    X, y, names = load_pima_diabetes()
    print(f"样本数: {X.shape[0]}, 特征数: {X.shape[1]}, 阳性率: {y.mean():.3f}")

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.2, random_state=SEED, stratify=y)
    X_train_s, X_test_s, _ = standardize(X_train, X_test)

    for rate in [0.1, 0.2, 0.3]:
        print(f"\n{'='*50}\n缺失率 = {int(rate*100)}%\n{'='*50}")
        X_tr_m, mask_tr = mask_mcar(X_train_s, rate, seed=SEED)
        X_te_m, mask_te = mask_mcar(X_test_s, rate, seed=SEED + 1)

        # DDPM
        print("[DDPM] 训练中...")
        model = train_ddpm(X_train_s, epochs=20000, n_steps=300)
        X_te_ddpm = ddpm_impute_multi(model, X_te_m, mask_te, n_samples=10)
        X_tr_ddpm = ddpm_impute_multi(model, X_tr_m, mask_tr, n_samples=10)
        print(f"[DDPM] RMSE={rmse_imputed(X_test_s, X_te_ddpm, mask_te):.5f}, "
              f"Acc={downstream_acc(X_tr_ddpm, y_train, X_te_ddpm, y_test):.4f}")

        # 基线
        for name, fn in [('Mean', mean_impute), ('KNN', knn_impute), ('MICE', mice_impute)]:
            X_all = np.vstack([X_tr_m, X_te_m])
            X_all_imp = fn(X_all)
            te_imp = X_all_imp[X_tr_m.shape[0]:]
            tr_imp = X_all_imp[:X_tr_m.shape[0]]
            print(f"[{name}] RMSE={rmse_imputed(X_test_s, te_imp, mask_te):.5f}, "
                  f"Acc={downstream_acc(tr_imp, y_train, te_imp, y_test):.4f}")


if __name__ == '__main__':
    main()
