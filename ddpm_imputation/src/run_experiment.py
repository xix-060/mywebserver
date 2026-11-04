"""
主实验脚本：训练 DDPM + 基线对比 + 下游分类任务
==============================================
配置：cosine 调度 / 300步 / EMA / 20000 epoch / 多样本平均插补
输出保存在 results/ 目录
"""
import os
import sys
import json
import numpy as np
import torch
import torch.optim as optim
from sklearn.model_selection import train_test_split
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import accuracy_score, mean_squared_error

sys.path.insert(0, os.path.dirname(__file__))
from data_utils import load_pima_diabetes, standardize, mask_mcar
from ddpm_model import DDPM, EMA
from baselines import BASELINES

DEVICE = 'cpu'
SEED = 42
np.random.seed(SEED)
torch.manual_seed(SEED)

RESULTS_DIR = os.path.join(os.path.dirname(__file__), '..', 'results')
os.makedirs(RESULTS_DIR, exist_ok=True)


def train_ddpm(X_train, n_steps=300, epochs=20000, lr=2e-3, batch_size=128,
               hidden_dim=256, n_layers=4, ema_decay=0.999):
    """训练 DDPM（含 EMA），返回模型、EMA 和 loss 历史"""
    input_dim = X_train.shape[1]
    model = DDPM(input_dim, n_steps=n_steps, beta_schedule='cosine',
                 hidden_dim=hidden_dim, n_layers=n_layers, device=DEVICE)
    ema = EMA(model, decay=ema_decay)
    optimizer = optim.Adam(model.parameters(), lr=lr)
    scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=epochs)
    X_t = torch.tensor(X_train, dtype=torch.float32, device=DEVICE)
    n = X_t.shape[0]
    loss_history = []
    for epoch in range(epochs):
        model.train()
        optimizer.zero_grad()
        idx = torch.randperm(n)[:batch_size]
        loss = model.train_step(X_t[idx])
        loss.backward()
        optimizer.step()
        ema.update(model)
        scheduler.step()
        loss_history.append(loss.item())
        if (epoch + 1) % 2000 == 0:
            print(f"  Epoch {epoch+1}/{epochs}, loss={loss.item():.5f}")
    # 评估时使用 EMA 权重
    ema.apply(model)
    return model, loss_history


@torch.no_grad()
def ddpm_impute(model, X_masked, mask, n_samples=10, n_steps=300, n_repaint=2):
    """DDPM 多样本平均插补"""
    x_m = torch.tensor(X_masked, dtype=torch.float32, device=DEVICE)
    m = torch.tensor(mask, dtype=torch.float32, device=DEVICE)
    model.eval()
    acc = np.zeros_like(X_masked, dtype=np.float64)
    for _ in range(n_samples):
        imp = model.impute(x_m, m, n_steps=n_steps, n_repaint=n_repaint).cpu().numpy()
        acc += imp
    return acc / n_samples


def rmse_imputed(X_true, X_imputed, mask):
    idx = mask == 1
    return np.sqrt(mean_squared_error(X_true[idx], X_imputed[idx])) if idx.sum() > 0 else 0.0


def downstream_clf(X_train_imp, y_train, X_test_imp, y_test):
    clf = LogisticRegression(max_iter=2000, random_state=SEED)
    clf.fit(X_train_imp, y_train)
    return accuracy_score(y_test, clf.predict(X_test_imp))


