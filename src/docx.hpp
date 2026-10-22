#pragma once
#include <QString>

namespace scenepad {
struct DocxResult {
    QString html;
    QString error;
    bool simplified = false;
};
DocxResult importDocx(const QString &path);
}
