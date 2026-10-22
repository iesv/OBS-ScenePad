#include "icons.hpp"
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace scenepad {
namespace {
class MonoIcon final : public QIconEngine {
public:
    MonoIcon(Icon icon, const QPalette &palette) : icon_(icon), palette_(palette) {}
    QIconEngine *clone() const override { return new MonoIcon(*this); }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const qreal scale = qMin(rect.width(), rect.height()) / 24.0;
        painter->translate(rect.x() + (rect.width() - 24 * scale) / 2,
            rect.y() + (rect.height() - 24 * scale) / 2);
        painter->scale(scale, scale);
        auto color = palette_.color(QPalette::Active, QPalette::WindowText);
        if (mode == QIcon::Disabled) color.setAlpha(qMin(color.alpha(), 110));
        painter->setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        QPainterPath path;
        switch (icon_) {
        case Icon::Mark:
            path.moveTo(12, 4); path.lineTo(20, 12); path.lineTo(12, 20); path.lineTo(4, 12); path.closeSubpath();
            break;
        case Icon::NextMark:
            path.moveTo(8, 5); path.lineTo(14, 12); path.lineTo(8, 19); path.lineTo(2, 12); path.closeSubpath();
            path.moveTo(17, 7); path.lineTo(22, 12); path.lineTo(17, 17);
            break;
        case Icon::QR:
            path.addRect(QRectF(3, 3, 6, 6));
            path.addRect(QRectF(15, 3, 6, 6));
            path.addRect(QRectF(3, 15, 6, 6));
            path.moveTo(15, 14); path.lineTo(15, 18); path.lineTo(12, 18);
            path.moveTo(18, 12); path.lineTo(21, 12); path.lineTo(21, 16);
            path.moveTo(18, 21); path.lineTo(21, 21); path.lineTo(21, 19);
            break;
        case Icon::Return:
            path.moveTo(9, 5); path.lineTo(4, 10); path.lineTo(9, 15);
            path.moveTo(4, 10); path.lineTo(14, 10); path.cubicTo(18, 10, 20, 12, 20, 16); path.lineTo(20, 19);
            break;
        case Icon::Previous:
            path.moveTo(6, 15); path.lineTo(12, 9); path.lineTo(18, 15);
            break;
        case Icon::Next:
            path.moveTo(6, 9); path.lineTo(12, 15); path.lineTo(18, 9);
            break;
        case Icon::Close:
            path.moveTo(6, 6); path.lineTo(18, 18);
            path.moveTo(18, 6); path.lineTo(6, 18);
            break;
        case Icon::Import:
        case Icon::Export: {
            path.moveTo(5, 16); path.lineTo(5, 20); path.lineTo(19, 20); path.lineTo(19, 16);
            if (icon_ == Icon::Import) {
                path.moveTo(12, 4); path.lineTo(12, 14);
                path.moveTo(8, 10); path.lineTo(12, 14); path.lineTo(16, 10);
            } else {
                path.moveTo(12, 14); path.lineTo(12, 4);
                path.moveTo(8, 8); path.lineTo(12, 4); path.lineTo(16, 8);
            }
            break;
        }
        case Icon::Bold:
            painter->setPen(QPen(color, 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            path.moveTo(7, 4); path.lineTo(7, 20); path.lineTo(13, 20);
            path.cubicTo(21, 20, 21, 12, 13, 12); path.lineTo(7, 12);
            path.moveTo(7, 4); path.lineTo(12, 4); path.cubicTo(20, 4, 20, 12, 12, 12);
            break;
        case Icon::Font:
            path.moveTo(2, 19); path.lineTo(7, 5); path.lineTo(12, 19);
            path.moveTo(4, 14); path.lineTo(10, 14);
            path.addEllipse(QRectF(14, 11, 7, 8));
            path.moveTo(21, 11); path.lineTo(21, 19);
            break;
        case Icon::Italic:
            path.moveTo(10, 4); path.lineTo(18, 4);
            path.moveTo(6, 20); path.lineTo(14, 20);
            path.moveTo(15, 4); path.lineTo(9, 20);
            break;
        case Icon::Underline:
            path.moveTo(6, 4); path.lineTo(6, 12); path.cubicTo(6, 20, 18, 20, 18, 12); path.lineTo(18, 4);
            path.moveTo(5, 22); path.lineTo(19, 22);
            break;
        case Icon::Strike:
            path.moveTo(18, 6); path.cubicTo(7, 0, 2, 11, 12, 12);
            path.cubicTo(22, 13, 17, 24, 6, 18);
            path.moveTo(4, 12); path.lineTo(20, 12);
            break;
        case Icon::Paint:
            path.addRoundedRect(QRectF(4, 3, 16, 7), 1, 1);
            path.moveTo(18, 10); path.lineTo(18, 14); path.lineTo(12, 14); path.lineTo(12, 21);
            break;
        case Icon::TextColor:
            path.moveTo(5, 17); path.lineTo(12, 3); path.lineTo(19, 17);
            path.moveTo(8, 12); path.lineTo(16, 12);
            path.moveTo(4, 22); path.lineTo(20, 22);
            break;
        case Icon::Highlight:
            path.moveTo(6, 13); path.lineTo(15, 4); path.lineTo(20, 9); path.lineTo(11, 18); path.closeSubpath();
            path.moveTo(6, 13); path.lineTo(4, 19); path.lineTo(11, 18);
            path.moveTo(4, 22); path.lineTo(20, 22);
            break;
        case Icon::List:
            for (int y : {6, 12, 18}) {
                path.addEllipse(QPointF(4, y), 0.6, 0.6);
                path.moveTo(9, y); path.lineTo(20, y);
            }
            break;
        case Icon::Clear:
            path.moveTo(8, 20); path.lineTo(3, 15); path.lineTo(13, 5);
            path.lineTo(21, 13); path.lineTo(14, 20); path.closeSubpath();
            path.moveTo(8, 10); path.lineTo(16, 18);
            path.moveTo(8, 20); path.lineTo(21, 20);
            break;
        case Icon::AlignLeft:
        case Icon::AlignCenter:
        case Icon::AlignRight:
        case Icon::AlignJustify:
            for (int row = 0; row < 4; ++row) {
                const int length = row % 2 && icon_ != Icon::AlignJustify ? 12 : 18;
                const int x = icon_ == Icon::AlignCenter ? (24 - length) / 2 :
                    (icon_ == Icon::AlignRight ? 21 - length : 3);
                path.moveTo(x, 5 + row * 5); path.lineTo(x + length, 5 + row * 5);
            }
            break;
        case Icon::IndentLess:
        case Icon::IndentMore:
            path.moveTo(3, 5); path.lineTo(21, 5);
            path.moveTo(12, 10); path.lineTo(21, 10);
            path.moveTo(12, 15); path.lineTo(21, 15);
            path.moveTo(3, 20); path.lineTo(21, 20);
            path.moveTo(3, 12); path.lineTo(8, 12);
            if (icon_ == Icon::IndentLess) {
                path.moveTo(6, 9); path.lineTo(3, 12); path.lineTo(6, 15);
            } else {
                path.moveTo(5, 9); path.lineTo(8, 12); path.lineTo(5, 15);
            }
            break;
        case Icon::Time:
            path.addEllipse(QPointF(12, 12), 8, 8);
            path.moveTo(12, 7); path.lineTo(12, 12); path.lineTo(16, 14);
            break;
        case Icon::Play:
            path.moveTo(8, 5); path.lineTo(19, 12); path.lineTo(8, 19); path.closeSubpath();
            painter->setBrush(color);
            break;
        case Icon::Pause:
            path.addRoundedRect(QRectF(6, 5, 4, 14), 0.5, 0.5);
            path.addRoundedRect(QRectF(14, 5, 4, 14), 0.5, 0.5);
            painter->setBrush(color);
            break;
        case Icon::Top:
            path.moveTo(5, 3); path.lineTo(19, 3);
            path.moveTo(12, 21); path.lineTo(12, 7);
            path.moveTo(6, 13); path.lineTo(12, 7); path.lineTo(18, 13);
            break;
        case Icon::Read:
            path.addRoundedRect(QRectF(5, 10, 14, 11), 1.5, 1.5);
            path.moveTo(8, 10); path.lineTo(8, 7);
            if (state == QIcon::On) {
                path.cubicTo(8, 1, 16, 1, 16, 7); path.lineTo(16, 10);
            } else {
                path.cubicTo(8, 0, 19, 0, 19, 6); path.lineTo(19, 7);
            }
            path.moveTo(12, 14); path.lineTo(12, 17);
            break;
        case Icon::ReadingGuide:
            path.moveTo(7, 5); path.lineTo(20, 5);
            path.moveTo(7, 19); path.lineTo(20, 19);
            path.moveTo(2, 9); path.lineTo(5, 12); path.lineTo(2, 15);
            path.moveTo(9, 12); path.lineTo(21, 12);
            break;
        case Icon::Overflow:
            path.moveTo(5, 6); path.lineTo(11, 12); path.lineTo(5, 18);
            path.moveTo(13, 6); path.lineTo(19, 12); path.lineTo(13, 18);
            break;
        }
        // Center the artwork, including underline and color strokes.
        const auto center = path.boundingRect().center();
        painter->translate(12 - center.x(), 12 - center.y());
        painter->drawPath(path);
        painter->restore();
    }
private:
    Icon icon_;
    QPalette palette_;
};
}
QIcon makeIcon(Icon icon, const QPalette &palette) { return QIcon(new MonoIcon(icon, palette)); }
}