def run_experiment(missing_rates=(0.1, 0.2, 0.3), epochs=20000, n_steps=300,
                   n_samples=10, n_repaint=2):
    print("=" * 60)
    print("加载 Pima Indians 糖尿病数据集")
    print("=" * 60)
    X, y, feature_names = load_pima_diabetes()
    print(f"样本数: {X.shape[0]}, 特征数: {X.shape[1]}, 阳性率: {y.mean():.3f}")
    print("特征:", feature_names)

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.2, random_state=SEED, stratify=y)
    print(f"训练集: {X_train.shape}, 测试集: {X_test.shape}")

    X_train_s, X_test_s, scaler = standardize(X_train, X_test)

    all_rmse, all_acc = {}, {}
    ddpm_dist_data = {}
    loss_history_20 = None

    for rate in missing_rates:
        print(f"\n{'='*60}\n缺失率 = {rate*100:.0f}%\n{'='*60}")

        X_train_m, mask_train = mask_mcar(X_train_s, missing_rate=rate, seed=SEED)
        X_test_m, mask_test = mask_mcar(X_test_s, missing_rate=rate, seed=SEED + 1)

        rate_rmse, rate_acc = {}, {}

        # ---- DDPM ----
        print(f"[DDPM] 训练 (epochs={epochs}, steps={n_steps})...")
        model, loss_hist = train_ddpm(X_train_s, n_steps=n_steps, epochs=epochs)
        torch.save(model.state_dict(),
                   os.path.join(RESULTS_DIR, f'ddpm_model_rate{int(rate*100)}.pt'))
        np.save(os.path.join(RESULTS_DIR, f'loss_history_rate{int(rate*100)}.npy'),
                np.array(loss_hist))
        if abs(rate - 0.2) < 1e-6:
            loss_history_20 = loss_hist

        X_test_ddpm = ddpm_impute(model, X_test_m, mask_test,
                                  n_samples=n_samples, n_steps=n_steps, n_repaint=n_repaint)
        ddpm_rmse = rmse_imputed(X_test_s, X_test_ddpm, mask_test)
        rate_rmse['DDPM'] = ddpm_rmse
        print(f"[DDPM] 测试集 RMSE = {ddpm_rmse:.5f}")

        X_train_ddpm = ddpm_impute(model, X_train_m, mask_train,
                                   n_samples=n_samples, n_steps=n_steps, n_repaint=n_repaint)
        ddpm_acc = downstream_clf(X_train_ddpm, y_train, X_test_ddpm, y_test)
        rate_acc['DDPM'] = ddpm_acc
        print(f"[DDPM] 下游分类准确率 = {ddpm_acc:.4f}")

        if abs(rate - 0.2) < 1e-6:
            plas_idx = 1
            ddpm_dist_data = {
                'true': X_test_s[:, plas_idx][mask_test[:, plas_idx] == 1].tolist(),
                'ddpm': X_test_ddpm[:, plas_idx][mask_test[:, plas_idx] == 1].tolist(),
            }

        # ---- 基线（合并 train+test 一起插补，无监督方法更公平）----
        for name, imp_fn in BASELINES.items():
            X_all_m = np.vstack([X_train_m, X_test_m])
            X_all_imp = imp_fn(X_all_m)
            X_test_imp = X_all_imp[X_train_m.shape[0]:]
            X_train_imp = X_all_imp[:X_train_m.shape[0]]
            rmse = rmse_imputed(X_test_s, X_test_imp, mask_test)
            acc = downstream_clf(X_train_imp, y_train, X_test_imp, y_test)
            rate_rmse[name] = rmse
            rate_acc[name] = acc
            print(f"[{name}] RMSE = {rmse:.5f}, 准确率 = {acc:.4f}")

        all_rmse[f'{int(rate*100)}%'] = rate_rmse
        all_acc[f'{int(rate*100)}%'] = rate_acc

    results = {
        'feature_names': feature_names,
        'n_samples': int(X.shape[0]),
        'n_features': int(X.shape[1]),
        'train_size': int(X_train.shape[0]),
        'test_size': int(X_test.shape[0]),
        'missing_rates': [f'{int(r*100)}%' for r in missing_rates],
        'rmse': all_rmse,
        'accuracy': all_acc,
        'ddpm_glucose_dist': ddpm_dist_data,
    }
    with open(os.path.join(RESULTS_DIR, 'results.json'), 'w') as f:
        json.dump(results, f, indent=2, ensure_ascii=False)

    print("\n" + "=" * 60)
    print("实验完成，结果已保存到 results/results.json")
    print("=" * 60)
    return results


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--epochs', type=int, default=20000)
    parser.add_argument('--steps', type=int, default=300)
    parser.add_argument('--samples', type=int, default=10)
    args = parser.parse_args()
    run_experiment(epochs=args.epochs, n_steps=args.steps, n_samples=args.samples)
