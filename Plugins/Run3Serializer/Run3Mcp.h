#pragma once
#include "Run3Document.h"
#include <QJsonObject>
#include <QObject>
#include <functional>

class QToolBar;
class QTcpServer;
class QPushButton;
class QSpinBox;
class QLabel;

namespace Run3Editor {
struct McpContext {
    std::function<Document*()> document;
    std::function<QJsonObject(const QString&, const QJsonObject&)> lifecycle;
};

// All callbacks execute on the Qt GUI thread. The external stdio process only
// translates MCP messages; it never touches Ogre pointers or map files.
class McpBridge : public QObject {
public:
    explicit McpBridge(McpContext context);
    ~McpBridge() override;
    QToolBar* toolbar() const { return mToolbar; }
    bool start(int port);
    void stop();
    QJsonObject dispatch(const QString& tool, const QJsonObject& args);
private:
    McpContext mContext;
    QTcpServer* mServer;
    QToolBar* mToolbar;
    QPushButton* mToggle;
    QSpinBox* mPort;
    QLabel* mStatus;
    QString mToken, mDescriptor;
    bool mBusy = false;
};
}
