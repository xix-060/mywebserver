"""绘制 DDPM 表格插补模型流程图"""
import os
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

FIG_DIR = os.path.join(os.path.dirname(__file__), '..', 'figures')
os.makedirs(FIG_DIR, exist_ok=True)

fig, ax = plt.subplots(figsize=(14, 8))
ax.set_xlim(0, 14)
ax.set_ylim(0, 8)
ax.axis('off')


def box(x, y, w, h, text, color='#E3F2FD', edge='#1565C0'):
    rect = mpatches.FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.1",
                                    facecolor=color, edgecolor=edge, linewidth=1.5)
    ax.add_patch(rect)
    ax.text(x + w/2, y + h/2, text, ha='center', va='center', fontsize=10, fontweight='bold')


def arrow(x1, y1, x2, y2):
    ax.annotate('', xy=(x2, y2), xytext=(x1, y1),
                arrowprops=dict(arrowstyle='->', color='#333', lw=1.5))


# 标题
ax.text(7, 7.6, 'DDPM 表格数据缺失值插补模型流程图', ha='center', fontsize=14, fontweight='bold')

# 第一行：数据输入
box(0.5, 6.2, 2.5, 1.0, '医疗表格数据\n(含缺失值 X_masked)', color='#FFF3E0', edge='#E65100')
box(4.0, 6.2, 2.5, 1.0, '缺失掩码 Mask\n(1=缺失, 0=观测)', color='#FFF3E0', edge='#E65100')
box(7.5, 6.2, 2.5, 1.0, '标准化\n(StandardScaler)', color='#FFF3E0', edge='#E65100')
box(11.0, 6.2, 2.5, 1.0, '完整数据 X_0\n(训练集)', color='#FFF3E0', edge='#E65100')

arrow(3.0, 6.7, 4.0, 6.7)
arrow(6.5, 6.7, 7.5, 6.7)

# 第二行：正向加噪
box(4.0, 4.6, 6.0, 1.0,
    '正向加噪过程 q(x_t|x_0)：x_t = √ᾱ_t·x_0 + √(1-ᾱ_t)·ε\n（马尔可夫链，逐步加噪至纯高斯噪声）',
    color='#E8F5E9', edge='#2E7D32')
arrow(12.25, 6.2, 12.25, 5.6)
arrow(12.25, 5.6, 10.0, 5.1)

# 第三行：训练
box(0.5, 3.0, 3.5, 1.0, 'MLP 去噪网络\nε_θ(x_t, t) → 预测噪声', color='#F3E5F5', edge='#6A1B9A')
box(5.0, 3.0, 3.5, 1.0, '损失函数\nMSE(ε_θ, ε)', color='#F3E5F5', edge='#6A1B9A')
box(9.5, 3.0, 3.5, 1.0, 'EMA 权重更新\n(decay=0.999)', color='#F3E5F5', edge='#6A1B9A')
arrow(4.0, 3.5, 5.0, 3.5)
arrow(8.5, 3.5, 9.5, 3.5)
arrow(7.0, 4.6, 2.25, 4.0)

# 第四行：反向去噪 + 条件插补
box(0.5, 1.2, 6.0, 1.3,
    '反向去噪 p_θ(x_{t-1}|x_t)：\n预测噪声 ε_θ → 计算均值 → 采样 x_{t-1}\n+ 条件约束：观测位替换为真实加噪值（固定噪声轨迹）',
    color='#E0F7FA', edge='#00838F')
box(7.5, 1.2, 6.0, 1.3,
    '多样本平均插补：\n独立采样 N=10 次，取均值\n→ 降低方差，逼近条件期望 E[x_missing | x_observed]',
    color='#E0F7FA', edge='#00838F')
arrow(2.25, 3.0, 2.25, 2.5)
arrow(6.5, 1.85, 7.5, 1.85)

# 输出
box(5.0, 0.0, 4.0, 0.9, '插补完成的完整数据\n→ 下游分类任务验证', color='#FFEBEE', edge='#C62828')
arrow(10.5, 1.2, 7.0, 0.9)

plt.tight_layout()
plt.savefig(os.path.join(FIG_DIR, 'fig0_model_flowchart.png'), dpi=150, bbox_inches='tight')
plt.close()
print("模型流程图已保存: fig0_model_flowchart.png")
