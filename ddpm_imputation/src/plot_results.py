"""
生成论文所需的所有图表
======================
- 图1：血糖特征 真实分布 vs DDPM插补后分布（直方图+核密度）
- 图2：DDPM 训练 Loss 曲线
- 图3：不同缺失率下各方法 RMSE 对比柱状图
"""
import os
import json
import numpy as np
import matplotlib.pyplot as plt
import matplotlib

# 中文字体设置
matplotlib.rcParams['font.sans-serif'] = ['DejaVu Sans', 'SimHei', 'Arial Unicode MS']
matplotlib.rcParams['axes.unicode_minus'] = False

RESULTS_DIR = os.path.join(os.path.dirname(__file__), '..', 'results')
FIG_DIR = os.path.join(os.path.dirname(__file__), '..', 'figures')
os.makedirs(FIG_DIR, exist_ok=True)


def plot_glucose_distribution():
    """图1：血糖特征分布对比"""
    with open(os.path.join(RESULTS_DIR, 'results.json')) as f:
        res = json.load(f)
    dist = res['ddpm_glucose_dist']
    true_vals = np.array(dist['true'])
    ddpm_vals = np.array(dist['ddpm'])

    fig, ax = plt.subplots(figsize=(8, 5))
    bins = np.linspace(min(true_vals.min(), ddpm_vals.min()),
                       max(true_vals.max(), ddpm_vals.max()), 30)
    ax.hist(true_vals, bins=bins, alpha=0.5, density=True, label='True (Ground Truth)', color='#2196F3')
    ax.hist(ddpm_vals, bins=bins, alpha=0.5, density=True, label='DDPM Imputed', color='#F44336')
    # 核密度估计
    from scipy.stats import gaussian_kde
    kde_true = gaussian_kde(true_vals)
    kde_ddpm = gaussian_kde(ddpm_vals)
    xs = np.linspace(bins[0], bins[-1], 200)
    ax.plot(xs, kde_true(xs), color='#2196F3', linewidth=2)
    ax.plot(xs, kde_ddpm(xs), color='#F44336', linewidth=2)
    ax.set_xlabel('Glucose (plas) - Standardized', fontsize=12)
    ax.set_ylabel('Density', fontsize=12)
    ax.set_title('Figure 1: Distribution of Glucose Feature — True vs DDPM Imputed (20% missing)', fontsize=13)
    ax.legend(fontsize=11)
    ax.grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(os.path.join(FIG_DIR, 'fig1_glucose_distribution.png'), dpi=150)
    plt.close()
    print("图1 已保存: fig1_glucose_distribution.png")


def plot_loss_curve():
    """图2：训练 Loss 曲线（用 20% 缺失率对应的模型训练 loss）"""
    loss = np.load(os.path.join(RESULTS_DIR, 'loss_history_rate20.npy'))
    # 平滑
    window = 20
    if len(loss) >= window:
        loss_smooth = np.convolve(loss, np.ones(window)/window, mode='valid')
    else:
        loss_smooth = loss
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(loss, alpha=0.3, color='#9E9E9E', label='Raw loss')
    ax.plot(np.arange(len(loss_smooth)) + window//2, loss_smooth,
            color='#E91E63', linewidth=2, label=f'Smoothed (window={window})')
    ax.set_xlabel('Epoch', fontsize=12)
    ax.set_ylabel('MSE Loss (noise prediction)', fontsize=12)
    ax.set_title('Figure 2: DDPM Training Loss Curve', fontsize=13)
    ax.legend(fontsize=11)
    ax.grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(os.path.join(FIG_DIR, 'fig2_loss_curve.png'), dpi=150)
    plt.close()
    print("图2 已保存: fig2_loss_curve.png")


def plot_rmse_comparison():
    """图3：不同缺失率下各方法 RMSE 对比柱状图"""
    with open(os.path.join(RESULTS_DIR, 'results.json')) as f:
        res = json.load(f)
    rates = res['missing_rates']
    methods = list(res['rmse'][rates[0]].keys())
    x = np.arange(len(rates))
    width = 0.18
    colors = ['#F44336', '#2196F3', '#4CAF50', '#FF9800']

    fig, ax = plt.subplots(figsize=(9, 5.5))
    for i, method in enumerate(methods):
        vals = [res['rmse'][r][method] for r in rates]
        bars = ax.bar(x + i*width, vals, width, label=method, color=colors[i % len(colors)])
        for bar, v in zip(bars, vals):
            ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.005,
                    f'{v:.3f}', ha='center', va='bottom', fontsize=8)
    ax.set_xlabel('Missing Rate', fontsize=12)
    ax.set_ylabel('RMSE (lower is better)', fontsize=12)
    ax.set_title('Figure 3: RMSE Comparison Across Missing Rates', fontsize=13)
    ax.set_xticks(x + width * (len(methods)-1) / 2)
    ax.set_xticklabels(rates)
    ax.legend(fontsize=11)
    ax.grid(axis='y', alpha=0.3)
    plt.tight_layout()
    plt.savefig(os.path.join(FIG_DIR, 'fig3_rmse_comparison.png'), dpi=150)
    plt.close()
    print("图3 已保存: fig3_rmse_comparison.png")


def plot_accuracy_comparison():
    """附加：下游分类准确率对比"""
    with open(os.path.join(RESULTS_DIR, 'results.json')) as f:
        res = json.load(f)
    rates = res['missing_rates']
    methods = list(res['accuracy'][rates[0]].keys())
    x = np.arange(len(rates))
    width = 0.18
    colors = ['#F44336', '#2196F3', '#4CAF50', '#FF9800']

    fig, ax = plt.subplots(figsize=(9, 5.5))
    for i, method in enumerate(methods):
        vals = [res['accuracy'][r][method] for r in rates]
        bars = ax.bar(x + i*width, vals, width, label=method, color=colors[i % len(colors)])
        for bar, v in zip(bars, vals):
            ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.003,
                    f'{v:.3f}', ha='center', va='bottom', fontsize=8)
    ax.set_xlabel('Missing Rate', fontsize=12)
    ax.set_ylabel('Classification Accuracy (higher is better)', fontsize=12)
    ax.set_title('Figure 4: Downstream Classification Accuracy After Imputation', fontsize=13)
    ax.set_xticks(x + width * (len(methods)-1) / 2)
    ax.set_xticklabels(rates)
    ax.legend(fontsize=11)
    ax.grid(axis='y', alpha=0.3)
    ax.set_ylim(0.5, 0.85)
    plt.tight_layout()
    plt.savefig(os.path.join(FIG_DIR, 'fig4_accuracy_comparison.png'), dpi=150)
    plt.close()
    print("图4 已保存: fig4_accuracy_comparison.png")


if __name__ == '__main__':
    plot_glucose_distribution()
    plot_loss_curve()
    plot_rmse_comparison()
    plot_accuracy_comparison()
    print("\n所有图表生成完毕，保存在 figures/ 目录")
