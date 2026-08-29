#include "render_graph.h"
#include <QPainter>
#include <cmath>
#include <algorithm>
#include <iostream>

namespace WallpaperEngine::Render {

void RenderGraph::setResolution(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;
    for (auto& [name, rt] : m_renderTargets) {
        rt.width = width;
        rt.height = height;
        rt.image = QImage(width, height, QImage::Format_RGBA8888);
        rt.image.fill(Qt::transparent);
    }
}

void RenderGraph::clear() {
    m_renderTargets.clear();
}

QImage& RenderGraph::getOrCreateRenderTarget(const std::string& name) {
    auto it = m_renderTargets.find(name);
    if (it != m_renderTargets.end()) {
        return it->second.image;
    }

    RenderTarget rt;
    rt.name = name;
    rt.width = m_width;
    rt.height = m_height;
    rt.image = QImage(m_width, m_height, QImage::Format_RGBA8888);
    rt.image.fill(Qt::transparent);

    auto inserted = m_renderTargets.emplace(name, std::move(rt));
    return inserted.first->second.image;
}

bool RenderGraph::hasRenderTarget(const std::string& name) const {
    return m_renderTargets.find(name) != m_renderTargets.end();
}

void RenderGraph::copyFramebufferToRenderTarget(const std::string& name, const QImage& sourceCanvas) {
    QImage& rtImage = getOrCreateRenderTarget(name);
    if (sourceCanvas.size() == rtImage.size()) {
        rtImage = sourceCanvas.copy();
    } else {
        rtImage = sourceCanvas.scaled(rtImage.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
}

QImage RenderGraph::applyBlurPass(const QImage& input, float radius, bool vertical) {
    if (input.isNull() || radius <= 0.5f) return input;

    QImage output(input.size(), QImage::Format_RGBA8888);
    output.fill(Qt::transparent);

    QPainter painter(&output);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    int r = std::clamp(static_cast<int>(radius), 1, 16);
    float step = r / 2.0f;

    if (vertical) {
        painter.setOpacity(0.33f);
        painter.drawImage(0, -step, input);
        painter.drawImage(0, 0, input);
        painter.drawImage(0, step, input);
    } else {
        painter.setOpacity(0.33f);
        painter.drawImage(-step, 0, input);
        painter.drawImage(0, 0, input);
        painter.drawImage(step, 0, input);
    }

    painter.end();
    return output;
}

QImage RenderGraph::applyWaterWavesPass(const QImage& input, const QImage& mask, float speed, float scale, float strength, float direction, float time) {
    if (input.isNull()) return input;

    QImage output = input.copy();
    float phase = time * speed * 3.0f;
    float waveAmp = strength * 15.0f;

    // Fast image warp pass for wave distortion
    QPainter painter(&output);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setOpacity(0.15f * std::clamp(strength, 0.0f, 1.0f));

    float dx = std::cos(direction) * std::sin(phase) * waveAmp;
    float dy = std::sin(direction) * std::cos(phase) * waveAmp;

    painter.drawImage(QRectF(dx, dy, input.width(), input.height()), input);
    painter.end();

    return output;
}

QImage RenderGraph::applyPulsePass(const QImage& input, const QImage& mask, float speed, float amount, float power, float time) {
    if (input.isNull()) return input;

    QImage output = input.copy();
    float pulseFactor = 1.0f + amount * 0.15f * std::sin(time * speed * 4.0f);

    QPainter painter(&output);
    painter.setOpacity(0.2f);
    painter.setCompositionMode(QPainter::CompositionMode_Screen);

    float newW = input.width() * pulseFactor;
    float newH = input.height() * pulseFactor;
    float offX = (input.width() - newW) / 2.0f;
    float offY = (input.height() - newH) / 2.0f;

    painter.drawImage(QRectF(offX, offY, newW, newH), input);
    painter.end();

    return output;
}

QImage RenderGraph::applyCompositionPass(const QImage& currentCanvas, const QImage& backgroundBuffer, const std::string& blendMode) {
    QImage composite = currentCanvas.copy();
    QPainter painter(&composite);

    if (blendMode == "multiply") {
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
    } else if (blendMode == "screen") {
        painter.setCompositionMode(QPainter::CompositionMode_Screen);
    } else if (blendMode == "additive" || blendMode == "add") {
        painter.setCompositionMode(QPainter::CompositionMode_Plus);
    } else {
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }

    painter.drawImage(0, 0, backgroundBuffer);
    painter.end();

    return composite;
}

} // namespace WallpaperEngine::Render
