#pragma once
#include <QTextEdit>
#include <functional>

namespace scenepad {
class Editor final : public QTextEdit {
public:
    explicit Editor(QWidget *parent = nullptr);
    void loadHtml(const QString &html);
    void setSideSpace(int width);
    bool acceptsText(const QString &text, const QTextCursor &selection);
    void insertCheckedText(const QString &text);
    void deleteSelection(bool cut = false);
    void setSearchMatch(bool active);
    std::function<void()> limitReached;

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    void insertFromMimeData(const QMimeData *source) override;
    void dropEvent(QDropEvent *event) override;

private:
    void updateNumberArea();
    void paintNumbers();
    void resizeText(int steps);
    QWidget *numbers_ = nullptr;
    int wheelDelta_ = 0;
    bool restyling_ = false;
    bool searchMatch_ = false;
};
}
