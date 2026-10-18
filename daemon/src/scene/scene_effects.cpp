#include "scene_effects.h"

#include <algorithm>
#include <cmath>

namespace WallpaperEngine::Scene
{

namespace
{

// Masked lerp: mask white => full effect. Null mask => apply everywhere.
// Integer math is kept within 255*255 — no overflow possible.
QImage blendMasked(const QImage& img, const QImage& affected, const QImage& mask)
{
    if (mask.isNull()) return affected;

    QImage m = mask.scaled(img.width(), img.height(),
                           Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                   .convertToFormat(QImage::Format_ARGB32);
    QImage out = img.convertToFormat(QImage::Format_ARGB32);
    const QImage a = affected.convertToFormat(QImage::Format_ARGB32);

    for (int y = 0; y < out.height(); ++y) {
        auto* o = reinterpret_cast<QRgb*>(out.scanLine(y));
        const auto* s = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* k = reinterpret_cast<const QRgb*>(m.constScanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const int t = qAlpha(k[x]) * (0.299 * qRed(k[x]) + 0.587 * qGreen(k[x]) + 0.114 * qBlue(k[x])) / 255;
            o[x] = qRgba(qRed(s[x]) * t / 255 + qRed(o[x]) * (255 - t) / 255,
                         qGreen(s[x]) * t / 255 + qGreen(o[x]) * (255 - t) / 255,
                         qBlue(s[x]) * t / 255 + qBlue(o[x]) * (255 - t) / 255,
                         qAlpha(s[x]) * t / 255 + qAlpha(o[x]) * (255 - t) / 255);
        }
    }
    return out;
}

QImage applyBlur(const QImage& src, const LayerEffect& eff)
{
    const int w = src.width(), h = src.height();
    if (w <= 1 || h <= 1) return src;

    // Radius is derived from (already-clamped) strength; bounded again here
    // so the per-pixel cost is O(n * radius) with radius <= 24.
    const int radius = std::clamp(static_cast<int>(std::clamp(eff.strength, 0.0f, 1.0f) * 12.0f + 0.5f), 1, 24);
    QImage img = src.convertToFormat(QImage::Format_ARGB32);

    auto boxBlur = [radius](const QImage& in, bool horizontal) {
        QImage o(in.size(), QImage::Format_ARGB32);
        const int n = radius * 2 + 1;
        for (int y = 0; y < in.height(); ++y) {
            const auto* sp = reinterpret_cast<const QRgb*>(in.constScanLine(y));
            auto* dp = reinterpret_cast<QRgb*>(o.scanLine(y));
            for (int x = 0; x < in.width(); ++x) {
                int r = 0, g = 0, b = 0, a = 0;
                for (int k = -radius; k <= radius; ++k) {
                    if (horizontal) {
                        const QRgb p = sp[std::clamp(x + k, 0, in.width() - 1)];
                        r += qRed(p); g += qGreen(p); b += qBlue(p); a += qAlpha(p);
                    } else {
                        const QRgb p = reinterpret_cast<const QRgb*>(in.constScanLine(std::clamp(y + k, 0, in.height() - 1)))[x];
                        r += qRed(p); g += qGreen(p); b += qBlue(p); a += qAlpha(p);
                    }
                }
                dp[x] = qRgba(r / n, g / n, b / n, a / n);
            }
        }
        return o;
    };

    for (int pass = 0; pass < 2; ++pass) {
        img = boxBlur(img, true);
        img = boxBlur(img, false);
    }
    return blendMasked(src, img, eff.maskImage);
}

QImage applyTint(const QImage& src, const LayerEffect& eff)
{
    QImage img = src.convertToFormat(QImage::Format_ARGB32);
    const float k = std::clamp(eff.strength, 0.0f, 1.0f);
    // Clamp the channel multipliers before any float->int cast; untrusted
    // colors previously allowed UB at extreme values.
    const float cr = std::clamp(eff.color.redF(), 0.0f, 4.0f);
    const float cg = std::clamp(eff.color.greenF(), 0.0f, 4.0f);
    const float cb = std::clamp(eff.color.blueF(), 0.0f, 4.0f);

    for (int y = 0; y < img.height(); ++y) {
        auto* p = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const int r = std::clamp(static_cast<int>(qRed(p[x]) * (1.0f - k + cr * k)), 0, 255);
            const int g = std::clamp(static_cast<int>(qGreen(p[x]) * (1.0f - k + cg * k)), 0, 255);
            const int b = std::clamp(static_cast<int>(qBlue(p[x]) * (1.0f - k + cb * k)), 0, 255);
            p[x] = qRgba(r, g, b, qAlpha(p[x]));
        }
    }
    return img;
}

} // namespace

bool hasStaticEffects(const std::vector<LayerEffect>& effects)
{
    for (const auto& eff : effects) {
        if (!eff.visible) continue;
        if (eff.type == EffectType::Blur || eff.type == EffectType::Tint) return true;
    }
    return false;
}

QImage applyStaticEffects(const QImage& src, const std::vector<LayerEffect>& effects)
{
    if (src.isNull() || effects.empty()) return src;

    QImage img = src;
    for (const auto& eff : effects) {
        if (!eff.visible) continue;
        switch (eff.type) {
            case EffectType::Blur: img = applyBlur(img, eff); break;
            case EffectType::Tint: img = applyTint(img, eff); break;
            default: break;
        }
    }
    return img;
}

} // namespace WallpaperEngine::Scene
