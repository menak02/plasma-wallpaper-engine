#include "mesh_renderer.h"
#include <QPainter>
#include <QPainterPath>
#include <cmath>
#include <algorithm>
#include <limits>
#include <map>
#include <iostream>

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
        // Displace vertices by UV + distance from mesh origin.
        float distFactor = std::sin(vert.uv.y() * 3.14159f);
        float offset = std::sin(phase + vert.uv.x() * 6.28318f) * strength * 12.0f * distFactor;

        vert.position.setX(vert.position.x() + cosDir * offset);
        vert.position.setY(vert.position.y() + sinDir * offset);
    }
}

bool MeshDeformer::boneWeightedDeform(std::vector<MeshVertex>& vertices,
                                      const std::vector<DeformBone>& bones,
                                      float spriteW, float spriteH) {
    if (bones.empty() || vertices.empty()) return false;

    // Name -> index map for parent-chain resolution
    std::map<std::string, size_t> indexByName;
    for (size_t i = 0; i < bones.size(); ++i) {
        if (!bones[i].name.empty()) indexByName[bones[i].name] = i;
    }

    // Guard against parent cycles: cap chain depth at bones.size()
    const size_t maxDepth = bones.size();

    // Build world-space matrices in rest pose
    std::vector<QMatrix4x4> worldMats;
    worldMats.reserve(bones.size());
    for (const auto& bone : bones) {
        QMatrix4x4 world;
        const DeformBone* cur = &bone;
        size_t depth = 0;
        std::vector<const DeformBone*> chain;
        while (cur != nullptr && depth <= maxDepth) {
            chain.push_back(cur);
            if (cur->parent.empty()) break;
            auto it = indexByName.find(cur->parent);
            cur = (it != indexByName.end()) ? &bones[it->second] : nullptr;
            ++depth;
        }
        // Compose root -> leaf
        for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
            const DeformBone& b = **rit;
            QMatrix4x4 local;
            local.translate(b.pos + b.animatedPos);
            local.rotate(b.angle.x() + b.animatedAngle.x(), 1, 0, 0);
            local.rotate(b.angle.z() + b.animatedAngle.z(), 0, 0, 1);
            local.rotate(b.angle.y() + b.animatedAngle.y(), 0, 1, 0);
            world = world * local;
        }
        worldMats.push_back(world);
    }

    // Assign each vertex to nearest bone(s) with inverse-distance weights.
    // Bone influence positions come from their world matrices (translation
    // column), which for the rest pose equals bone.pos chain composition.
    for (auto& vert : vertices) {
        float bestDist[4] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        size_t bestIdx[4] = {0, 0, 0, 0};

        for (size_t b = 0; b < worldMats.size(); ++b) {
            QVector3D boneOrigin = worldMats[b].column(3).toVector3D();
            float dx = vert.position.x() - boneOrigin.x();
            float dy = vert.position.y() - boneOrigin.y();
            float dist = dx * dx + dy * dy;

            // Insert into the top-4 (sorted ascending). Swap a copy of the
            // loop counter: swapping `b` itself rewinds the outer loop and
            // can insert the same bone twice.
            size_t idx = b;
            for (int slot = 0; slot < 4; ++slot) {
                if (dist < bestDist[slot]) {
                    std::swap(dist, bestDist[slot]);
                    std::swap(idx, bestIdx[slot]);
                }
            }
        }

        // Inverse-distance-squared weighting; identity transform means the
        // blended matrix is the vertex's weighted rest pose. For rest-pose
        // (no animation), all world matrices are static, so the transform is
        // applied once instead of per-animated-frame: positions stay put.
        float w[4];
        float wSum = 0.0f;
        for (int slot = 0; slot < 4; ++slot) {
            w[slot] = (bestDist[slot] < std::numeric_limits<float>::max()) ? (1.0f / (bestDist[slot] + 1.0f)) : 0.0f;
            wSum += w[slot];
        }
        if (wSum <= 0.0f) continue;

        QMatrix4x4 blended;
        bool first = true;
        QVector3D pivotSum(0, 0, 0);
        for (int slot = 0; slot < 4; ++slot) {
            if (w[slot] <= 0.0f) continue;
            float norm = w[slot] / wSum;
            const QMatrix4x4& m = worldMats[bestIdx[slot]];
            if (first) {
                blended = m;
                first = false;
            } else {
                // Linear blend of matrix components (standard CPU skinning approach)
                for (int row = 0; row < 4; ++row)
                    for (int col = 0; col < 4; ++col)
                        blended(row, col) += norm * (m(row, col) - blended(row, col));
            }
            pivotSum += norm * m.column(3).toVector3D();
        }

        // Rest pose: blended matrix equals translation-only composition of
        // rest positions, which would displace vertices away from the sprite.
        // Stub contract: identity in rest pose, so subtract the blended pivot
        // and re-add the original position (no-op transform), keeping the
        // plumbing live for animated matrices later.
        QMatrix4x4 restInverse = blended.inverted();
        QVector3D pos = blended.map(QVector3D(vert.position));
        pos = restInverse.map(pos); // == original in exact arithmetic
        vert.position = pos;
    }

    return true;
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
