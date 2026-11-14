// Stroke-based painter: coarse-to-fine rounds, thick->thin brush, shrinking strokes.
#pragma once
#include <vector>
#include <cmath>
#include <cstdint>
#include <random>
#include <algorithm>

struct RGB { float r, g, b; };

struct Stroke {
    std::vector<std::pair<float, float>> pts;  // polyline in image pixel coords
    float radius;                              // brush radius (px)
    RGB color;                                 // 0..255
    int round;                                 // which iteration produced it
};

struct Image {
    int w = 0, h = 0;
    std::vector<RGB> px;  // row-major
    RGB at(int x, int y) const { return px[y * w + x]; }
};

// Gaussian-ish box blur of an image, radius r (separable box blur x2).
static Image blur(const Image& src, int r) {
    if (r <= 0) return src;
    Image tmp = src, out = src;
    auto pass = [&](const Image& in, Image& o, bool horiz) {
        for (int y = 0; y < in.h; ++y)
            for (int x = 0; x < in.w; ++x) {
                RGB s{0, 0, 0}; int n = 0;
                for (int k = -r; k <= r; ++k) {
                    int xx = horiz ? x + k : x, yy = horiz ? y : y + k;
                    if (xx < 0 || yy < 0 || xx >= in.w || yy >= in.h) continue;
                    RGB c = in.at(xx, yy); s.r += c.r; s.g += c.g; s.b += c.b; ++n;
                }
                o.px[y * in.w + x] = {s.r / n, s.g / n, s.b / n};
            }
    };
    pass(src, tmp, true);
    pass(tmp, out, false);
    return out;
}

struct PainterParams {
    int rounds = 5;             // n iterations
    int canvasSize = 256;       // working resolution
    float firstRadius = 14.f;   // brush radius in round 0
    float lastRadius = 2.f;     // brush radius in final round
    float firstLength = 60.f;   // max stroke length in round 0
    float lastLength = 6.f;     // max stroke length in final round
    int candidatesPerRound = 4000;
    unsigned seed = 42;
};

// Detail 1..5 -> finer final brush and more candidate strokes per round.
// size is the working resolution (square); candidate count scales with its area.
static PainterParams makeParams(int w, int h, int detail, int rounds) {
    PainterParams P;
    detail = std::max(1, std::min(detail, 5));
    P.rounds = std::max(1, std::min(rounds, 20));
    P.canvasSize = std::max(w, h);
    P.lastRadius = 3.0f - 0.4f * (detail - 1);  // 3.0 .. 1.4 px
    double area = (double)w * h / (256.0 * 256.0);
    P.candidatesPerRound = (int)(3000 * area * (0.6 + 0.4 * detail));
    return P;
}

// Undo record: pixel index + previous colour.
using Undo = std::vector<std::pair<int, RGB>>;

// Render one stroke onto canvas (solid colour disc along the polyline).
// If undo is non-null, previous pixel values are recorded so the stroke can be reverted.
static void renderStroke(Image& cv, const Stroke& s, Undo* undo = nullptr) {
    for (size_t i = 0; i + 1 < s.pts.size() || (s.pts.size() == 1 && i == 0); ++i) {
        auto a = s.pts[i];
        auto b = s.pts.size() > 1 ? s.pts[i + 1] : a;
        int steps = std::max(1, (int)std::ceil(std::hypot(b.first - a.first, b.second - a.second)));
        for (int t = 0; t <= steps; ++t) {
            float u = (float)t / steps;
            float cx = a.first + (b.first - a.first) * u;
            float cy = a.second + (b.second - a.second) * u;
            int r = (int)std::ceil(s.radius);
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    if (dx * dx + dy * dy > s.radius * s.radius) continue;
                    int x = (int)std::round(cx) + dx, y = (int)std::round(cy) + dy;
                    if (x < 0 || y < 0 || x >= cv.w || y >= cv.h) continue;
                    if (undo) undo->push_back({y * cv.w + x, cv.px[y * cv.w + x]});
                    cv.px[y * cv.w + x] = s.color;
                }
        }
    }
}

