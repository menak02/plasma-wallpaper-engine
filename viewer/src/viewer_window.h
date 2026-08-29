#pragma once

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QDBusInterface>
#include <QProcess>
#include <QTimer>

class LiveViewport : public QWidget {
    Q_OBJECT
public:
    explicit LiveViewport(QWidget* parent = nullptr);
    ~LiveViewport() override;

    void updateBuffer(int fd, uint32_t width, uint32_t height, uint32_t stride, size_t size);
    void triggerRedraw();

Q_SIGNALS:
    void mouseMoved(float normX, float normY);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    int m_fd = -1;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_stride = 0;
    size_t m_size = 0;
    void* m_mappedPtr = nullptr;
};

class ViewerWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit ViewerWindow(QWidget* parent = nullptr);
    ~ViewerWindow() override = default;

    void loadPath(const QString& path);

public Q_SLOTS:
    void checkConnection();
    void openPkgFile();
    void onFrameReady();
    void onWallpaperLoaded(const QString& title);

private:
    void ensureDaemonRunning();

    LiveViewport* m_viewport = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_infoLabel = nullptr;
    QLabel* m_fpsLabel = nullptr;
    int m_frameCount = 0;
    int m_activeFd = -1;
    QString m_pendingPath;
    QProcess* m_daemonProc = nullptr;
};
