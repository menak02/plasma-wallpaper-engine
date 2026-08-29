#include "mesh_renderer.h"
#include <QPainter>
#include <QPainterPath>
#include <cmath>
#include <algorithm>

namespace WallpaperEngine::Render {

void MeshDeformer::generateDefaultGrid(uint32_t cols, uint32_t rows, float width, float height,
                                       std::vector<MeshVertex>& outVertices, std::vector<MeshTriangle>& outIndices) {
    outVertices.clear();
    outIndices.clear();

    if (cols < 1 || rows < 1) return;

    for (uint32_t r = 0; r <= rows; ++r) {
        float v = static_cast<float>(r) / static_cast<float>(rows);
        float y = (v - 0.5f) * height;

        for (uint32_t c = 0; c <= cols; ++c) {
            float u = static_cast<float>(c) / static_cast<float>(cols);
            float x = (u - 0.5f) * width;

            MeshVertex vert;
            vert.position = QVector3D(x, y, 0.0f);
            vert.uv = QVector2D(u, v);
            vert.boneWeights = QVector4D(1.0f, 0.0f, 0.0f, 0.0f);
            vert.boneIndices = QVector4D(0.0f, 0.0f, 0.0f, 0.0f);
            outVertices.push_back(vert);
        }
    }

    uint32_t stride = cols + 1;
    for (uint32_t r = 0; r < rows; ++r) {
        for (uint32_t c = 0; c < cols; ++c) {
            uint32_t topLeft = r * stride + c;
            uint32_t topRight = topLeft + 1;
            uint32_t bottomLeft = (r + 1) * stride + c;
            uint32_t bottomRight = bottomLeft + 1;

            outIndices.push_back({topLeft, bottomLeft, topRight});
            outIndices.push_back({topRight, bottomLeft, bottomRight});
        }
    }
}

void MeshDeformer::deformVertices(std::vector<MeshVertex>& vertices, float time, float speed, float strength, float direction) {
    if (vertices.empty() || strength <= 0.001f) return;

    float phase = time * speed * 2.5f;
    float cosDir = std::cos(direction);
    float sinDir = std::sin(direction);

    for (auto& vert : vertices) {
        // Vertex displacement based on UV coordinates and distance from mesh origin
        float distFactor = std::sin(vert.uv.y() * 3.14159f);
        float offset = std::sin(phase + vert.uv.x() * 6.28318f) * strength * 12.0f * distFactor;

        vert.position.setX(vert.position.x() + cosDir * offset);
        vert.position.setY(vert.position.y() + sinDir * offset);
    }
}

void MeshDeformer::renderDeformedMesh(QPainter& painter, const QImage& texture,
                                      const std::vector<MeshVertex>& vertices,
                                      const std::vector<MeshTriangle>& indices,
                                      float originX, float originY, float spriteW, float spriteH) {
    if (texture.isNull() || vertices.empty() || indices.empty()) return;

    // Render deformed triangles using painter clip paths
    for (const auto& tri : indices) {
        if (tri.i0 >= vertices.size() || tri.i1 >= vertices.size() || tri.i2 >= vertices.size()) continue;

        const auto& v0 = vertices[tri.i0];
        const auto& v1 = vertices[tri.i1];
        const auto& v2 = vertices[tri.i2];

        QPointF p0(v0.position.x(), v0.position.y());
        QPointF p1(v1.position.x(), v1.position.y());
        QPointF p2(v2.position.x(), v2.position.y());

        QPolygonF poly;
        poly << p0 << p1 << p2;

        painter.save();
        QPainterPath path;
        path.addPolygon(poly);
        painter.setClipPath(path);

        // Draw texture tile mapped to local triangle bounds
        QRectF drawRect(-spriteW / 2.0f, -spriteH / 2.0f, spriteW, spriteH);
        painter.drawImage(drawRect, texture);
        painter.restore();
    }
}

} // namespace WallpaperEngine::Render
