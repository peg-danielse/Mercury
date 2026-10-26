#include "gru.hpp"

#include <algorithm>
#include <cmath>

namespace {

float gelu(float x) { return 0.5f * x * (1.0f + std::erf(x / std::sqrt(2.0f))); }  // torch exact GELU
float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }
float softplus(float x) { return x > 20.0f ? x : std::log1p(std::exp(x)); }    // torch: beta 1, threshold 20

// W [rows][cols] -> [cols][rows]
std::vector<float> transpose(const std::vector<float>& W, int rows, int cols) {
  std::vector<float> t(W.size());
  for (int r = 0; r < rows; ++r)
    for (int c = 0; c < cols; ++c)
      t[static_cast<std::size_t>(c) * rows + r] = W[static_cast<std::size_t>(r) * cols + c];
  return t;
}

// y[r] = b[r] + sum_c Wt[c][r] x[c], summed in column order (the order of the row-by-row form, so the result is
// the same float for float), with the inner loop over independent outputs.
void affine(const std::vector<float>& Wt, const std::vector<float>& b, const float* x, int rows, int cols, float* y) {
  for (int r = 0; r < rows; ++r) y[r] = b[static_cast<std::size_t>(r)];
  for (int c = 0; c < cols; ++c) {
    const float xc = x[c];
    const float* w = Wt.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(rows);
    for (int r = 0; r < rows; ++r) y[r] += w[r] * xc;
  }
}

}  // namespace

GruModel::GruModel(std::shared_ptr<const GruWeights> weights) : w_(std::move(weights)) {
  const int d = w_->d;
  inp_t_ = transpose(w_->inp_w, d, w_->in);
  for (const auto& L : w_->layers) {
    ih_t_.push_back(transpose(L.w_ih, 3 * d, d));
    hh_t_.push_back(transpose(L.w_hh, 3 * d, d));
  }
  reset();
}

void GruModel::reset() { h_.assign(w_->layers.size(), std::vector<float>(static_cast<std::size_t>(w_->d), 0.f)); }

float GruModel::step(const std::array<float, kGruDaemonFeatures>& features) {
  const int d = w_->d;
  std::vector<float> x(static_cast<std::size_t>(d));
  affine(inp_t_, w_->inp_b, features.data(), d, w_->in, x.data());
  for (auto& v : x) v = gelu(v);
  std::vector<float> gi(static_cast<std::size_t>(3 * d)), gh(static_cast<std::size_t>(3 * d));
  for (std::size_t l = 0; l < w_->layers.size(); ++l) {
    const auto& L = w_->layers[l];
    auto& h = h_[l];
    affine(ih_t_[l], L.b_ih, x.data(), 3 * d, d, gi.data());
    affine(hh_t_[l], L.b_hh, h.data(), 3 * d, d, gh.data());
    // torch.nn.GRU: r = s(Wir x + bir + Whr h + bhr), z = s(...), n = tanh(Win x + bin + r (Whn h + bhn)),
    // h' = (1 - z) n + z h; gate rows in the order r, z, n.
    for (int j = 0; j < d; ++j) {
      const float r = sigmoid(gi[static_cast<std::size_t>(j)] + gh[static_cast<std::size_t>(j)]);
      const float z = sigmoid(gi[static_cast<std::size_t>(d + j)] + gh[static_cast<std::size_t>(d + j)]);
      const float n = std::tanh(gi[static_cast<std::size_t>(2 * d + j)] + r * gh[static_cast<std::size_t>(2 * d + j)]);
      h[static_cast<std::size_t>(j)] = (1.0f - z) * n + z * h[static_cast<std::size_t>(j)];
    }
    x = h;
  }
  float out = w_->out_b;
  for (int j = 0; j < d; ++j) out += w_->out_w[static_cast<std::size_t>(j)] * x[static_cast<std::size_t>(j)];
  return softplus(out);
}

int gru_request(float demand) {
  return std::clamp(static_cast<int>(std::ceil(static_cast<double>(demand) - 1e-3)), 1, 16);
}

std::array<float, kGruDaemonFeatures> GruFeatures::tick(const std::vector<GruThread>& threads, float other_run,
                                                        float other_wait, float occupancy, float pressure, int alloc) {
  std::array<float, kGruDaemonFeatures> f{};
  float tc = 0, run = 0, wait = 0, max_run = 0, max_wait = 0, cs = 0, mig = 0, busy = 0, alive = 0;
  std::array<float, kGruNodes> node{};
  for (const auto& t : threads) {
    tc += t.task_clock; run += t.run; wait += t.wait; cs += t.ctx_switch_k; mig += t.migrations_100;
    max_run = std::max(max_run, t.run);
    max_wait = std::max(max_wait, t.wait);
    if (t.run > 0.1f) busy += 1;
    if (std::abs(t.task_clock) + std::abs(t.run) + std::abs(t.wait) > 0) {
      alive += 1;
      if (t.numa >= 0 && t.numa < kGruNodes) node[static_cast<std::size_t>(t.numa)] += 1;
    }
  }
  const bool changed = started_ && alloc != prev_alloc_;
  const int da = started_ ? alloc - prev_alloc_ : 0;
  since_ = changed ? 0 : since_ + (started_ ? 1 : 0);
  recent_.push_back(changed ? 1 : 0);
  recent_sum_ += changed ? 1 : 0;
  if (recent_.size() > 100) {
    recent_sum_ -= recent_.front();
    recent_.pop_front();
  }
  started_ = true;
  prev_alloc_ = alloc;
  f[0] = tc; f[1] = run; f[2] = wait; f[3] = max_run; f[4] = max_wait; f[5] = cs; f[6] = mig;
  f[7] = busy / 16.f; f[8] = alive / 100.f; f[9] = occupancy; f[10] = pressure; f[11] = other_run; f[12] = other_wait;
  f[13] = static_cast<float>(alloc) / 16.f; f[14] = static_cast<float>(da) / 4.f;
  f[15] = static_cast<float>(std::log1p(static_cast<double>(since_)) / 10.0);
  f[16] = static_cast<float>(recent_sum_) / 10.f;
  for (int i = 0; i < kGruNodes; ++i) f[static_cast<std::size_t>(17 + i)] = node[static_cast<std::size_t>(i)] / std::max(alive, 1.f);
  return f;
}
