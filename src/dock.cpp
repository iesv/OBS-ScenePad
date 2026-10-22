#include "dock.hpp"
#include "editor.hpp"
#include "locale.hpp"
#include "icons.hpp"
#include "docx.hpp"
#include "notes.hpp"
#include "backup.hpp"
#include "text-limits.hpp"
#include "remote.hpp"
#include "web-document.hpp"
#include <QActionGroup>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QColorDialog>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressDialog>
#include <QPropertyAnimation>
#include <QWheelEvent>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSlider>
#include <QSpinBox>
#include <QTextBlock>
#include <QTextEdit>
#include <QTextList>
#include <QTemporaryDir>
#include <QThread>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <algorithm>
#include <cmath>
#include <functional>

namespace scenepad {
namespace {
const QSize buttonSize(28, 24);
const QSize iconSize(18, 18);
constexpr double BaseScrollSpeed = 10.0;
constexpr int MaxAutoClearDays = 36500;
const char *inlineInputStyle = "QLineEdit { min-width: 0px; min-height: 0px; padding: 0px 3px; border: 1px solid palette(highlight); border-radius: 3px; color: palette(text); background: palette(base); } QLineEdit[invalid=\"true\"] { border-color: #b96359; }";
QTextCursor markAt(QTextCursor cursor)
{
    // Inspect one paragraph, never the full script.
    const auto line = cursor.block().text();
    const int position = cursor.position() - cursor.block().position();
    const int start = line.lastIndexOf(QChar(0x25c6), qMin(position, int(line.size()) - 1));
    if (start < 0) return {};
    int end = start + 1;
    if (line.mid(end, 1) == "[") {
        const int closing = line.indexOf(']', end + 1);
        if (closing > end) {
            const auto note = line.mid(end + 1, closing - end - 1);
            if (!note.contains(QChar(0x25c6)) && !note.contains(QChar::LineSeparator)) end = closing + 1;
        }
    }
    if (position > end) return {};
    const int offset = cursor.block().position();
    cursor.setPosition(offset + start); cursor.setPosition(offset + end, QTextCursor::KeepAnchor);
    return cursor;
}
void fileJob(QWidget *parent, const QString &label, const std::function<void()> &work)
{
    // Compression and backup validation run without blocking OBS rendering or the UI loop.
    QProgressDialog progress(label, {}, 0, 0, parent);
    progress.setCancelButton(nullptr);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
    progress.setMinimumDuration(0);
    QEventLoop loop;
    auto *worker = QThread::create(work);
    QObject::connect(worker, &QThread::finished, &loop, &QEventLoop::quit);
    progress.show();
    worker->start();
    loop.exec();
    worker->wait();
    delete worker;
}
class ReadingBand final : public QWidget {
public:
    explicit ReadingBand(QWidget *parent) : QWidget(parent)
    {
        setObjectName("readingBand");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        hide();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const bool dark = palette().color(QPalette::Base).lightness() < 128;
        auto color = dark ? QColor(56, 189, 248) : QColor(0, 119, 204);
        color.setAlpha(dark ? 54 : 36);
        painter.fillRect(rect(), color);
        color.setAlpha(210);
        painter.setPen(QPen(color, 2));
        painter.drawLine(0, 1, width() - 1, 1);
        painter.drawLine(0, height() - 2, width() - 1, height() - 2);
    }
};
}
Dock::Dock(const QString &settingsFile, QWidget *parent)
    : QWidget(parent), preferences_(settingsFile, QSettings::IniFormat),
      notes_(QFileInfo(settingsFile).dir().filePath("notes"))
{
    setObjectName("ScenePadContent");
    locked_ = preferences_.value("locked", false).toBool();
    setMinimumSize(700, 300);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(3);
    auto *topControls = new QWidget(this);
    topControls->setObjectName("topControls");
    auto *header = new QHBoxLayout(topControls);
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(4);
    auto *scenePrefix = new QLabel("[", topControls);
    scenePrefix->setObjectName("scenePrefix");
    scenePrefix->setFixedWidth(10);
    scenePrefix->setAlignment(Qt::AlignCenter);
    header->addWidget(scenePrefix);
    sceneLabel_ = new QLabel(text("NoScene"), this);
    sceneLabel_->setObjectName("sceneLabel");
    sceneLabel_->setTextFormat(Qt::PlainText);
    sceneLabel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    sceneLabel_->setMaximumWidth(140);
    sceneLabel_->setMinimumWidth(20);
    header->addWidget(sceneLabel_);
    root->addWidget(topControls);
    saveError_ = new QLabel(this);
    saveError_->setObjectName("saveError");
    saveError_->setTextFormat(Qt::PlainText);
    saveError_->setContentsMargins(6, 3, 6, 3);
    saveError_->hide();
    root->addWidget(saveError_);
    editor_ = new Editor(this);
    static_cast<Editor *>(editor_)->limitReached = [this] {
        showStatus(text("TextLimit"));
        QToolTip::showText(editor_->viewport()->mapToGlobal(editor_->cursorRect().bottomRight()), text("TextLimit"), editor_);
    };
    editor_->setObjectName("noteEditor");
    editor_->setAcceptRichText(true);
    editor_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(editor_, &QTextEdit::customContextMenuRequested, this, [this](const QPoint &position) {
        showEditorContextMenu(position);
    });
    editor_->installEventFilter(this);
    editor_->viewport()->installEventFilter(this);
    readingBand_ = new ReadingBand(editor_->viewport());
    smoothScroll_ = new QPropertyAnimation(editor_->verticalScrollBar(), "value", this);
    smoothScroll_->setObjectName("readingScrollAnimation");
    smoothScroll_->setDuration(120); smoothScroll_->setEasingCurve(QEasingCurve::OutCubic);
    connect(smoothScroll_, &QPropertyAnimation::finished, this, [this] {
        scrollPosition_ = editor_->verticalScrollBar()->value(); manualScrollClock_.invalidate(); scrollClock_.restart();
    });
    qrButton_ = new QToolButton(this);
    qrButton_->setObjectName("remoteQrButton");
    qrButton_->setFixedSize(24, 24);
    qrButton_->setIconSize(iconSize);
    qrButton_->setAutoRaise(true);
    qrButton_->setFocusPolicy(Qt::NoFocus);
    qrButton_->setIcon(makeIcon(Icon::QR, palette()));
    qrButton_->setToolTip(text("RemoteQR"));
    qrButton_->hide();
    connect(qrButton_, &QToolButton::clicked, this, [this] {
        const auto link = remote_->url();
        if (!link.isEmpty()) QApplication::clipboard()->setText(link);
        remote_->toggleQr(editor_->viewport());
    });
    returnButton_ = new QToolButton(this);
    returnButton_->setObjectName("returnPosition");
    returnButton_->setFixedSize(qrButton_->size());
    returnButton_->setIconSize(iconSize);
    returnButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    returnButton_->setAutoRaise(true);
    returnButton_->setIcon(makeIcon(Icon::Return, palette()));
    returnButton_->setAccessibleName(text("ReturnPosition"));
    returnButton_->setToolTip(text("ReturnPosition"));
    returnButton_->setFocusPolicy(Qt::NoFocus);
    returnButton_->hide();
    connect(returnButton_, &QToolButton::clicked, this, [this] { returnToPosition(); });
    auto holdScroll = [this] { smoothScroll_->stop(); if (isScrolling()) manualScrollClock_.restart(); };
    connect(editor_->verticalScrollBar(), &QScrollBar::sliderPressed, this, holdScroll);
    connect(editor_->verticalScrollBar(), &QScrollBar::sliderReleased, this, holdScroll);
    root->addWidget(editor_, 1);
    createToolbar();
    createSearchField();
    header->addWidget(toolbar_);
    header->addWidget(searchInput_, 1);

    // Paragraph tools share a single row with the scrolling controls.
    controls_ = new QWidget(this);
    controls_->setObjectName("bottomControls");
    controls_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *controlsLayout = new QBoxLayout(QBoxLayout::LeftToRight, controls_);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(8);
    readingControls_ = new QWidget(controls_);
    readingControls_->setObjectName("readingControls");
    auto *readingLayout = new QHBoxLayout(readingControls_);
    readingLayout->setContentsMargins(0, 0, 0, 0);
    readingLayout->setSpacing(2);
    controlsLayout->addWidget(readingControls_, 1);
    metrics_ = new QWidget(controls_);
    metrics_->setObjectName("readingMetrics");
    auto *metricsLayout = new QHBoxLayout(metrics_);
    metricsLayout->setContentsMargins(0, 0, 0, 0);
    metricsLayout->setSpacing(6);
    controlsLayout->addWidget(metrics_, 0, Qt::AlignRight);
    controls_->installEventFilter(this);
    playButton_ = new QToolButton(this);
    playButton_->setObjectName("scrollPlay");
    playButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    playButton_->setFixedSize(buttonSize);
    playButton_->setIconSize(iconSize);
    playButton_->setStyleSheet("QToolButton { padding: 0px; }");
    playButton_->setText(text("Scroll"));
    playButton_->setIcon(makeIcon(Icon::Play, palette()));
    playButton_->setToolTip(text("ScrollTip"));
    readingLayout->addWidget(playButton_);
    auto *guideButton = new QToolButton(this);
    guideButton->setObjectName("readingGuide");
    guideButton->setFixedSize(buttonSize);
    guideButton->setIconSize(iconSize);
    guideButton->setStyleSheet("QToolButton { padding: 0px; }");
    readingGuide_ = new QAction(makeIcon(Icon::ReadingGuide, palette()), text("ReadingGuide"), this);
    readingGuide_->setCheckable(true);
    readingGuide_->setChecked(preferences_.value("readingGuide", true).toBool());
    readingGuide_->setToolTip(text("ReadingGuide"));
    guideButton->setDefaultAction(readingGuide_);
    connect(readingGuide_, &QAction::toggled, this, [this](bool enabled) {
        preferences_.setValue("readingGuide", enabled);
        updateReadingBand();
    });
    auto *top = new QToolButton(this);
    top->setObjectName("scrollTop");
    top->setFixedSize(buttonSize);
    top->setIconSize(iconSize);
    top->setStyleSheet("QToolButton { padding: 0px; }");
    top->setIcon(makeIcon(Icon::Top, palette()));
    top->setToolTip(text("TopTip"));
    readingLayout->addWidget(top);
    readingLayout->addWidget(guideButton);
    countdown_ = new QCheckBox("3s", controls_);
    countdown_->setObjectName("startCountdown");
    countdown_->setToolTip(text("Countdown"));
    countdown_->setAccessibleName(text("Countdown"));
    countdown_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    countdown_->setStyleSheet("QCheckBox { padding: 0px; spacing: 3px; }");
    countdown_->setChecked(preferences_.value("startCountdown", false).toBool());
    connect(countdown_, &QCheckBox::toggled, this, [this](bool enabled) {
        preferences_.setValue("startCountdown", enabled);
        if (isCountingDown()) pauseScroll();
    });
    readingLayout->addWidget(countdown_);
    readingLayout->addWidget(paragraphToolbar_);
    speedBox_ = new QDoubleSpinBox(this);
    speedBox_->setObjectName("scrollSpeed");
    speedBox_->setRange(0.1, 5.0);
    speedBox_->setDecimals(1);
    speedBox_->setSingleStep(0.1);
    speedBox_->setSuffix("×");
    speedBox_->setToolTip(text("SpeedTip"));
    speedBox_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    speedBox_->setAlignment(Qt::AlignCenter);
    speedBox_->setStyleSheet("QDoubleSpinBox { min-height: 0px; min-width: 0px; padding: 2px 6px; }");
    auto *lockButton = new QToolButton(this);
    lockButton->setObjectName("readMode");
    lockButton->setFixedSize(buttonSize);
    lockButton->setIconSize(iconSize);
    lockButton->setStyleSheet("QToolButton { padding: 0px; }");
    lockButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    lock_ = new QAction(text("Read"), this);
    lock_->setIcon(makeIcon(Icon::Read, palette()));
    lock_->setCheckable(true);
    lock_->setToolTip(text("ReadTip"));
    lockButton->setDefaultAction(lock_);
    speedSlider_ = new QSlider(Qt::Horizontal, this);
    speedSlider_->setObjectName("scrollSlider");
    speedSlider_->setRange(1, 50);
    speedSlider_->setValue(5);
    speedSlider_->setToolTip(text("SpeedTip"));
    speedSlider_->setAccessibleName(text("Speed"));
    speedSlider_->setMinimumWidth(8);
    auto addSpeedSeparator = [this, readingLayout](const char *name) {
        auto *line = new QFrame(readingControls_);
        line->setObjectName(name);
        line->setFixedSize(1, 12);
        readingLayout->addSpacing(4);
        readingLayout->addWidget(line, 0, Qt::AlignVCenter);
        readingLayout->addSpacing(4);
    };
    addSpeedSeparator("speedStartSeparator");
    readingLayout->addWidget(speedSlider_, 1);
    readingLayout->addWidget(speedBox_);
    addSpeedSeparator("speedEndSeparator");
    documentStatus_ = new QLabel(metrics_);
    documentStatus_->setObjectName("documentStatus");
    documentStatus_->setTextFormat(Qt::PlainText);
    documentStatus_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    characters_ = new QLabel(metrics_);
    characters_->setObjectName("characterCount");
    characters_->setTextFormat(Qt::PlainText);
    characters_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    scrollPercent_ = new QLabel(metrics_);
    scrollPercent_->setObjectName("scrollPercent");
    scrollPercent_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    for (auto *label : {documentStatus_, characters_, scrollPercent_}) {
        if (label != documentStatus_) {
            auto *separator = new QFrame(metrics_);
            separator->setObjectName(label == characters_ ? "characterSeparator" : "percentSeparator");
            separator->setFixedSize(1, 12);
            metricsLayout->addWidget(separator, 0, Qt::AlignVCenter);
        }
        metricsLayout->addWidget(label);
    }
    for (auto *label : {documentStatus_, scrollPercent_}) {
        label->setCursor(Qt::PointingHandCursor);
        label->setToolTip(text("GoTo"));
        label->installEventFilter(this);
    }
    readingLayout->insertWidget(0, lockButton);
    status_ = new QLabel(this);
    status_->setObjectName("saveStatus");
    status_->setTextFormat(Qt::PlainText);
    status_->setFixedWidth(10);
    status_->setAlignment(Qt::AlignCenter);
    header->insertWidget(2, status_);
    root->addWidget(controls_);
    // Keep compact controls inside their layout cells on macOS.
    for (auto *row : {header, readingLayout, metricsLayout})
        for (int i = 0; i < row->count(); ++i)
            if (auto *widget = row->itemAt(i)->widget()) widget->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    showStatus(text("InitialStatus"));
    updateAppearance();
    updateControlSizes();

    // Debounce scene-note saves while typing.
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(500);
    connect(&saveTimer_, &QTimer::timeout, this, [this] { flush(); });
    scrollTimer_.setTimerType(Qt::PreciseTimer);
    scrollTimer_.setInterval(16);
    connect(&scrollTimer_, &QTimer::timeout, this, [this] { scrollFrame(); });
    countdownTimer_.setTimerType(Qt::PreciseTimer);
    countdownTimer_.setInterval(100);
    connect(&countdownTimer_, &QTimer::timeout, this, [this] { updateCountdown(); });
    connect(editor_, &QTextEdit::textChanged, this, [this] {
        if (restoring_ || noteLoadFailed_) return;
        clearReturnPosition();
        smoothScroll_->stop();
        setFormatPainter(false);
        dirty_ = true;
        if (remote_) remote_->noteChanged(sceneId_);
        showStatus(text("Saving"));
        updateCharacterStatus();
        saveTimer_.start();
    });
    connect(editor_, &QTextEdit::selectionChanged, this, [this] { clearColorPreview(); updateCharacterStatus(); });
    connect(editor_, &QTextEdit::currentCharFormatChanged, this, [this] { syncFormat(); });
    connect(editor_, &QTextEdit::cursorPositionChanged, this, [this] {
        updateMarkFormat();
        syncFormat();
        if (!searchDirty_) updateSearchStatus();
    });
    connect(editor_->document(), &QTextDocument::contentsChange, this, [this](int, int removed, int added) {
        if (!removed && !added) return;
        // Text edits end automatic mark search without moving the caret.
        if (markSearch_) searchInput_->clear();
        else invalidateSearch();
    });
    connect(editor_->document(), &QTextDocument::blockCountChanged, this, &Dock::updateDocumentStatus);
    connect(editor_->verticalScrollBar(), &QScrollBar::valueChanged, this, &Dock::updateDocumentStatus);
    connect(editor_->verticalScrollBar(), &QScrollBar::rangeChanged, this, &Dock::updateDocumentStatus);
    connect(editor_->verticalScrollBar(), &QScrollBar::valueChanged, this, &Dock::updateReadingBand);
    connect(editor_->verticalScrollBar(), &QScrollBar::rangeChanged, this, &Dock::updateReadingBand);
    connect(playButton_, &QToolButton::clicked, this, [this] { command(Command::Toggle); });
    connect(top, &QToolButton::clicked, this, [this] { command(Command::Top); });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *) {
        if (shuttingDown_ || !goToInput_ || !goToInput_->isVisible()) return;
        // Resolve focus after popup and window transitions.
        QTimer::singleShot(0, this, [this] {
            if (shuttingDown_ || QApplication::activePopupWidget()) return;
            auto *focused = QApplication::focusWidget();
            if (goToInput_ && goToInput_->isVisible() && focused != goToInput_) goToInput_->hide();
        });
    });
    connect(lock_, &QAction::toggled, this, [this](bool locked) { setLocked(locked); });
    connect(speedSlider_, &QSlider::valueChanged, this, [this](int value) { setSpeed(value / 10.0); });
    connect(speedBox_, &QDoubleSpinBox::valueChanged, this, [this](double value) { setSpeed(value); });
    setSpeed(0.5);
    bindScene(nullptr);
    remote_ = new Remote(*this, preferences_);
    autoClearDays_ = preferences_.value("autoClear/days", 0).toInt();
    if (autoClearDays_ < 0 || autoClearDays_ > MaxAutoClearDays) autoClearDays_ = 0;
    autoClearAll_ = preferences_.value("autoClear/all", false).toBool();
    const auto selected = preferences_.value("autoClear/scenes").toStringList();
    autoClearSelection_ = QSet<QString>(selected.cbegin(), selected.cend());
    autoClearTimer_.setParent(this);
    autoClearTimer_.setObjectName("autoClearTimer");
    autoClearTimer_.setSingleShot(true);
    autoClearTimer_.setTimerType(Qt::CoarseTimer);
    connect(&autoClearTimer_, &QTimer::timeout, this, &Dock::clearExpiredNotes);
}