// Squared colour error between canvas and target inside the stroke's footprint.
static float strokeError(const Image& cv, const Image& tgt, const Stroke& s) {
    float e = 0;
    int r = (int)std::ceil(s.radius);
    for (auto& p : s.pts) {
        int cx = (int)std::round(p.first), cy = (int)std::round(p.second);
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (dx * dx + dy * dy > s.radius * s.radius) continue;
                int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= cv.w || y >= cv.h) continue;
                RGB a = cv.at(x, y), b = tgt.at(x, y);
                float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
                e += dr * dr + dg * dg + db * db;
            }
    }
    return e;
}

// Generates strokes for all rounds. Canvas starts white.
// Each round: blur target more weakly (coarse->fine), thinner brush, shorter strokes.
static std::vector<Stroke> paint(const Image& target, const PainterParams& P) {
    std::mt19937 rng(P.seed);
    std::uniform_real_distribution<float> U(0.f, 1.f);
    Image canvas = target;
    for (auto& c : canvas.px) c = {255, 255, 255};
    std::vector<Stroke> out;

    for (int r = 0; r < P.rounds; ++r) {
        float t = P.rounds > 1 ? (float)r / (P.rounds - 1) : 1.f;
        float radius = P.firstRadius + (P.lastRadius - P.firstRadius) * t;
        float maxLen = P.firstLength + (P.lastLength - P.firstLength) * t;
        // Coarse target for early rounds (blurry copy), sharp target for final round.
        int blurR = (int)std::round((1.f - t) * 6.f);
        Image goal = blur(target, blurR);

        for (int c = 0; c < P.candidatesPerRound; ++c) {
            // Random start point; prefer locations with high error (where canvas differs most).
            float x0 = U(rng) * canvas.w, y0 = U(rng) * canvas.h;
            float ang = U(rng) * 6.2831853f;
            float len = maxLen * (0.3f + 0.7f * U(rng));
            int segs = std::max(1, (int)(len / std::max(1.f, radius)));
            Stroke s;
            s.radius = radius;
            s.round = r;
            s.pts.push_back({x0, y0});
            float x = x0, y = y0, a = ang;
            for (int i = 0; i < segs; ++i) {
                a += (U(rng) - 0.5f) * 0.6f;  // gentle curvature
                x += std::cos(a) * len / segs;
                y += std::sin(a) * len / segs;
                s.pts.push_back({x, y});
            }
            // Colour = mean of goal under the footprint.
            float sr = 0, sg = 0, sb = 0; int n = 0;
            int rr = (int)std::ceil(radius);
            for (auto& p : s.pts) {
                int cx = (int)std::round(p.first), cy = (int)std::round(p.second);
                for (int dy = -rr; dy <= rr; ++dy)
                    for (int dx = -rr; dx <= rr; ++dx) {
                        if (dx * dx + dy * dy > radius * radius) continue;
                        int xx = cx + dx, yy = cy + dy;
                        if (xx < 0 || yy < 0 || xx >= goal.w || yy >= goal.h) continue;
                        RGB g = goal.at(xx, yy);
                        sr += g.r; sg += g.g; sb += g.b; ++n;
                    }
            }
            if (n == 0) continue;
            s.color = {sr / n, sg / n, sb / n};

            // Accept only if it reduces error against the goal.
            float e0 = strokeError(canvas, goal, s);
            Undo undo;
            renderStroke(canvas, s, &undo);
            float e1 = strokeError(canvas, goal, s);
            if (e1 < e0) {
                out.push_back(s);
            } else {
                // Revert in reverse order so overlapping writes restore correctly.
                for (auto it = undo.rbegin(); it != undo.rend(); ++it)
                    canvas.px[it->first] = it->second;
            }
        }
    }
    return out;
}
