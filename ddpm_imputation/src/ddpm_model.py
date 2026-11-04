"""
基于 DDPM 的表格数据缺失值插补模型（标准 DDPM + EMA）
====================================================
- 正向加噪：马尔可夫链逐步加噪到纯高斯噪声
- MLP 去噪网络：预测噪声 ε
- EMA（指数滑动平均）：稳定生成质量
- 条件插补：观测值沿固定噪声轨迹约束，缺失值由反向过程生成
"""
import torch
import torch.nn as nn
import torch.nn.functional as F
import math
from copy import deepcopy


class SinusoidalPosEmb(nn.Module):
    def __init__(self, dim):
        super().__init__()
        self.dim = dim

    def forward(self, t):
        device = t.device
        half_dim = self.dim // 2
        emb = math.log(10000) / (half_dim - 1)
        emb = torch.exp(torch.arange(half_dim, device=device) * -emb)
        emb = t[:, None] * emb[None, :]
        return torch.cat([emb.sin(), emb.cos()], dim=-1)


class DenoiserMLP(nn.Module):
    """MLP 去噪网络：输入 (x_t, t) -> 预测噪声 ε"""
    def __init__(self, input_dim, hidden_dim=256, n_layers=4, time_dim=128):
        super().__init__()
        self.time_mlp = nn.Sequential(
            SinusoidalPosEmb(time_dim),
            nn.Linear(time_dim, time_dim),
            nn.GELU(),
            nn.Linear(time_dim, time_dim),
        )
        in_dim = input_dim + time_dim
        layers = [nn.Linear(in_dim, hidden_dim), nn.GELU()]
        for _ in range(n_layers - 1):
            layers += [nn.Linear(hidden_dim, hidden_dim), nn.GELU()]
        layers += [nn.Linear(hidden_dim, input_dim)]
        self.net = nn.Sequential(*layers)

    def forward(self, x, t):
        t_emb = self.time_mlp(t)
        return self.net(torch.cat([x, t_emb], dim=-1))


class EMA:
    """指数滑动平均：生成阶段用 EMA 权重，显著提升质量"""
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
    def __init__(self, input_dim, n_steps=300, beta_schedule='cosine',
                 hidden_dim=256, n_layers=4, device='cpu'):
        super().__init__()
        self.input_dim = input_dim
        self.n_steps = n_steps
        self.device = device

        if beta_schedule == 'linear':
            betas = torch.linspace(1e-4, 0.02, n_steps, device=device)
        elif beta_schedule == 'cosine':
            s = 0.008
            t_arr = torch.linspace(0, n_steps, n_steps + 1, device=device) / n_steps
            ac = torch.cos(((t_arr + s) / (1 + s)) * math.pi * 0.5) ** 2
            ac = ac / ac[0]
            betas = 1 - (ac[1:] / ac[:-1])
            betas = torch.clamp(betas, 0, 0.999)

        alphas = 1.0 - betas
        alphas_cumprod = torch.cumprod(alphas, dim=0)
        self.register_buffer('alphas_cumprod', alphas_cumprod)
        self.register_buffer('sqrt_alphas_cumprod', torch.sqrt(alphas_cumprod))
        self.register_buffer('sqrt_one_minus_alphas_cumprod', torch.sqrt(1 - alphas_cumprod))
        self.register_buffer('betas', betas)
        self.register_buffer('sqrt_recip_alphas', torch.sqrt(1.0 / alphas))
        self.register_buffer('posterior_variance',
                             betas * (1. - alphas_cumprod) / (1. - alphas_cumprod))

        self.denoiser = DenoiserMLP(input_dim, hidden_dim, n_layers).to(device)

    def q_sample(self, x_0, t, noise=None):
        if noise is None:
            noise = torch.randn_like(x_0)
        sa = self.sqrt_alphas_cumprod[t].view(-1, 1)
        soma = self.sqrt_one_minus_alphas_cumprod[t].view(-1, 1)
        return sa * x_0 + soma * noise, noise

    def train_step(self, x_0):
        bs = x_0.shape[0]
        t = torch.randint(0, self.n_steps, (bs,), device=self.device).long()
        x_t, noise = self.q_sample(x_0, t)
        pred_noise = self.denoiser(x_t, t.float())
        return F.mse_loss(pred_noise, noise)

    @torch.no_grad()
    def p_sample(self, x_t, t):
        t_tensor = torch.full((x_t.shape[0],), t, device=self.device, dtype=torch.long)
        pred_noise = self.denoiser(x_t, t_tensor.float())
        mean = self.sqrt_recip_alphas[t] * (x_t - self.betas[t] * pred_noise
                                            / self.sqrt_one_minus_alphas_cumprod[t])
        if t > 0:
            x_prev = mean + self.posterior_variance[t].sqrt() * torch.randn_like(x_t)
        else:
            x_prev = mean
        return x_prev

    @torch.no_grad()
    def impute(self, x_masked, mask, n_steps=None, n_repaint=2):
        """
        条件插补（RePaint 风格）：
        - mask: 1=缺失, 0=观测
        - 观测值沿固定噪声轨迹 z_obs 走，缺失值由反向去噪生成
        """
        if n_steps is None:
            n_steps = self.n_steps
        mask = mask.to(self.device).float()
        mask_obs = 1.0 - mask
        x_obs = torch.nan_to_num(x_masked.clone().to(self.device), nan=0.0)
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
                    sa = self.sqrt_alphas_cumprod[t]
                    soma = self.sqrt_one_minus_alphas_cumprod[t]
                    x_t = sa * x_prev + soma * z
                    x_t = obs_at(t) * mask_obs + x_t * mask
                else:
                    x_t = x_prev
            if t > 0:
                x_t = obs_at(t - 1) * mask_obs + x_t * mask
            else:
                x_t = x_obs * mask_obs + x_t * mask
        return x_t