Dock::~Dock() { shutdown(); }

void Dock::createToolbar()
{
    toolbar_ = new QToolBar(this);
    toolbar_->setObjectName("formatToolbar");
    paragraphToolbar_ = new QToolBar(this);
    paragraphToolbar_->setObjectName("paragraphToolbar");
    for (auto *bar : {toolbar_, paragraphToolbar_}) {
        bar->setIconSize(iconSize);
        bar->setMovable(false);
        bar->setFloatable(false);
        // Allow native toolbars to shrink into overflow.
        bar->setMinimumWidth(1);
    }
    toolbar_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    paragraphToolbar_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto add = [this](QToolBar *bar, Icon icon, const QString &tip, const char *name) {
        auto *action = bar->addAction(makeIcon(icon, palette()), tip);
        action->setProperty("scenePadIcon", static_cast<int>(icon));
        action->setObjectName(name);
        return action;
    };
    auto *import = add(toolbar_, Icon::Import, text("Import"), "importDocument");
    connect(import, &QAction::triggered, this, [this] { importFile(); });
    auto *exportAction = add(toolbar_, Icon::Export, text("Export"), "exportDocument");
    connect(exportAction, &QAction::triggered, this, [this] { exportDocument(); });
    toolbar_->addSeparator();
    auto *font = add(toolbar_, Icon::Font, text("FontMenu"), "chooseFont");
    connect(font, &QAction::triggered, this, [this] {
        if (editor_->isReadOnly()) return;
        bool ok = false;
        const auto value = QFontDialog::getFont(&ok, editor_->currentFont(), this, text("Font"));
        if (ok) { QTextCharFormat f; f.setFont(value); applyFormat(f); }
    });
    fontSize_ = new QSpinBox(toolbar_);
    fontSize_->setObjectName("fontSize");
    fontSize_->setRange(6, 144);
    fontSize_->setToolTip(text("FontSizeTip"));
    fontSize_->setAccessibleName(text("FontSizeTip"));
    fontSize_->setKeyboardTracking(false);
    fontSize_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    fontSize_->setAlignment(Qt::AlignCenter);
    fontSize_->setStyleSheet("QSpinBox { min-height: 0px; min-width: 0px; padding: 2px 6px; } QSpinBox QLineEdit { background: transparent; border: 0px; border-radius: 0px; padding: 0px; margin: 0px; min-height: 0px; min-width: 0px; }");
    fontSize_->setValue(12);
    toolbar_->addWidget(fontSize_);
    connect(fontSize_, &QSpinBox::valueChanged, this, [this](int value) {
        QTextCharFormat f; f.setFontPointSize(value); applyFormat(f);
    });
    connect(fontSize_, &QSpinBox::editingFinished, this, [this] {
        // Clear numeric selection after input commits.
        QTimer::singleShot(0, fontSize_, [this] {
            if (auto *input = fontSize_->findChild<QLineEdit *>(); input && !fontSize_->hasFocus()) input->deselect();
            fontSize_->update();
        });
    });
    toolbar_->addSeparator();
    bold_ = add(toolbar_, Icon::Bold, text("Bold"), "formatBold");
    italic_ = add(toolbar_, Icon::Italic, text("Italic"), "formatItalic");
    underline_ = add(toolbar_, Icon::Underline, text("Underline"), "formatUnderline");
    strike_ = add(toolbar_, Icon::Strike, text("Strikethrough"), "formatStrike");
    for (auto *action : {bold_, italic_, underline_, strike_}) action->setCheckable(true);
    bold_->setShortcut(QKeySequence::Bold);
    italic_->setShortcut(QKeySequence::Italic);
    underline_->setShortcut(QKeySequence::Underline);
    for (auto *action : {bold_, italic_, underline_}) {
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(action);
    }
    connect(bold_, &QAction::triggered, this, [this](bool checked) {
        QTextCharFormat f; f.setFontWeight(checked ? QFont::Bold : QFont::Normal); applyFormat(f);
    });
    connect(italic_, &QAction::triggered, this, [this](bool checked) {
        QTextCharFormat f; f.setFontItalic(checked); applyFormat(f);
    });
    connect(underline_, &QAction::triggered, this, [this](bool checked) {
        QTextCharFormat f; f.setFontUnderline(checked); applyFormat(f);
    });
    connect(strike_, &QAction::triggered, this, [this](bool checked) {
        QTextCharFormat f; f.setFontStrikeOut(checked); applyFormat(f);
    });
    toolbar_->addSeparator();
    auto *color = add(toolbar_, Icon::TextColor, text("TextColor"), "textColor");
    connect(color, &QAction::triggered, this, [this] {
        const auto value = QColorDialog::getColor(editor_->textColor(), this, text("TextColor"));
        if (value.isValid()) { QTextCharFormat f; f.setForeground(value); applyFormat(f); }
    });
    auto *highlight = add(toolbar_, Icon::Highlight, text("Highlight"), "highlight");
    connect(highlight, &QAction::triggered, this, [this] {
        const auto value = QColorDialog::getColor(editor_->textBackgroundColor(), this, text("Highlight"));
        if (value.isValid()) { QTextCharFormat f; f.setBackground(value); applyFormat(f); }
    });
    auto *clear = add(toolbar_, Icon::Clear, text("ClearFormat"), "clearFormat");
    connect(clear, &QAction::triggered, this, [this] { clearFormat(); });
    paint_ = add(toolbar_, Icon::Paint, text("FormatPainter"), "formatPainter");
    paint_->setCheckable(true);
    connect(paint_, &QAction::triggered, this, [this](bool active) { setFormatPainter(active); });
    toolbar_->widgetForAction(paint_)->installEventFilter(this);
    toolbar_->addSeparator();
    auto *mark = add(toolbar_, Icon::Mark, text("InsertMark"), "insertMark");
    connect(mark, &QAction::triggered, this, [this] { command(Command::InsertMark); });
    auto *nextMark = add(toolbar_, Icon::NextMark, text("NextMark"), "nextMark");
    connect(nextMark, &QAction::triggered, this, [this] { command(Command::NextMark); });

    paragraphToolbar_->addSeparator();
    alignment_ = new QActionGroup(paragraphToolbar_);
    const struct { const char *key; const char *name; Icon icon; Qt::Alignment value; } alignments[] = {
        {"AlignLeft", "alignLeft", Icon::AlignLeft, Qt::AlignLeft},
        {"AlignCenter", "alignCenter", Icon::AlignCenter, Qt::AlignHCenter},
        {"AlignRight", "alignRight", Icon::AlignRight, Qt::AlignRight},
        {"AlignJustify", "alignJustify", Icon::AlignJustify, Qt::AlignJustify}};
    for (const auto &entry : alignments) {
        auto *action = add(paragraphToolbar_, entry.icon, text(entry.key), entry.name);
        action->setCheckable(true);
        action->setProperty("paragraphAlignment", static_cast<int>(entry.value));
        alignment_->addAction(action);
        connect(action, &QAction::triggered, this, [this, value = entry.value] {
            if (!editor_->isReadOnly()) editor_->setAlignment(value);
        });
    }
    paragraphToolbar_->addSeparator();
    auto *listButton = new QToolButton(paragraphToolbar_);
    listButton->setObjectName("listStyle");
    listButton->setIcon(makeIcon(Icon::List, palette()));
    listButton->setToolTip(text("ListStyle"));
    listButton->setPopupMode(QToolButton::InstantPopup);
    auto *listMenu = new QMenu(listButton);
    const std::pair<const char *, QTextListFormat::Style> styles[] = {
        {"ListDisc", QTextListFormat::ListDisc}, {"ListCircle", QTextListFormat::ListCircle},
        {"ListSquare", QTextListFormat::ListSquare}, {"ListDecimal", QTextListFormat::ListDecimal},
        {"ListLowerAlpha", QTextListFormat::ListLowerAlpha}, {"ListUpperAlpha", QTextListFormat::ListUpperAlpha},
        {"ListLowerRoman", QTextListFormat::ListLowerRoman},
        {"ListUpperRoman", QTextListFormat::ListUpperRoman}};
    for (const auto &entry : styles) {
        auto *action = listMenu->addAction(text(entry.first));
        connect(action, &QAction::triggered, this, [this, style = entry.second] { setList(style); });
    }
    listMenu->addSeparator();
    listMenu->addAction(text("RemoveList"), this, [this] { setList(0); });
    listButton->setMenu(listMenu);
    paragraphToolbar_->addWidget(listButton);
    auto *less = add(paragraphToolbar_, Icon::IndentLess, text("DecreaseIndent"), "decreaseIndent");
    connect(less, &QAction::triggered, this, [this] { indent(-1); });
    auto *more = add(paragraphToolbar_, Icon::IndentMore, text("IncreaseIndent"), "increaseIndent");
    connect(more, &QAction::triggered, this, [this] { indent(1); });
    paragraphToolbar_->addSeparator();
    auto *time = add(paragraphToolbar_, Icon::Time, text("InsertTime"), "insertTime");
    connect(time, &QAction::triggered, this, [this] { command(Command::InsertTime); });
    for (auto *bar : {toolbar_, paragraphToolbar_}) {
        for (auto *action : bar->actions()) {
            if (auto *button = qobject_cast<QToolButton *>(bar->widgetForAction(action))) {
                button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
                button->setFixedSize(buttonSize);
                button->setIconSize(iconSize);
            }
        }
    }
}


