#pragma once
#include <QJsonObject>
#include <QString>

class QTextDocument;

namespace scenepad {
constexpr int maxRemoteText = 1024 * 1024;
QJsonObject webDocument(const QString &html);
QString plainText(const QString &html);
bool basicHtml(const QTextDocument &document, QString &html);
bool applyWebDocument(const QString &original, const QJsonObject &input, QString &html, bool *limitExceeded = nullptr);
}
