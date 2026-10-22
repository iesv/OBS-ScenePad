#pragma once
#include <QIcon>
#include <QPalette>

namespace scenepad {
enum class Icon { Import, Export, Font, Bold, Italic, Underline, TextColor, Highlight, Clear,
    AlignLeft, AlignCenter, AlignRight, AlignJustify, IndentLess, IndentMore, Time,
    List, Play, Pause, Top, Read, ReadingGuide, Overflow, Previous, Next, Close, QR, Mark, NextMark, Strike, Paint, Return };
QIcon makeIcon(Icon icon, const QPalette &palette);
}