void Dock::createSearchField()
{
    searchInput_ = new QLineEdit(this);
    searchInput_->setObjectName("searchText");
    searchInput_->setPlaceholderText(text("FindPlaceholder") + " (" + QKeySequence(QKeySequence::Find).toString(QKeySequence::NativeText) + ")");
    searchInput_->setAccessibleName(text("Find"));
    searchInput_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    searchInput_->installEventFilter(this);
    auto *row = new QHBoxLayout(searchInput_);
    row->setContentsMargins(0, 0, 4, 0);
    row->setSpacing(2);
    row->addStretch(1);
    searchStatus_ = new QLabel(searchInput_);
    searchStatus_->setObjectName("searchStatus");
    searchStatus_->setTextFormat(Qt::PlainText);
    row->addWidget(searchStatus_);
    searchStatus_->hide();
    searchTimer_.setSingleShot(true);
    searchTimer_.setInterval(160);
    connect(&searchTimer_, &QTimer::timeout, this, [this] {
        if (searchFromStart_) findText(false, true);
        else { rebuildSearch(); updateSearchStatus(); }
    });
    find_ = new QAction(text("Find"), this);
    find_->setObjectName("findText");
    find_->setShortcut(QKeySequence::Find);
    find_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(find_);
    connect(find_, &QAction::triggered, this, [this] { showSearch(); });
    auto button = [this, row](const char *name, Icon icon, const QString &tip) {
        auto *value = new QToolButton(searchInput_);
        value->setObjectName(name);
        value->setFixedSize(20, 20);
        value->setIconSize(iconSize);
        value->setIcon(makeIcon(icon, palette()));
        value->setToolTip(tip);
        value->setAutoRaise(true);
        value->setStyleSheet("QToolButton { min-width: 0px; min-height: 0px; padding: 0px; margin: 0px; border: 0px; background: transparent; } QToolButton:hover { background: palette(button); }");
        row->addWidget(value);
        return value;
    };
    connect(button("findPrevious", Icon::Previous, text("FindPrevious")), &QToolButton::clicked, this, [this] { findText(true); });
    connect(button("findNext", Icon::Next, text("FindNext")), &QToolButton::clicked, this, [this] { findText(); });
    clearSearchButton_ = button("clearSearch", Icon::Close, text("CloseSearch"));
    connect(clearSearchButton_, &QToolButton::clicked, this, [this] { clearSearch(); });
    clearSearchButton_->hide();
    connect(searchInput_, &QLineEdit::textChanged, this, [this](const QString &value) {
        markSearch_ = false;
        clearSearchButton_->setVisible(!value.isEmpty());
        invalidateSearch(true);
    });
    connect(searchInput_, &QLineEdit::returnPressed, this, [this] { findText(); });
    updateSearchMargins();
}

void Dock::updateSearchMargins()
{
    const int inset = searchInput_->layout()->sizeHint().width();
    searchInput_->setTextMargins(0, 0, inset, 0);
    searchInput_->setMinimumWidth(searchInput_->fontMetrics().horizontalAdvance("MM") + inset + 8);
}

void Dock::showSearch()
{
    if (!editor_->isEnabled()) return;
    pauseScroll();
    const auto selected = editor_->textCursor().selectedText();
    if (!selected.isEmpty() && selected.size() <= 256 && !selected.contains(QChar::ParagraphSeparator))
        searchInput_->setText(selected);
    searchInput_->setFocus();
    searchInput_->selectAll();
}

void Dock::invalidateSearch(bool fromStart)
{
    static_cast<Editor *>(editor_)->setSearchMatch(false);
    searchDirty_ = true;
    searchFromStart_ = searchFromStart_ || fromStart;
    searchMatches_.clear();
    searchTimer_.stop();
    searchStatus_->hide();
    searchInput_->setAccessibleDescription({});
    searchInput_->setToolTip(text("FindPlaceholder"));
    updateSearchMargins();
    if (!searchInput_->text().isEmpty() && editor_->isEnabled()) searchTimer_.start();
}

void Dock::rebuildSearch()
{
    // Cache match positions until the query or document changes.
    searchTimer_.stop();
    if (!searchDirty_) return;
    searchMatches_.clear();
    const auto query = searchInput_->text();
    if (!query.isEmpty() && editor_->isEnabled()) {
        // Scan each paragraph once using Qt's case and space matching.
        for (auto block = editor_->document()->begin(); block.isValid(); block = block.next()) {
            auto plain = block.text();
            plain.replace(QChar::Nbsp, QLatin1Char(' '));
            qsizetype offset = 0;
            while ((offset = plain.indexOf(query, offset, Qt::CaseInsensitive)) >= 0) {
                const int start = block.position() + static_cast<int>(offset);
                searchMatches_.append({start, start + static_cast<int>(query.size())});
                offset += query.size();
            }
        }
    }
    searchDirty_ = false;
}

void Dock::updateSearchStatus()
{
    if (searchInput_->text().isEmpty() || !editor_->isEnabled()) {
        static_cast<Editor *>(editor_)->setSearchMatch(false);
        searchStatus_->hide();
        updateSearchMargins();
        return;
    }
    const auto cursor = editor_->textCursor();
    const auto match = std::lower_bound(searchMatches_.cbegin(), searchMatches_.cend(), cursor.selectionStart(),
        [](const auto &range, int position) { return range.first < position; });
    const int current = match != searchMatches_.cend() && match->first == cursor.selectionStart() &&
        match->second == cursor.selectionEnd() ? int(match - searchMatches_.cbegin()) + 1 : 0;
    static_cast<Editor *>(editor_)->setSearchMatch(current > 0);
    const auto count = QString("%1/%2").arg(current).arg(searchMatches_.size());
    if (searchStatus_->text() != count) searchStatus_->setText(count);
    const auto description = searchMatches_.isEmpty() ? text("NoMatch") :
        text("MatchCount").arg(current).arg(searchMatches_.size());
    searchStatus_->setToolTip(description);
    searchInput_->setAccessibleDescription(description);
    searchInput_->setToolTip(description);
    searchStatus_->show();
    updateSearchMargins();
}

void Dock::findText(bool backward, bool fromStart)
{
    const bool restart = fromStart || searchFromStart_;
    searchFromStart_ = false;
    rebuildSearch();
    if (!searchInput_->text().isEmpty() && editor_->isEnabled()) pauseScroll();
    if (!searchMatches_.isEmpty()) {
        const auto previous = editor_->textCursor();
        auto match = searchMatches_.cbegin();
        if (!restart && backward) {
            match = std::upper_bound(searchMatches_.cbegin(), searchMatches_.cend(), previous.selectionStart(),
                [](int position, const auto &range) { return position < range.second; });
            if (match == searchMatches_.cbegin()) match = searchMatches_.cend();
            --match;
        } else if (!restart) {
            match = std::lower_bound(searchMatches_.cbegin(), searchMatches_.cend(), previous.selectionEnd(),
                [](const auto &range, int position) { return range.first < position; });
            if (match == searchMatches_.cend()) match = searchMatches_.cbegin();
        }
        rememberReturnPosition();
        QTextCursor cursor(editor_->document());
        cursor.setPosition(match->first);
        cursor.setPosition(match->second, QTextCursor::KeepAnchor);
        editor_->setTextCursor(cursor);
        editor_->ensureCursorVisible();
    }
    updateSearchStatus();
}

void Dock::clearSearch()
{
    searchInput_->clear();
    auto cursor = editor_->textCursor();
    cursor.clearSelection();
    editor_->setTextCursor(cursor);
    editor_->setFocus();
}

