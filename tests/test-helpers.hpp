#pragma once
#include <QApplication>
#include <QElapsedTimer>
#include <QMenu>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <stdexcept>

namespace scenepad::test {
inline int checks = 0;
inline void require(bool condition, const char *message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

inline void wait(int milliseconds, int interval = 1)
{
    QElapsedTimer clock; clock.start();
    while (clock.elapsed() < milliseconds) { QApplication::processEvents(); QThread::msleep(interval); }
}

// Keep menu callbacks inside the popup's event loop.
template<class Inspect> void contextMenu(QTextEdit *editor, Inspect inspect)
{
    QTimer::singleShot(0, editor, [editor, inspect] {
        if (auto *menu = editor->findChild<QMenu *>("noteContextMenu")) {
            inspect(menu);
            menu->close();
        }
    });
    QMetaObject::invokeMethod(editor, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, QPoint(8, 8)));
}
}
