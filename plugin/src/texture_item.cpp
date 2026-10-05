#include "texture_item.h"
#include <QSGRectangleNode>
#include <QQuickWindow>

TextureItem::TextureItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

TextureItem::~TextureItem() = default;

QSGNode *TextureItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGRectangleNode *>(oldNode);
    if (!node) {
        node = window()->createRectangleNode();
        node->setColor(Qt::black); // Placeholder black screen
    }

    // In Phase 2, this will be replaced with a texture node holding the dmabuf FD
    node->setRect(boundingRect());

    return node;
}