void Dock::showEditorContextMenu(const QPoint &position)
{
    QMenu menu(editor_);
    menu.setObjectName("noteContextMenu");
    const bool editable = editor_->isEnabled() && !editor_->isReadOnly();
    const bool selected = editor_->textCursor().hasSelection();
    auto add = [&](const char *key, const QKeySequence &shortcut, bool enabled, auto operation) {
        auto label = text(key);
        if (!shortcut.isEmpty()) label += "\t" + shortcut.toString(QKeySequence::NativeText);
        auto *action = menu.addAction(label);
        action->setObjectName(QStringLiteral("ctx") + key);
        action->setEnabled(enabled);
        connect(action, &QAction::triggered, editor_, operation);
    };
    add("Undo", QKeySequence::Undo, editable && editor_->document()->isUndoAvailable(), [this] { editor_->undo(); });
    add("Redo", QKeySequence::Redo, editable && editor_->document()->isRedoAvailable(), [this] { editor_->redo(); });
    menu.addSeparator();
    add("Cut", QKeySequence::Cut, editable && selected, [this] { static_cast<Editor *>(editor_)->deleteSelection(true); });
    add("Copy", QKeySequence::Copy, selected, [this] { editor_->copy(); });
    add("Paste", QKeySequence::Paste, editable && editor_->canPaste(), [this] { editor_->paste(); });
    add("PastePlain", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), editable && !QApplication::clipboard()->text().isEmpty(), [this] {
        static_cast<Editor *>(editor_)->insertCheckedText(QApplication::clipboard()->text());
    });
    add("Delete", QKeySequence::Delete, editable && selected, [this] {
        static_cast<Editor *>(editor_)->deleteSelection();
    });
    add("SelectAll", QKeySequence::SelectAll, !editor_->document()->isEmpty(), [this] { editor_->selectAll(); });
    menu.addSeparator();
    const auto mark = markAt(editor_->cursorForPosition(position));
    if (!mark.isNull()) {
        add("DeleteMark", {}, editable, [this, mark] {
            auto cursor = mark; cursor.beginEditBlock(); cursor.removeSelectedText(); cursor.endEditBlock(); editor_->setTextCursor(cursor);
        });
        menu.addSeparator();
    }
    add("ConvertBasic", {}, editable && !editor_->document()->isEmpty(), [this] { convertToBasic(); });
    add("ClearBody", {}, editable && !editor_->document()->isEmpty(), [this] { replaceHtml({}, "ClearBody", "ClearBodyPrompt", false); });
    menu.addSeparator();
    add("BackupCurrent", {}, !sceneId_.isEmpty(), [this] { backup(false); });
    add("BackupAll", {}, true, [this] { backup(true); });
    add("RestoreBackup", {}, !locked_ && !remoteEditing_ && !sceneId_.isEmpty(), [this] { restoreBackup(); });
    menu.addSeparator();
    menu.addAction(text("RemoteAccess"), this, [this] { remote_->showSettings(); });
    menu.addAction(text("About"), this, &Dock::showAbout)->setObjectName("ctxAbout");
    menu.exec(editor_->mapToGlobal(position));
}

void Dock::showAbout()
{
    QDialog dialog(this);
    dialog.setObjectName("aboutDialog");
    dialog.setWindowTitle(text("About"));
    auto *layout = new QVBoxLayout(&dialog);
    for (const auto *key : {"AboutBody", "ShortcutBody"}) {
        auto body = text(key);
        if (qstrcmp(key, "AboutBody") == 0) body = body.arg(QString::fromUtf8(SCENEPAD_VERSION));
        auto *label = new QLabel(body, &dialog);
        label->setObjectName(QString::fromLatin1(key));
        label->setTextFormat(Qt::RichText);
        label->setTextInteractionFlags(Qt::TextBrowserInteraction);
        label->setOpenExternalLinks(true);
        layout->addWidget(label);
        auto *line = new QFrame(&dialog);
        line->setFixedHeight(1);
        line->setStyleSheet(palette().color(QPalette::Window).lightness() < 128 ?
            "background: #666666; border: 0;" : "background: #aaaaaa; border: 0;");
        layout->addWidget(line);
    }
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *days = new QSpinBox(&dialog);
    days->setObjectName("autoClearDays");
    days->setRange(0, MaxAutoClearDays);
    days->setSpecialValueText(text("AutoClearOff"));
    days->setSuffix(text("AutoClearDaySuffix"));
    days->setValue(autoClearDays_);
    form->addRow(text("AutoClear"), days);
    auto *scopeRow = new QHBoxLayout;
    auto *scope = new QComboBox(&dialog);
    scope->setObjectName("autoClearScope");
    scope->addItem(text("AutoClearSelected"), false);
    scope->addItem(text("AutoClearAll"), true);
    scope->setCurrentIndex(autoClearAll_ ? 1 : 0);
    scopeRow->addWidget(scope, 1);
    auto *choose = new QPushButton(&dialog);
    choose->setObjectName("autoClearChoose");
    scopeRow->addWidget(choose);
    form->addRow(text("AutoClearScope"), scopeRow);
    auto selected = autoClearSelection_;
    auto update = [&] {
        scope->setEnabled(days->value() > 0);
        choose->setEnabled(scope->isEnabled() && !scope->currentData().toBool());
        choose->setText(text("AutoClearChoose").arg(selected.size()));
    };
    connect(days, &QSpinBox::valueChanged, &dialog, update);
    connect(scope, &QComboBox::currentIndexChanged, &dialog, update);
    connect(choose, &QPushButton::clicked, &dialog, [&] {
        QDialog selection(&dialog);
        selection.setObjectName("autoClearScenesDialog");
        selection.setWindowTitle(text("AutoClearSelected"));
        auto *selectionLayout = new QVBoxLayout(&selection);
        auto *list = new QListWidget(&selection);
        list->setObjectName("autoClearScenes");
        list->setMinimumSize(320, 180);
        list->setSelectionMode(QAbstractItemView::NoSelection);
        selectionLayout->addWidget(list);
        // Display OBS order, but retain stable IDs across renames and collections.
        obs_frontend_source_list scenes{};
        obs_frontend_get_scenes(&scenes);
        for (size_t i = 0; i < scenes.sources.num; ++i) {
            auto *source = scenes.sources.array[i];
            const auto id = QString::fromUtf8(obs_source_get_uuid(source));
            auto *item = new QListWidgetItem(QString::fromUtf8(obs_source_get_name(source)), list);
            item->setData(Qt::UserRole, id);
            item->setCheckState(selected.contains(id) ? Qt::Checked : Qt::Unchecked);
        }
        obs_frontend_source_list_free(&scenes);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &selection);
        selectionLayout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &selection, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &selection, &QDialog::reject);
        if (selection.exec() != QDialog::Accepted) return;
        for (int i = 0; i < list->count(); ++i) {
            const auto *item = list->item(i);
            const auto id = item->data(Qt::UserRole).toString();
            if (item->checkState() == Qt::Checked) selected.insert(id);
            else selected.remove(id);
        }
        update();
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (days->value() > 0 && !scope->currentData().toBool() && selected.isEmpty()) {
            QMessageBox::information(&dialog, text("AutoClear"), text("AutoClearEmpty")); return;
        }
        if (autoClearDays_ == days->value() && autoClearAll_ == scope->currentData().toBool() && autoClearSelection_ == selected) {
            dialog.accept(); return;
        }
        preferences_.setValue("autoClear/days", days->value());
        preferences_.setValue("autoClear/all", scope->currentData().toBool());
        preferences_.setValue("autoClear/scenes", QStringList(selected.cbegin(), selected.cend()));
        preferences_.sync();
        if (preferences_.status() != QSettings::NoError) {
            QMessageBox::warning(&dialog, text("AutoClear"), text("AutoClearSaveFailed")); return;
        }
        autoClearDays_ = days->value();
        autoClearAll_ = scope->currentData().toBool();
        autoClearSelection_ = selected;
        scheduleAutoClear();
        dialog.accept();
    });
    update();
    dialog.exec();
}

void Dock::scheduleAutoClear()
{
    autoClearPending_.clear();
    autoClearTimer_.stop();
    if (autoClearReady_ && autoClearDays_ > 0 && (autoClearAll_ || !autoClearSelection_.isEmpty())) autoClearTimer_.start(5000);
}

void Dock::clearExpiredNotes()
{
    if (shuttingDown_ || !autoClearReady_ || autoClearDays_ == 0) return;
    if (restoring_ || QApplication::activeModalWidget()) { autoClearTimer_.start(60000); return; }
    if (autoClearPending_.isEmpty()) {
        obs_frontend_source_list scenes{};
        obs_frontend_get_scenes(&scenes);
        for (size_t i = 0; i < scenes.sources.num; ++i) autoClearPending_.append(OBSGetWeakRef(scenes.sources.array[i]));
        obs_frontend_source_list_free(&scenes);
    }
    // Process one inactive scene per turn; all saves remain serialized on the UI thread.
    if (!autoClearPending_.isEmpty()) {
        auto source = OBSGetStrongRef(autoClearPending_.takeFirst());
        if (source && !obs_source_removed(source)) {
            const auto id = QString::fromUtf8(obs_source_get_uuid(source));
            if ((autoClearAll_ || autoClearSelection_.contains(id)) && id != sceneId_ && (!remote_ || !remote_->sceneInUse(id))) {
                OBSSourceAutoRelease program = obs_frontend_get_current_scene();
                OBSSourceAutoRelease preview = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene() : nullptr;
                if (source != program && source != preview) {
                    QString error;
                    if (notes_.clearExpired(source, QDateTime::currentDateTimeUtc().addDays(-autoClearDays_), &error)) {
                        if (remote_) remote_->noteChanged(id);
                        if (notes_.takeReferenceChanges()) obs_frontend_save();
                    } else if (!error.isEmpty()) {
                        showSaveError(error);
                        showStatus(text("AutoClearFailed").arg(QString::fromUtf8(obs_source_get_name(source))));
                    }
                }
            }
        }
    }
    autoClearTimer_.start(autoClearPending_.isEmpty() ? 24 * 60 * 60 * 1000 : 25);
}

void Dock::updateSceneStyle()
{
    const auto sceneStyle = QString("font-weight: bold; color: %1;").arg(palette().color(locked_ ? QPalette::Highlight : QPalette::WindowText).name(QColor::HexArgb));
    if (sceneLabel_->styleSheet() != sceneStyle) sceneLabel_->setStyleSheet(sceneStyle);
}

