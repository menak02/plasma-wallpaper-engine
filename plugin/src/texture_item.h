#pragma once
#include <QQuickItem>

#include <QtQml/qqmlregistration.h>

class TextureItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
public:
    explicit TextureItem(QQuickItem *parent = nullptr);
    ~TextureItem() override;

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *updatePaintNodeData) override;
};