void Dock::updateAppearance()
{
    if (!toolbar_) return;
    updateSceneStyle();
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    saveError_->setStyleSheet(dark ? "QLabel { color: #ffd2cc; background: #4b2525; border: 1px solid #b96359; border-radius: 3px; }" :
        "QLabel { color: #8c2520; background: #fff0ec; border: 1px solid #b96359; border-radius: 3px; }");
    const auto background = palette().color(QPalette::Window);
    const auto foreground = palette().color(QPalette::WindowText);
    const auto accent = palette().color(QPalette::Highlight);
    // OBS supplies ordinary selection colors; only explicit color previews override them.
    const auto preview = (selectionText_.isEmpty() ? QString() : "selection-color: " + selectionText_ + ";") +
        (selectionBackground_.isEmpty() ? QString() : "selection-background-color: " + selectionBackground_ + ";");
    const auto editorStyle = QString("QTextEdit { color: %2; %3 border: 1px solid palette(highlight); } QTextEdit[readOnly=\"true\"], QTextEdit:disabled { border-color: palette(mid); } QTextEdit[readOnly=\"true\"], QTextEdit:!focus { background-color: %1; } QTextEdit[searchMatch=\"true\"] { selection-background-color: #fff200; selection-color: #202126; }")
        .arg(background.darker(115).name(), foreground.name(), preview);
    if (editor_->styleSheet() != editorStyle) editor_->setStyleSheet(editorStyle);
    // Use a softened theme foreground for separators.
    auto separator = palette().color(QPalette::Active, QPalette::WindowText);
    separator.setAlpha(170);
    const auto color = QString("rgba(%1, %2, %3, %4)")
        .arg(separator.red()).arg(separator.green()).arg(separator.blue()).arg(separator.alpha());
    const QColor line((foreground.red() + 3 * background.red()) / 4,
        (foreground.green() + 3 * background.green()) / 4, (foreground.blue() + 3 * background.blue()) / 4);
    const auto sliderStyle = QString("QSlider::groove:horizontal { height: 4px; border-radius: 2px; background: %1; } QSlider::sub-page:horizontal { border-radius: 2px; background: %2; } QSlider::handle:horizontal { width: 14px; margin: -3px 0; border-radius: 5px; background: %2; } QSlider::handle:horizontal:hover { background: %3; }")
        .arg(line.name(), dark ? "#d8dce3" : "#606975", accent.name());
    if (speedSlider_ && speedSlider_->styleSheet() != sliderStyle) speedSlider_->setStyleSheet(sliderStyle);
    // OBS themes cap toolbar items; keep CSS limits equal to the widget dimensions.
    const auto buttonStyle = QString("QToolButton { min-width: %1px; max-width: %1px; min-height: %2px; max-height: %2px; icon-size: %3px; padding: 0px; margin: 0px; border: 0px; border-radius: 3px; background: transparent; } QToolButton:checked { background: %4; } QToolButton#readMode:checked { background: transparent; } QToolButton:enabled:hover, QToolButton#readMode:enabled:hover { background: %5; } QToolButton::menu-indicator { image: none; width: 0px; } QToolButton#qt_toolbar_ext_button { min-width: 16px; max-width: 16px; }")
        .arg(buttonSize.width()).arg(buttonSize.height()).arg(iconSize.width())
        .arg(palette().color(QPalette::Mid).name(), palette().color(QPalette::Button).lighter(120).name());
    const auto toolbarStyle = QString("QToolBar { border: 0px; padding: 0px; margin: 0px; spacing: 2px; } QToolBar::separator { width: 1px; background: %1; margin: 6px 4px; }").arg(line.name()) + buttonStyle;
    for (const auto *name : {"readMode", "scrollPlay", "scrollTop", "readingGuide"})
        if (auto *button = findChild<QToolButton *>(name); button && button->styleSheet() != buttonStyle) button->setStyleSheet(buttonStyle);
    for (auto *bar : {toolbar_, paragraphToolbar_}) {
        if (!bar) continue;
        if (bar->styleSheet() != toolbarStyle) bar->setStyleSheet(toolbarStyle);
        for (auto *action : bar->actions()) {
            const auto icon = action->property("scenePadIcon");
            if (icon.isValid()) action->setIcon(makeIcon(static_cast<Icon>(icon.toInt()), palette()));
        }
        if (auto *extension = bar->findChild<QToolButton *>("qt_toolbar_ext_button")) {
            extension->setIcon(makeIcon(Icon::Overflow, palette()));
            extension->setToolTip(text("MoreTools"));
        }
    }
    const auto lineStyle = QString("QFrame { border: 0px; background: %1; }").arg(line.name());
    for (const auto *name : {"speedStartSeparator", "speedEndSeparator", "characterSeparator", "percentSeparator"})
        if (auto *frame = findChild<QFrame *>(name))
            if (frame->styleSheet() != lineStyle) frame->setStyleSheet(lineStyle);
    for (auto *label : {status_, findChild<QLabel *>("scenePrefix"), documentStatus_, characters_, scrollPercent_}) {
        if (!label) continue;
        QPalette separatorPalette;
        separatorPalette.setColor(QPalette::WindowText, separator);
        label->setPalette(separatorPalette);
    }
    if (searchInput_) {
        const auto searchStyle = QString("QLineEdit { min-height: 0px; padding: 2px 6px; placeholder-text-color: %1; } QLineEdit QLabel { background: transparent; border: 0px; padding: 0px; color: %1; }").arg(color);
        if (searchInput_->styleSheet() != searchStyle) searchInput_->setStyleSheet(searchStyle);
    }
    if (playButton_) playButton_->setIcon(makeIcon(isScrolling() ? Icon::Pause : Icon::Play, palette()));
    if (auto *top = findChild<QToolButton *>("scrollTop")) top->setIcon(makeIcon(Icon::Top, palette()));
    if (auto *list = findChild<QToolButton *>("listStyle")) list->setIcon(makeIcon(Icon::List, palette()));
    const std::pair<const char *, Icon> searchIcons[] = {
        {"findPrevious", Icon::Previous}, {"findNext", Icon::Next}, {"clearSearch", Icon::Close}};
    for (const auto &entry : searchIcons)
        if (auto *button = findChild<QToolButton *>(entry.first)) button->setIcon(makeIcon(entry.second, palette()));
    if (lock_) lock_->setIcon(makeIcon(Icon::Read, palette()));
    if (qrButton_) qrButton_->setIcon(makeIcon(Icon::QR, palette()));
    if (returnButton_) returnButton_->setIcon(makeIcon(Icon::Return, palette()));
    if (readingGuide_) readingGuide_->setIcon(makeIcon(Icon::ReadingGuide, palette()));
    if (readingBand_) readingBand_->update();
}

void Dock::updateReadingBand()
{
    if (!readingBand_ || !readingGuide_) return;
    auto *viewport = editor_->viewport();
    if (!isScrolling() || !readingGuide_->isChecked() || viewport->height() < 1) {
        readingBand_->hide();
        return;
    }
    // Center the guide at the upper golden-section point.
    const int center = qRound(viewport->height() * 0.382);
    const auto cursor = editor_->cursorForPosition(QPoint(viewport->width() / 2, center));
    const int height = qMin(viewport->height(), qMax(24, editor_->cursorRect(cursor).height() + 8));
    const int top = qBound(0, center - height / 2, viewport->height() - height);
    readingBand_->setGeometry(0, top, viewport->width(), height);
    readingBand_->raise();
    readingBand_->show();
}

void Dock::updateControlSizes()
{
    if (!fontSize_ || !speedBox_ || !controls_) return;
    auto fit = [this](QAbstractSpinBox *box, const QString &sample, int minimumWidth, int padding) {
        box->setAttribute(Qt::WA_LayoutUsesWidgetRect);
        box->ensurePolished();
        box->setFont(font());
        const auto metrics = box->fontMetrics();
        box->setFixedSize(qMax(minimumWidth, metrics.horizontalAdvance(sample) + padding), qMax(28, metrics.height() + 10));
    };
    fit(fontSize_, "144", 44, 16);
    fit(speedBox_, "5.0×", 52, 16);
    searchInput_->setFixedHeight(fontSize_->height());
    updateSearchMargins();
    readingControls_->setFixedHeight(qMax(paragraphToolbar_->sizeHint().height(), speedBox_->height()));
    paragraphToolbar_->setFixedWidth(paragraphToolbar_->sizeHint().width());
    toolbar_->setFixedWidth(toolbar_->sizeHint().width());
    scrollPercent_->setFont(font());
    scrollPercent_->setFixedWidth(scrollPercent_->fontMetrics().horizontalAdvance("100%"));
    updateControlLayout();
    updateSideControls();
}

void Dock::updateControlLayout()
{
    if (!readingControls_ || !metrics_) return;
    // Move statistics below the controls only when their minimum widths no longer fit.
    auto *layout = static_cast<QBoxLayout *>(controls_->layout());
    const bool stacked = controls_->width() < readingControls_->minimumSizeHint().width() + metrics_->sizeHint().width() + layout->spacing();
    const auto direction = stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (layout->direction() != direction) layout->setDirection(direction);
    controls_->setFixedHeight(stacked ? readingControls_->height() + metrics_->sizeHint().height() + layout->spacing() : readingControls_->height());
}

void Dock::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange ||
        event->type() == QEvent::StyleChange) updateAppearance();
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::StyleChange) updateControlSizes();
}

void Dock::updateEditorActions()
{
    if (!editor_->isEnabled() || editor_->isReadOnly()) setFormatPainter(false);
    find_->setEnabled(editor_->isEnabled());
    searchInput_->setEnabled(editor_->isEnabled());
    toolbar_->setEnabled(editor_->isEnabled());
    paragraphToolbar_->setEnabled(editor_->isEnabled() && !editor_->isReadOnly());
    for (auto *action : toolbar_->actions()) {
        const auto name = action->objectName();
        action->setEnabled(!editor_->isReadOnly() || name == "exportDocument" || name == "nextMark" ||
            (name == "importDocument" && noteLoadFailed_ && !locked_ && !remoteEditing_));
    }
}

double Dock::scrollProgress() const
{
    const auto *bar = editor_->verticalScrollBar();
    const int range = bar->maximum() - bar->minimum();
    return range > 0 ? static_cast<double>(bar->value() - bar->minimum()) / range : 0.0;
}

bool Dock::goToPosition(QString target)
{
    if (shuttingDown_ || !editor_->isEnabled()) return false;
    target = target.trimmed();
    if (target.size() > 10) return false;
    const bool percentage = target.endsWith('%');
    if (percentage) target.chop(1);
    bool valid = false;
    const int value = target.toInt(&valid);
    if (!valid || value < (percentage ? 0 : 1) || value > (percentage ? 100 : editor_->document()->blockCount())) return false;
    rememberReturnPosition();
    pauseScroll();
    auto *bar = editor_->verticalScrollBar();
    // Use Qt's block index and scroll range without copying the document.
    if (percentage) bar->setValue(static_cast<int>(static_cast<qint64>(bar->maximum()) * value / 100));
    else {
        editor_->setTextCursor(QTextCursor(editor_->document()->findBlockByNumber(value - 1)));
        editor_->ensureCursorVisible();
        bar->setValue(bar->value() + editor_->cursorRect().top() - qRound(editor_->viewport()->height() * 0.382));
    }
    return true;
}

void Dock::showGoTo(bool percentage)
{
    if (shuttingDown_ || !editor_->isEnabled()) return;
    if (!goToInput_) {
        goToInput_ = new QLineEdit(this);
        goToInput_->setObjectName("goToInput");
        goToInput_->setMaxLength(10);
        goToInput_->setAccessibleName(text("GoToPrompt"));
        goToInput_->setPlaceholderText(text("GoToPrompt"));
        goToInput_->installEventFilter(this);
        connect(goToInput_, &QLineEdit::returnPressed, this, [this] {
            if (goToPosition(goToInput_->text())) { goToInput_->hide(); editor_->setFocus(); }
            else {
                goToInput_->setProperty("invalid", true);
                goToInput_->setStyleSheet(goToInput_->styleSheet());
                goToInput_->setToolTip(text("GoToInvalid").arg(editor_->document()->blockCount()));
                goToInput_->selectAll();
            }
        });
    }
    goToInput_->setFont(font());
    goToInput_->setAlignment(percentage ? Qt::AlignRight : Qt::AlignLeft);
    goToInput_->setProperty("invalid", false);
    goToInput_->setToolTip(text("GoToPrompt"));
    goToInput_->setStyleSheet(inlineInputStyle);
    // Overlay the statistics without changing toolbar geometry.
    const auto corner = metrics_->mapTo(this, QPoint(0, metrics_->height() / 2));
    const int width = metrics_->width();
    const int height = qMax(24, fontMetrics().height() + 6);
    goToInput_->setGeometry(qBound(0, corner.x(), this->width() - width),
        qBound(0, corner.y() - height / 2, this->height() - height), width, height);
    const auto current = percentage ? QString::number(qRound(scrollProgress() * 100)) + '%' : QString::number(editor_->textCursor().blockNumber() + 1);
    goToInput_->setText(current);
    goToInput_->show(); goToInput_->raise(); goToInput_->setFocus();
    goToInput_->selectAll();
}

void Dock::updateMarkFormat()
{
    auto cursor = editor_->textCursor();
    if (editor_->isReadOnly() || cursor.hasSelection()) return;
    auto mark = markAt(cursor);
    if (mark.isNull() || mark.selectedText().size() < 3) return;
    auto format = editor_->currentCharFormat();
    // Keep note text orange and restore the surrounding color at either boundary.
    if (cursor.position() > mark.selectionStart() && cursor.position() < mark.selectionEnd())
        format.setForeground(QColor(palette().color(QPalette::Window).lightness() < 128 ? "#ffb347" : "#b85b00"));
    else {
        auto adjacent = mark;
        if (mark.selectionEnd() < mark.block().position() + mark.block().length() - 1) {
            adjacent.setPosition(mark.selectionEnd()); adjacent.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        } else if (mark.selectionStart() > mark.block().position()) {
            adjacent.setPosition(mark.selectionStart() - 1); adjacent.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        } else { format.clearForeground(); editor_->setCurrentCharFormat(format); return; }
        if (adjacent.charFormat().hasProperty(QTextFormat::ForegroundBrush)) format.setForeground(adjacent.charFormat().foreground());
        else format.clearForeground();
    }
    if (format != editor_->currentCharFormat()) editor_->setCurrentCharFormat(format);
}

void Dock::updateDocumentStatus()
{
    updateCharacterStatus();
    const auto value = text("DocumentStatus").arg(editor_->isEnabled() ? editor_->document()->blockCount() : 0);
    if (documentStatus_->text() != value) documentStatus_->setText(value);
    const auto progress = QString::number(qRound(100.0 * scrollProgress())) + '%';
    if (scrollPercent_->text() != progress) scrollPercent_->setText(progress);
    if (remote_) remote_->scrollChanged();
}

void Dock::updateCharacterStatus()
{
    const int used = editor_->isEnabled() ? textLength() : 0;
    const auto cursor = editor_->textCursor();
    // Cursor offsets avoid copying selected text, even for the entire script.
    const int selected = cursor.hasSelection() ? cursor.selectionEnd() - cursor.selectionStart() -
        (editor_->document()->findBlock(cursor.selectionEnd()).blockNumber() - editor_->document()->findBlock(cursor.selectionStart()).blockNumber()) : 0;
    const auto value = text(cursor.hasSelection() ? "SelectedCharacters" : "CharacterCount").arg(QLocale().toString(cursor.hasSelection() ? selected : used));
    if (characters_->text() != value) characters_->setText(value);
    const auto details = text("CharacterRemaining").arg(QLocale().toString(used), QLocale().toString(maxNoteCharacters),
        QLocale().toString(qMax(0, maxNoteCharacters - used)));
    characters_->setToolTip(details);
    characters_->setAccessibleName(cursor.hasSelection() ? value + "; " + details : details);
}

void Dock::refreshScene()
{
    if (shuttingDown_) return;
    const bool preview = obs_frontend_preview_program_mode_active();
    OBSSourceAutoRelease scene = preview ? obs_frontend_get_current_preview_scene() : obs_frontend_get_current_scene();
    bindScene(scene);
}

QString Dock::readTextFile(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 24 * 1024 * 1024) {
        *error = text("FileReadFailed") + path;
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

void Dock::bindScene(obs_source_t *scene)
{
    const auto id = scene ? QString::fromUtf8(obs_source_get_uuid(scene)) : QString();
    if (id == sceneId_ && !id.isEmpty() && !noteLoadFailed_) {
        QString error;
        if (!dirty_ && !notes_.rename(scene, &error)) showStatus(error);
        sceneLabel_->setText(QString::fromUtf8(obs_source_get_name(scene)));
        sceneLabel_->setToolTip(sceneLabel_->text());
        flush();
        return;
    }
    if (!flush()) return;
    clearReturnPosition();
    setFormatPainter(false);
    if (goToInput_) goToInput_->hide();
    rememberPosition();
    pauseScroll();
    scene_ = scene ? OBSGetWeakRef(scene) : OBSWeakSource();
    sceneId_ = id;
    QString error;
    notes_.rename(scene, &error);
    const auto note = error.isEmpty() ? notes_.read(scene, &error) : Note{};
    if (error.isEmpty() && !withinHtmlLimits(note.html)) error = text("TextLimit");
    noteLoadFailed_ = !error.isEmpty();
    editor_->setPlaceholderText(noteLoadFailed_ ? error : QString());
    {
        const QSignalBlocker block(editor_);
        static_cast<Editor *>(editor_)->loadHtml(noteLoadFailed_ ? QString() : note.html);
        editor_->setReadOnly(locked_ || remoteEditing_ || noteLoadFailed_);
        editor_->setEnabled(scene != nullptr);
        editor_->verticalScrollBar()->setValue(qMax(0, positions_.value(id,
            preferences_.value("readingPositions/" + id, 0).toInt())));
    }
    {
        const QSignalBlocker blockLock(lock_);
        lock_->setChecked(locked_ || remoteEditing_);
    }
    lock_->setEnabled(true);
    updateEditorActions();
    playButton_->setEnabled(scene != nullptr);
    readingGuide_->setEnabled(scene != nullptr);
    sceneLabel_->setText(scene ? QString::fromUtf8(obs_source_get_name(scene)) : text("NoScene"));
    sceneLabel_->setToolTip(sceneLabel_->text());
    dirty_ = false;
    saveError_->hide();
    saveTimer_.stop();
    syncFormat();
    showStatus(noteLoadFailed_ ? error : scene ? text("Loaded") : text("SelectScene"));
    updateDocumentStatus();
    searchFromStart_ = false;
    invalidateSearch();
    if (remote_) remote_->stateChanged();
    flush();
}

void Dock::sceneRenamed(obs_source_t *scene)
{
    if (shuttingDown_ || restoring_) return;
    if (QString::fromUtf8(obs_source_get_uuid(scene)) == sceneId_) { bindScene(scene); return; }
    QString error;
    if (!notes_.rename(scene, &error)) showStatus(error);
    flush();
}

bool Dock::flush(bool saveSceneReferences, bool notify)
{
    saveTimer_.stop();
    if (dirty_) {
        if (!withinTextLimits(*editor_->document())) { static_cast<Editor *>(editor_)->limitReached(); return false; }
        auto scene = OBSGetStrongRef(scene_);
        QString error;
        if (!scene || !notes_.write(scene, {editor_->toHtml()}, &error)) {
            if (notify) { showSaveError(error); showStatus(text("NoteSaveFailed") + " " + error); }
            return false;
        }
        dirty_ = false;
        saveError_->hide();
        if (notify) showStatus(text("SavedNote"));
    }
    // Only new or renamed references require a scene-collection save.
    if (saveSceneReferences && !shuttingDown_ && notes_.takeReferenceChanges()) obs_frontend_save();
    return true;
}

QString Dock::readHtml(obs_source_t *scene, QString *error) const
{
    // Readers see pending edits without forcing an autosave or scene save.
    if (scene && dirty_ && QString::fromUtf8(obs_source_get_uuid(scene)) == sceneId_) return editor_->toHtml();
    return notes_.read(scene, error).html;
}

int Dock::textLength() const { return editor_->document()->characterCount() - 1; }

void Dock::frontendEvent(obs_frontend_event event)
{
    if (restoring_) { refreshAfterRestore_ = true; return; }
    switch (event) {
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
        autoClearReady_ = false;
        scheduleAutoClear();
        if (remote_) remote_->setCollectionChanging(true);
        flush(false);
        bindScene(nullptr);
        break;
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
    case OBS_FRONTEND_EVENT_SCENE_CHANGED:
    case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
    case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
    case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
    case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
        if (remote_ && (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED))
            remote_->setCollectionChanging(false);
        refreshScene();
        if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
            autoClearReady_ = true;
            scheduleAutoClear();
        }
        if (remote_) remote_->stateChanged();
        break;
    default: break;
    }
}

void Dock::applyFormat(const QTextCharFormat &format)
{
    if (editor_->isReadOnly() || !editor_->isEnabled()) return;
    setFormatPainter(false);
    editor_->mergeCurrentCharFormat(format);
    // Keep applied colors visible while the same range stays selected.
    if (editor_->textCursor().hasSelection() &&
        (format.hasProperty(QTextFormat::ForegroundBrush) || format.hasProperty(QTextFormat::BackgroundBrush))) {
        if (format.hasProperty(QTextFormat::ForegroundBrush)) selectionText_ = format.foreground().style() == Qt::NoBrush ? QString() : format.foreground().color().name();
        if (format.hasProperty(QTextFormat::BackgroundBrush)) selectionBackground_ = format.background().style() == Qt::NoBrush ? QString() : format.background().color().name();
        updateAppearance();
    }
    editor_->setFocus(Qt::OtherFocusReason);
    syncFormat();
}

void Dock::clearColorPreview()
{
    if (selectionText_.isEmpty() && selectionBackground_.isEmpty()) return;
    selectionText_.clear(); selectionBackground_.clear();
    updateAppearance();
}

void Dock::setFormatPainter(bool active, bool continuous)
{
    active = active && editor_->isEnabled() && !editor_->isReadOnly();
    paintContinuous_ = active && continuous;
    if (!active && paintFormat_.isEmpty()) return;
    if (active) {
        if (paintFormat_.isEmpty()) {
            const auto source = editor_->currentCharFormat();
            paintFormat_.setFont(editor_->currentFont());
            paintFormat_.setForeground(source.foreground());
            paintFormat_.setBackground(source.background());
        }
        editor_->viewport()->setCursor(Qt::CrossCursor);
        editor_->setFocus(Qt::OtherFocusReason);
    } else {
        paintFormat_ = QTextCharFormat();
        editor_->viewport()->unsetCursor();
    }
    paint_->setChecked(active);
    paint_->setToolTip(text(paintContinuous_ ? "FormatPainterContinuous" : "FormatPainter"));
}

void Dock::paintSelection()
{
    if (!paint_->isChecked() || !editor_->textCursor().hasSelection()) return;
    const auto format = paintFormat_;
    const bool continuous = paintContinuous_;
    applyFormat(format);
    if (continuous) {
        // Formatting emits textChanged; retain the original source for the next target.
        paintFormat_ = format;
        setFormatPainter(true, true);
    }
}

void Dock::syncFormat()
{
    const QSignalBlocker b(bold_), i(italic_), u(underline_), k(strike_), s(fontSize_);
    const auto format = editor_->currentCharFormat();
    bold_->setChecked(format.fontWeight() >= QFont::Bold);
    italic_->setChecked(format.fontItalic());
    underline_->setChecked(format.fontUnderline());
    strike_->setChecked(format.fontStrikeOut());
    auto size = format.fontPointSize();
    if (size <= 0) size = editor_->font().pointSizeF();
    fontSize_->setValue(qBound(6, qRound(size), 144));
    for (auto *action : alignment_->actions()) {
        action->setChecked((static_cast<int>(editor_->alignment()) & action->property("paragraphAlignment").toInt()) != 0);
    }
}

void Dock::setLocked(bool locked)
{
    // One desktop lock controls editing on every connected device.
    flush();
    locked_ = locked;
    preferences_.setValue("locked", locked);
    if (remote_) remote_->lockChanged();
    { const QSignalBlocker block(lock_); lock_->setChecked(locked_ || remoteEditing_); }
    if (!locked) pauseScroll();
    editor_->setReadOnly(locked || remoteEditing_ || noteLoadFailed_);
    updateSceneStyle();
    updateEditorActions();
}

void Dock::setRemoteEditing(bool editing)
{
    if (editing == remoteEditing_) return;
    flush(false);
    pauseScroll();
    remoteEditing_ = editing;
    editor_->setReadOnly(editing || locked_ || noteLoadFailed_);
    const QSignalBlocker block(lock_);
    lock_->setChecked(locked_ || editing);
    lock_->setToolTip(text("ReadTip"));
    editor_->setToolTip(editing ? text("RemoteEditing") : QString());
    updateEditorActions();
}

void Dock::setRemoteAvailable(bool available)
{
    qrButton_->setVisible(available);
    updateSideControls();
}

bool Dock::applyRemoteHtml(obs_source_t *source, const QString &html)
{
    if (restoring_) return false;
    QString error;
    if (!notes_.write(source, {html}, &error)) { showSaveError(error); showStatus(error); return false; }
    if (QString::fromUtf8(obs_source_get_uuid(source)) == sceneId_) {
        const int position = editor_->verticalScrollBar()->value();
        clearReturnPosition();
        const QSignalBlocker block(editor_);
        static_cast<Editor *>(editor_)->loadHtml(html);
        editor_->verticalScrollBar()->setValue(position);
        dirty_ = false;
        saveError_->hide();
        noteLoadFailed_ = false;
        editor_->setPlaceholderText({});
        editor_->setReadOnly(locked_ || remoteEditing_);
        invalidateSearch();
        updateDocumentStatus();
    }
    flush();
    return true;
}

void Dock::setList(int style)
{
    if (editor_->isReadOnly()) return;
    auto cursor = editor_->textCursor();
    cursor.beginEditBlock();
    if (!style) {
        const int start = cursor.selectionStart();
        const int end = cursor.hasSelection() ? cursor.selectionEnd() - 1 : start;
        for (auto block = editor_->document()->findBlock(start); block.isValid() && block.position() <= end;
             block = block.next()) {
            if (auto *list = block.textList()) list->remove(block);
        }
    } else {
        QTextListFormat format;
        format.setStyle(static_cast<QTextListFormat::Style>(style));
        format.setIndent(cursor.currentList() ? cursor.currentList()->format().indent() : 1);
        cursor.createList(format);
    }
    cursor.endEditBlock();
    editor_->setFocus();
}

void Dock::indent(int delta)
{
    if (editor_->isReadOnly()) return;
    auto cursor = editor_->textCursor();
    cursor.beginEditBlock();
    if (auto *list = cursor.currentList()) {
        auto format = list->format();
        if (format.indent() + delta <= 0) list->remove(cursor.block());
        else { format.setIndent(qBound(1, format.indent() + delta, 12)); cursor.createList(format); }
    } else {
        auto format = cursor.blockFormat();
        format.setIndent(qBound(0, format.indent() + delta, 12));
        cursor.mergeBlockFormat(format);
    }
    cursor.endEditBlock();
}

void Dock::clearFormat()
{
    if (editor_->isReadOnly()) return;
    auto cursor = editor_->textCursor();
    if (!cursor.hasSelection()) cursor.select(QTextCursor::Document);
    cursor.beginEditBlock();
    cursor.setCharFormat(QTextCharFormat());
    const int end = cursor.selectionEnd() - 1;
    QTextBlockFormat defaults;
    defaults.setLineHeight(140, QTextBlockFormat::ProportionalHeight);
    for (auto block = editor_->document()->findBlock(cursor.selectionStart());
         block.isValid() && block.position() <= end; block = block.next()) {
        if (auto *list = block.textList()) list->remove(block);
        QTextCursor(block).setBlockFormat(defaults);
    }
    cursor.endEditBlock();
    clearColorPreview();
    syncFormat();
}

void Dock::convertToBasic()
{
    if (!editor_->isEnabled() || editor_->isReadOnly() || editor_->document()->isEmpty()) return;
    QString html;
    if (!basicHtml(*editor_->document(), html)) {
        QMessageBox::warning(this, text("ConvertBasic"), text("ConvertFailed"));
        return;
    }
    if (replaceHtml(html, "ConvertBasic", "ConvertPrompt")) showStatus(text("ConvertedBasic"));
}

bool Dock::replaceHtml(const QString &html, const char *title, const char *prompt, bool notify)
{
    if (!editor_->isEnabled() || locked_ || remoteEditing_) return false;
    if (!withinHtmlLimits(html)) { static_cast<Editor *>(editor_)->limitReached(); return false; }
    const auto id = sceneId_;
    const int revision = editor_->document()->revision();
    if ((!editor_->document()->isEmpty() || noteLoadFailed_) && QMessageBox::question(this, text(title),
        text(prompt), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return false;
    // A modal confirmation can outlive a remote edit or scene change.
    if (locked_ || remoteEditing_ || sceneId_ != id || editor_->document()->revision() != revision) return false;
    const auto previousStatus = status_->toolTip();
    const bool wasDirty = dirty_;
    pauseScroll();
    auto cursor = editor_->textCursor();
    cursor.select(QTextCursor::Document);
    cursor.beginEditBlock();
    if (html.isEmpty()) cursor.removeSelectedText();
    else cursor.insertHtml(html);
    cursor.endEditBlock();
    noteLoadFailed_ = false;
    editor_->setPlaceholderText({});
    editor_->setReadOnly(false);
    updateEditorActions();
    editor_->setTextCursor(cursor);
    dirty_ = true;
    const bool saved = flush(true, notify);
    if (!notify) {
        // Quiet replacements retain their text when saving fails.
        if (!saved) { editor_->undo(); saveTimer_.stop(); dirty_ = wasDirty; }
        showStatus(previousStatus);
    }
    return saved;
}

void Dock::importFile()
{
    if (!editor_->isEnabled() || locked_ || remoteEditing_) return;
    const auto path = QFileDialog::getOpenFileName(this, text("ImportTitle"),
        {}, text("ImportFilter"));
    if (path.isEmpty()) return;
    if (path.endsWith(".docx", Qt::CaseInsensitive)) {
        const auto result = importDocx(path);
        if (!result.error.isEmpty()) { QMessageBox::warning(this, text("WordImportFailed"), result.error); return; }
        if (replaceHtml(result.html) && result.simplified)
            showStatus(text("WordSimplified"));
    } else {
        QString error;
        auto html = readTextFile(path, &error);
        if (!error.isEmpty()) { QMessageBox::warning(this, text("ImportFailed"), error); return; }
        if (path.endsWith(".txt", Qt::CaseInsensitive)) {
            QTextDocument document;
            document.setLayoutEnabled(false);
            document.setUndoRedoEnabled(false);
            document.setPlainText(html);
            if (!withinTextLimits(document)) { static_cast<Editor *>(editor_)->limitReached(); return; }
            html = document.toHtml();
        }
        replaceHtml(html);
    }
}

void Dock::exportDocument()
{
    if (!editor_->isEnabled()) return;
    QString filter;
    auto path = QFileDialog::getSaveFileName(this, text("ExportTitle"), "ScenePad", text("ExportFilter"), &filter);
    if (path.isEmpty()) return;
    const bool plain = path.endsWith(".txt", Qt::CaseInsensitive) ||
        (filter.contains("*.txt") && !path.endsWith(".html", Qt::CaseInsensitive) && !path.endsWith(".htm", Qt::CaseInsensitive));
    if (!path.endsWith(".txt", Qt::CaseInsensitive) && !path.endsWith(".html", Qt::CaseInsensitive) && !path.endsWith(".htm", Qt::CaseInsensitive))
        path += plain ? ".txt" : ".html";
    QSaveFile file(path);
    auto scene = OBSGetStrongRef(scene_);
    QString error;
    const auto content = noteLoadFailed_ ? notes_.read(scene, &error).html : plain ? editor_->toPlainText().remove(QChar::ObjectReplacementCharacter) : editor_->toHtml();
    if (!error.isEmpty()) { showStatus(error); return; }
    const auto bytes = (plain && noteLoadFailed_ ? plainText(content) : content).toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        QMessageBox::warning(this, text("ExportFailed"), file.errorString());
    else showStatus(text("Exported"));
}

void Dock::backup(bool all)
{
    if (!flush()) return;
    QStringList files;
    QString name = "OBS-ScenePad_Backup";
    QString error;
    if (all) {
        const QDir dir(notes_.directory());
        for (const auto &file : dir.entryList({"*.json"}, QDir::Files, QDir::Name)) files.append(dir.filePath(file));
    } else {
        auto scene = OBSGetStrongRef(scene_);
        if (!scene) return;
        if (notes_.filePath(scene).isEmpty() && !notes_.write(scene, {}, &error)) { showStatus(error); return; }
        files.append(notes_.filePath(scene));
        name = noteName(QString::fromUtf8(obs_source_get_name(scene)));
        flush();
    }
    if (files.isEmpty()) { showStatus(text("BackupEmpty")); return; }
    const auto suffix = all ? ".zip" : ".json";
    auto path = QFileDialog::getSaveFileName(this, text(all ? "BackupAll" : "BackupCurrent"),
        name + QDateTime::currentDateTime().toString("_yyyyMMdd_HHmmss") + suffix, all ? "ZIP (*.zip)" : "JSON (*.json)");
    if (path.isEmpty()) return;
    if (!path.endsWith(suffix, Qt::CaseInsensitive)) path += suffix;
    if (QFileInfo(path).dir().canonicalPath() == QDir(notes_.directory()).canonicalPath()) {
        QMessageBox::warning(this, text("BackupFailed"), text("BackupChooseFolder")); return;
    }
    bool success = false;
    fileJob(this, text("BackingUp"), [&] { success = backupNotes(path, files, error); });
    if (!success) QMessageBox::warning(this, text("BackupFailed"), error);
    else showStatus(text("BackupSaved"));
}

void Dock::restoreBackup()
{
    if (locked_ || remoteEditing_ || restoring_ || !flush()) return;
    auto current = OBSGetStrongRef(scene_);
    if (!current) return;
    const auto path = QFileDialog::getOpenFileName(this, text("RestoreBackup"), {}, "ScenePad (*.json *.zip)");
    if (path.isEmpty()) return;
    QTemporaryDir staging;
    if (!staging.isValid()) { showStatus(text("RestoreFailed")); return; }
    QString error;
    QList<BackupNote> entries;
    fileJob(this, text("CheckingBackup"), [&] { entries = inspectBackup(path, staging.path(), error); });
    if (!error.isEmpty()) { QMessageBox::warning(this, text("RestoreFailed"), error); return; }
    if (shuttingDown_ || locked_ || remoteEditing_ || !flush()) return;
    struct Restore { OBSSource source; NoteTarget target; QString staged; };
    QList<Restore> plan;
    QSet<QString> reserved;
    int skipped = 0, replaced = 0;
    for (const auto &entry : entries) {
        OBSSourceAutoRelease scene = path.endsWith(".json", Qt::CaseInsensitive) ?
            obs_source_get_ref(current) : obs_get_source_by_uuid(entry.sceneId.toUtf8().constData());
        if (!scene || obs_source_removed(scene) || !obs_scene_from_source(scene) || obs_scene_is_group(obs_scene_from_source(scene))) {
            ++skipped; continue;
        }
        const auto target = notes_.prepare(scene, reserved);
        reserved.insert(QFileInfo(target.path).fileName().toCaseFolded());
        if (QFileInfo::exists(target.path)) ++replaced;
        plan.push_back({OBSSource(scene), target, entry.path});
    }
    if (plan.isEmpty()) { QMessageBox::information(this, text("RestoreBackup"), text("RestoreNoMatch")); return; }
    if (QMessageBox::question(this, text("RestoreBackup"), text("RestorePrompt").arg(plan.size()).arg(replaced)) != QMessageBox::Yes) return;
    if (locked_ || remoteEditing_ || !flush()) return;
    restoring_ = true;
    pauseScroll();
    editor_->setReadOnly(true);
    if (remote_) remote_->setCollectionChanging(true);
    int restored = 0;
    fileJob(this, text("RestoringBackup"), [&] {
        // Keep only one document in memory; every replacement is atomic.
        for (const auto &item : plan) {
            QFile file(item.staged);
            NoteFile note;
            if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); break; }
            if (!decodeNote(file.readAll(), note, error)) break;
            note.sceneId = item.target.sceneId;
            note.sceneName = item.target.sceneName;
            if (QFileInfo(item.target.path).isSymLink()) { error = text("BackupInvalid"); break; }
            if (!saveNoteFile(item.target.path, encodeNote(note), error)) break;
            ++restored;
        }
    });
    for (int i = 0; i < restored; ++i) notes_.accept(plan[i].source, plan[i].target);
    restoring_ = false;
    flush();
    sceneId_.clear();
    auto scene = OBSGetStrongRef(scene_);
    bindScene(scene);
    if (remote_) remote_->setCollectionChanging(false);
    if (refreshAfterRestore_) { refreshAfterRestore_ = false; refreshScene(); }
    const auto result = text("RestoreResult").arg(restored).arg(skipped);
    if (!error.isEmpty()) QMessageBox::warning(this, text("RestoreFailed"), result + "\n" + error);
    else showStatus(result);
}

double Dock::speed() const { return speedBox_->value(); }

void Dock::setSpeed(double value)
{
    if (!std::isfinite(value)) value = 0.5;
    value = qRound(qBound(0.1, value, 5.0) * 10.0) / 10.0;
    if (isScrolling()) scrollFrame();
    const QSignalBlocker a(speedBox_), b(speedSlider_);
    speedBox_->setValue(value);
    speedSlider_->setValue(qRound(value * 10.0));
    if (remote_) remote_->stateChanged();
}

void Dock::startScroll()
{
    if (shuttingDown_ || !editor_->isEnabled() || isScrolling() || isCountingDown()) return;
    auto *bar = editor_->verticalScrollBar();
    if (bar->maximum() <= 0) { showStatus(text("NoScrollNeeded")); return; }
    if (bar->value() >= bar->maximum()) { showStatus(text("AtEnd")); return; }
    setLocked(true);
    if (countdown_->isChecked()) {
        countdownClock_.restart();
        countdownTimer_.start();
        playButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
        updateCountdown();
        editor_->setFocus();
    } else beginScroll();
}

void Dock::updateCountdown()
{
    const int remaining = qMax(0, static_cast<int>((3000 - countdownClock_.elapsed() + 999) / 1000));
    if (remaining == 0) {
        beginScroll();
        return;
    }
    playButton_->setText(QString::number(remaining));
    playButton_->setToolTip(text("CountdownRemaining").arg(remaining));
    showStatus(text("CountdownRemaining").arg(remaining));
}

void Dock::beginScroll()
{
    auto *bar = editor_->verticalScrollBar();
    if (!editor_->isEnabled() || bar->value() >= bar->maximum()) { pauseScroll(); return; }
    countdownTimer_.stop();
    scrollPosition_ = bar->value();
    manualScrollClock_.invalidate();
    scrollTimer_.start();
    updateReadingBand();
    playButton_->setText(text("Pause"));
    playButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    playButton_->setToolTip(text("Pause"));
    playButton_->setIcon(makeIcon(Icon::Pause, palette()));
    showStatus(text("Scrolling"));
    if (remote_) remote_->stateChanged();
    editor_->setFocus();
    scrollClock_.restart();
}

void Dock::pauseScroll()
{
    smoothScroll_->stop();
    const bool wasScrolling = isScrolling() || isCountingDown();
    if (!wasScrolling) return;
    countdownTimer_.stop();
    scrollTimer_.stop();
    if (readingBand_) readingBand_->hide();
    if (playButton_) {
        playButton_->setText(text("Scroll"));
        playButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
        playButton_->setToolTip(text("ScrollTip"));
        playButton_->setIcon(makeIcon(Icon::Play, palette()));
    }
    showStatus(text("Paused"));
    if (remote_) remote_->stateChanged();
}

void Dock::smoothScrollBy(int delta)
{
    auto *bar = editor_->verticalScrollBar();
    const int target = qBound(bar->minimum(), (smoothScroll_->state() == QAbstractAnimation::Running ? smoothScroll_->endValue().toInt() : bar->value()) + delta, bar->maximum());
    smoothScroll_->stop();
    if (target == bar->value()) {
        scrollPosition_ = target; manualScrollClock_.invalidate(); scrollClock_.restart(); return;
    }
    smoothScroll_->setStartValue(bar->value()); smoothScroll_->setEndValue(target); smoothScroll_->start();
}

void Dock::scrollFrame()
{
    // Use elapsed time to keep scrolling independent of frame timing.
    if (!isScrolling()) return;
    auto *bar = editor_->verticalScrollBar();
    if (smoothScroll_->state() == QAbstractAnimation::Running) {
        scrollPosition_ = bar->value(); manualScrollClock_.invalidate(); scrollClock_.restart(); return;
    }
    // Rebase after manual scrolling and hold briefly without leaving playback mode.
    if (bar->value() != static_cast<int>(std::floor(scrollPosition_))) {
        scrollPosition_ = bar->value();
        manualScrollClock_.restart();
    }
    const auto elapsed = scrollClock_.restart();
    if (bar->isSliderDown() || (manualScrollClock_.isValid() && manualScrollClock_.elapsed() < 1000)) return;
    scrollPosition_ += elapsed * BaseScrollSpeed * speed() / 1000.0;
    bar->setValue(qMin(bar->maximum(), static_cast<int>(std::floor(scrollPosition_))));
    if (scrollPosition_ >= bar->maximum()) {
        pauseScroll();
        showStatus(text("ReachedEnd"));
    }
}

void Dock::command(Command command)
{
    if (shuttingDown_ || restoring_) return;

    switch (command) {
    case Command::Start: if (!isScrolling()) startScroll(); break;
    case Command::Pause: pauseScroll(); break;
    case Command::Toggle: if (isScrolling() || isCountingDown()) pauseScroll(); else startScroll(); break;
    case Command::Faster: setSpeed(speed() + 0.1); break;
    case Command::Slower: setSpeed(speed() - 0.1); break;
    case Command::Top: pauseScroll(); editor_->verticalScrollBar()->setValue(0); showStatus(text("AtTop")); break;
    case Command::InsertMark:
        if (editor_->isEnabled() && !editor_->isReadOnly()) {
            auto cursor = editor_->textCursor(); cursor.clearSelection();
            const auto mark = QStringLiteral("\u25c6[]");
            if (static_cast<Editor *>(editor_)->acceptsText(mark, cursor)) {
                auto format = cursor.charFormat();
                format.setForeground(QColor(palette().color(QPalette::Window).lightness() < 128 ? "#ffb347" : "#b85b00"));
                cursor.beginEditBlock(); cursor.insertText(mark, format); cursor.endEditBlock();
                // Enter the optional note directly in the document.
                cursor.movePosition(QTextCursor::PreviousCharacter);
                editor_->setTextCursor(cursor); editor_->setCurrentCharFormat(format); editor_->setFocus();
            }
        }
        break;
    case Command::NextMark:
        if (editor_->isEnabled()) {
            // Reuse the cached search and its wraparound navigation.
            searchInput_->setText(QStringLiteral("\u25c6")); searchTimer_.stop(); searchFromStart_ = false; markSearch_ = true;
            findText(); editor_->setFocus();
        }
        break;
    case Command::InsertTime:
        if (editor_->isEnabled() && !editor_->isReadOnly())
            static_cast<Editor *>(editor_)->insertCheckedText(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss "));
        break;
    case Command::ReturnPosition: returnToPosition(); break;
    case Command::ToggleLock:
        if (lock_->isEnabled()) lock_->trigger();
        break;
    }
}

bool Dock::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonDblClick && paint_ && watched == toolbar_->widgetForAction(paint_) &&
        static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        setFormatPainter(true, true);
        return true;
    }
    if (paint_ && paint_->isChecked()) {
        if (watched == editor_ && event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            setFormatPainter(false); return true;
        }
        if ((watched == editor_->viewport() && event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) ||
            (watched == editor_ && event->type() == QEvent::KeyRelease && (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Shift ||
                (static_cast<QKeyEvent *>(event)->key() >= Qt::Key_Home && static_cast<QKeyEvent *>(event)->key() <= Qt::Key_Down))))
            QTimer::singleShot(0, this, &Dock::paintSelection);
    }
    if (watched == goToInput_) {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            goToInput_->hide(); editor_->setFocus(); return true;
        }
    }
    if ((watched == documentStatus_ || watched == scrollPercent_) && event->type() == QEvent::MouseButtonRelease &&
        static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        showGoTo(watched == scrollPercent_);
        return true;
    }
    if (watched == controls_ && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest)) {
        if (event->type() == QEvent::Resize && goToInput_) goToInput_->hide();
        updateControlLayout();
    }
    if (watched == searchInput_ && event->type() == QEvent::FocusIn) pauseScroll();
    if (watched == editor_->viewport() && event->type() == QEvent::Resize) {
        if (remote_) remote_->hideQr();
        updateReadingBand();
        updateSideControls();
    }
    if (event->type() == QEvent::Wheel || event->type() == QEvent::MouseButtonPress) {
        if (remote_) remote_->hideQr();
    }
    if (watched == editor_->viewport() && event->type() == QEvent::Wheel) {
        const auto *wheel = static_cast<QWheelEvent *>(event);
        if (wheel->modifiers() == Qt::NoModifier && wheel->pixelDelta().isNull() && wheel->angleDelta().y()) {
            smoothScrollBy(qRound(-wheel->angleDelta().y() * editor_->verticalScrollBar()->singleStep() * QApplication::wheelScrollLines() / 120.0));
            return true;
        }
    }
    if (watched == editor_->viewport() &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton && mouse->modifiers() == (Qt::AltModifier | Qt::ShiftModifier)) {
            if (event->type() == QEvent::MouseButtonPress && editor_->isEnabled() && !editor_->isReadOnly()) {
                editor_->setTextCursor(editor_->cursorForPosition(mouse->position().toPoint()));
                command(Command::InsertMark);
            }
            return true;
        }
        if (mouse->button() == Qt::LeftButton && mouse->modifiers() == Qt::AltModifier) {
            if (event->type() == QEvent::MouseButtonPress) { command(Command::ToggleLock); editor_->setFocus(); }
            return true;
        }
    }
    if (watched == editor_ && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (!editor_->isReadOnly() && key->key() == Qt::Key_Backspace && key->modifiers() == Qt::NoModifier && !editor_->textCursor().hasSelection()) {
            auto mark = markAt(editor_->textCursor());
            if (!mark.isNull() && editor_->textCursor().position() == mark.selectionEnd()) {
                mark.beginEditBlock(); mark.removeSelectedText(); mark.endEditBlock(); editor_->setTextCursor(mark); return true;
            }
        }
        if (remote_ && remote_->hideQr() && key->key() == Qt::Key_Escape) return true;
        if (key->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) && key->key() == Qt::Key_V) {
            if (!editor_->isReadOnly()) static_cast<Editor *>(editor_)->insertCheckedText(QApplication::clipboard()->text());
            return true;
        }
        if (editor_->isReadOnly() && (key->modifiers() & ~Qt::ShiftModifier) == Qt::NoModifier) {
            if (key->key() == Qt::Key_Space) { command(Command::Toggle); return true; }
            if (key->key() == Qt::Key_Plus || key->key() == Qt::Key_Equal) { command(Command::Faster); return true; }
            if (key->key() == Qt::Key_Minus) { command(Command::Slower); return true; }
            if (key->key() == Qt::Key_Home) { command(Command::Top); return true; }
            if (key->modifiers() == Qt::NoModifier && (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)) {
                const int step = editor_->verticalScrollBar()->singleStep();
                smoothScrollBy(key->key() == Qt::Key_Up ? -step : step);
                return true;
            }
            if (key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown) pauseScroll();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void Dock::hideEvent(QHideEvent *event)
{
    setFormatPainter(false);
    if (remote_) remote_->hideQr();
    rememberPosition();
    pauseScroll();
    flush();
    QWidget::hideEvent(event);
}

void Dock::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (sceneId_.isEmpty()) return;
    const auto id = sceneId_;
    const int position = positions_.value(id, preferences_.value("readingPositions/" + id, 0).toInt());
    // Restore the reading offset after layout.
    QTimer::singleShot(0, this, [this, id, position] {
        if (sceneId_ == id && isVisible() && !isScrolling() && !isCountingDown())
            editor_->verticalScrollBar()->setValue(position);
    });
}

void Dock::rememberReturnPosition()
{
    if (returnPosition_ >= 0 || !editor_->isEnabled()) return;
    returnPosition_ = editor_->verticalScrollBar()->value();
    returnCursor_ = editor_->textCursor();
    returnButton_->show(); updateSideControls();
}

void Dock::clearReturnPosition()
{
    if (returnPosition_ < 0) return;
    returnPosition_ = -1; returnCursor_ = QTextCursor(); returnButton_->hide();
}

bool Dock::returnToPosition()
{
    if (returnPosition_ < 0 || !editor_->isEnabled()) return false;
    const int position = returnPosition_; const auto cursor = returnCursor_;
    clearReturnPosition(); pauseScroll(); searchInput_->clear();
    editor_->setFocus(); editor_->setTextCursor(cursor); editor_->verticalScrollBar()->setValue(position);
    return true;
}

void Dock::updateSideControls()
{
    if (!returnButton_) return;
    const int width = qrButton_->width() + 8;
    static_cast<Editor *>(editor_)->setSideSpace(width);
    const auto corner = editor_->viewport()->mapTo(this, QPoint(editor_->viewport()->width() + width - 4, 4));
    returnButton_->move(corner.x() - returnButton_->width(), corner.y()); returnButton_->raise();
    qrButton_->move(corner.x() - qrButton_->width(), corner.y() + qMax(0, editor_->viewport()->height() - qrButton_->height() - 8));
    qrButton_->raise();
}

void Dock::rememberPosition()
{
    if (!sceneId_.isEmpty()) positions_[sceneId_] = editor_->verticalScrollBar()->value();
}

void Dock::savePreferences()
{
    for (auto it = positions_.cbegin(); it != positions_.cend(); ++it)
        preferences_.setValue("readingPositions/" + it.key(), it.value());
    preferences_.sync();
}

void Dock::showStatus(const QString &text)
{
    if (!status_) return;
    status_->setText("]");
    status_->setAccessibleName(text);
    status_->setToolTip(text);
}

void Dock::showSaveError(const QString &detail)
{
    // Keep failures visible until the note is successfully saved.
    saveError_->setText("! " + text("NoteSaveFailed"));
    saveError_->setToolTip(detail);
    saveError_->setAccessibleName(text("NoteSaveFailed") + " " + detail);
    saveError_->show();
}

void Dock::shutdown()
{
    if (shuttingDown_) return;
    if (goToInput_) goToInput_->hide();
    if (remote_) remote_->shutdown();
    rememberPosition();
    flush(false);
    pauseScroll();
    searchTimer_.stop();
    autoClearTimer_.stop();
    autoClearPending_.clear();
    savePreferences();
    shuttingDown_ = true;
}
}
