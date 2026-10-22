#pragma once
#include <QDockWidget>
#include <QInputMethodEvent>
#include <QMainWindow>
#include <QPropertyAnimation>

void editingChecks(QApplication &, const QString &preview)
{
    returnChecks(preview);
    QTemporaryDir temp;
    require(temp.isValid(), "editing temporary directory");
    OBSSceneAutoRelease scene = obs_scene_create("Editing checks");
    QMainWindow window;
    auto *container = new QDockWidget("ScenePad", &window);
    auto *dock = new scenepad::Dock(temp.filePath("settings.ini"), container);
    container->setWidget(dock); window.addDockWidget(Qt::BottomDockWidgetArea, container);
    window.resize(900, 420); dock->bindScene(obs_scene_get_source(scene)); window.show(); window.activateWindow(); waitEvents(40);
    auto *editor = dock->findChild<QTextEdit *>("noteEditor");
    auto *search = dock->findChild<QLineEdit *>("searchText");
    auto *status = dock->findChild<QLabel *>("searchStatus");
    auto *toolbar = dock->findChild<QToolBar *>("formatToolbar");
    auto *next = qobject_cast<QToolButton *>(toolbar->widgetForAction(dock->findChild<QAction *>("nextMark")));
    auto click = [&](QWidget *widget) {
        const auto center = widget->rect().center();
        QMouseEvent down(QEvent::MouseButtonPress, center, widget->mapToGlobal(center), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent up(QEvent::MouseButtonRelease, center, widget->mapToGlobal(center), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &down); QApplication::sendEvent(widget, &up);
    };
    auto select = [&](int a, int b) { auto c=editor->textCursor(); c.setPosition(a); c.setPosition(b, QTextCursor::KeepAnchor); editor->setTextCursor(c); };
    require(dock->remote()->listen("127.0.0.1",0), "QR copy check listens on loopback");
    auto *qr=dock->findChild<QToolButton *>("remoteQrButton");
    for(int i=0;i<10;++i){
        qr->click();
        require(QApplication::clipboard()->text()==dock->remote()->url(), "QR icon copies the current link while toggling the popup");
    }
    dock->remote()->stop();
    std::printf("PASS QR COPY: 10 icon clicks copy the current link\n");
    auto *strike = dock->findChild<QAction *>("formatStrike");
    auto *paint = dock->findChild<QAction *>("formatPainter");
    require(strike && paint && strike->shortcut().isEmpty() && paint->shortcut().isEmpty(), "new tools have no default hotkeys");
    QElapsedTimer menuClock; menuClock.start();
    for(int i=0;i<20;++i){
        editor->setPlainText(i%2 ? QString::fromUtf8("◆[note] text") : QString("Plain text"));
        bool valid=false;
        scenepad::test::contextMenu(editor,[&](QMenu *menu){
            valid=!menu->findChild<QAction *>("ctxGoTo");
            const auto entries=menu->actions();
            for(int j=1;j<entries.size();++j)valid&=!entries[j-1]->isSeparator() || !entries[j]->isSeparator();
        });
        require(valid,"context menu omits Go to and keeps distinct groups with and without marks");
    }
    require(menuClock.elapsed()<5000,"repeated context menus remain responsive");
    std::printf("PASS CONTEXT MENU: 20 opens with/without marks=%lldms\n",menuClock.elapsed());
    editor->setPlainText("source target tail"); select(0,6);
    strike->trigger(); require(editor->textCursor().charFormat().fontStrikeOut(), "strikethrough turns on");
    strike->trigger(); require(!editor->textCursor().charFormat().fontStrikeOut(), "strikethrough turns off");
    QTextCharFormat sample; sample.setFontWeight(QFont::Bold); sample.setFontItalic(true); sample.setFontUnderline(true);
    sample.setFontStrikeOut(true); sample.setFontPointSize(22); sample.setForeground(QColor("#b82030")); sample.setBackground(QColor("#e0c080"));
    editor->mergeCurrentCharFormat(sample);
    auto *paintButton=qobject_cast<QToolButton *>(toolbar->widgetForAction(paint));
    click(paintButton); require(paint->isChecked(), "painter arms without changing the source");
    select(7,13); QKeyEvent release(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier); QApplication::sendEvent(editor, &release); waitEvents(10);
    const auto painted=editor->textCursor().charFormat();
    require(!paint->isChecked() && painted.fontWeight()==QFont::Bold && painted.fontItalic() && painted.fontUnderline() && painted.fontStrikeOut() &&
        painted.fontPointSize()==22 && painted.foreground().color()==QColor("#b82030") && painted.background().color()==QColor("#e0c080"), "one-shot painter copies character formatting");
    require(editor->toPlainText()=="source target tail", "painting preserves script text");
    editor->undo(); select(7,13); require(!editor->textCursor().charFormat().fontStrikeOut(), "paint is one undo step");
    editor->redo(); select(7,13); click(paintButton); select(14,18); QApplication::sendEvent(editor,&release); waitEvents(10);
    require(!paint->isChecked() && editor->textCursor().charFormat().fontStrikeOut(), "painter can be reused");
    select(6,7); click(paintButton); select(7,13); QApplication::sendEvent(editor,&release); waitEvents(10);
    require(!editor->textCursor().charFormat().fontStrikeOut() && editor->textCursor().charFormat().foreground().style()==Qt::NoBrush &&
        editor->textCursor().charFormat().background().style()==Qt::NoBrush,"plain source clears target color and decoration");
    editor->undo(); select(7,13);
    select(7,7); const auto dragStart=editor->cursorRect().center();
    select(13,13); const auto dragEnd=editor->cursorRect().center();
    select(0,6); click(paintButton);
    QMouseEvent dragDown(QEvent::MouseButtonPress,dragStart,editor->viewport()->mapToGlobal(dragStart),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent dragMove(QEvent::MouseMove,dragEnd,editor->viewport()->mapToGlobal(dragEnd),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent dragUp(QEvent::MouseButtonRelease,dragEnd,editor->viewport()->mapToGlobal(dragEnd),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(editor->viewport(),&dragDown); QApplication::sendEvent(editor->viewport(),&dragMove); QApplication::sendEvent(editor->viewport(),&dragUp); waitEvents(10);
    require(!paint->isChecked() && editor->textCursor().selectedText()=="target", "mouse drag applies painter once to the target selection");
    auto doubleClick = [&] {
        click(paintButton);
        const auto center=paintButton->rect().center();
        QMouseEvent down(QEvent::MouseButtonDblClick,center,paintButton->mapToGlobal(center),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent up(QEvent::MouseButtonRelease,center,paintButton->mapToGlobal(center),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(paintButton,&down); QApplication::sendEvent(paintButton,&up);
    };
    editor->setPlainText("source target tail"); editor->selectAll(); editor->setCurrentCharFormat(QTextCharFormat());
    select(0,6); editor->mergeCurrentCharFormat(sample);
    editor->document()->clearUndoRedoStacks();
    doubleClick(); require(paint->isChecked(), "double-click arms continuous painter after the second release");
    select(7,13); QApplication::sendEvent(editor,&release); waitEvents(10);
    require(paint->isChecked() && editor->textCursor().charFormat().fontStrikeOut(), "continuous painter remains active after first target");
    select(14,18); QApplication::sendEvent(editor,&release); waitEvents(10);
    require(paint->isChecked() && editor->textCursor().charFormat().foreground().color()==sample.foreground().color(), "continuous painter retains the original format for another target");
    click(paintButton); require(!paint->isChecked(), "click exits continuous painter");
    editor->undo(); select(14,18); require(!editor->textCursor().charFormat().fontStrikeOut(), "continuous targets undo separately");
    select(7,13); require(editor->textCursor().charFormat().fontStrikeOut(), "undo leaves the previous painted target intact");
    doubleClick(); key(editor,Qt::Key_Escape); require(!paint->isChecked(), "Escape exits continuous painter");
    doubleClick(); select(18,18); editor->insertPlainText("x"); require(!paint->isChecked(), "typing exits continuous painter");
    select(18,19); editor->textCursor().removeSelectedText();
    doubleClick(); dock->bindScene(nullptr); require(!paint->isChecked(), "scene change exits continuous painter");
    dock->bindScene(obs_scene_get_source(scene)); select(7,13);
    click(paintButton); key(editor,Qt::Key_Escape); require(!paint->isChecked(), "Escape cancels painter");
    click(paintButton); click(paintButton); require(!paint->isChecked(), "second click cancels painter");
    doubleClick(); dock->command(scenepad::Command::ToggleLock);
    require(!paint->isChecked() && !paint->isEnabled() && !strike->isEnabled(), "lock cancels and disables new tools");
    dock->command(scenepad::Command::ToggleLock); require(dock->flush(), "save formatted script");
    dock->bindScene(nullptr); dock->bindScene(obs_scene_get_source(scene)); select(7,13);
    require(editor->textCursor().charFormat().fontStrikeOut(), "painted formatting persists after reload");
    if(!preview.isEmpty())require(window.grab().save("preview-format-tools.png"),"save format tools evidence");
    editor->setPlainText("ab\ncd"); select(0,5);
    require(dock->findChild<QLabel *>("characterCount")->text().contains('4'), "selection count excludes paragraph separators");
    editor->moveCursor(QTextCursor::End);
    require(dock->findChild<QLabel *>("characterCount")->text().contains('5'), "clearing selection restores total count");
    std::printf("PASS FORMAT NATIVE: strike toggle, painter reuse/cancel/lock/undo/reload and selected counts\n");
    const auto mark = QString::fromUtf8("◆[]");
    editor->setPlainText(QString("prefix%1 middle%1 tail%1 end").arg(mark)); editor->moveCursor(QTextCursor::Start);
    click(next); click(next);
    require(status->text() == "2/3", "mouse navigation reaches the second mark");
    auto cursor = editor->textCursor(); cursor.setPosition(cursor.selectionEnd() + 2); editor->setTextCursor(cursor);
    const int beforeEdit = cursor.position();
    QInputMethodEvent input; input.setCommitString(QString::fromUtf8("中文")); QApplication::sendEvent(editor, &input); waitEvents(200);
    require(search->text().isEmpty() && !status->isVisible() && editor->hasFocus() && editor->textCursor().position() == beforeEdit + 2,
        "IME editing ends automatic search without moving focus or caret");
    click(next); require(status->text() == "3/3", "navigation resumes after the current caret");
    click(next); require(status->text() == "1/3", "navigation wraps only after the last mark");
    for (const auto &query : {QString("tail"), QString::fromUtf8("◆")}) {
        search->clear(); search->setText(query); waitSearch(*dock);
        editor->moveCursor(QTextCursor::End); editor->insertPlainText("x"); waitSearch(*dock);
        require(search->text() == query && status->isVisible(), "manual text and symbol searches survive editing");
    }
    editor->setPlainText((QString(95, 'a') + mark + '\n').repeated(2000)); editor->moveCursor(QTextCursor::Start);
    QElapsedTimer selectionClock; selectionClock.start();
    for(int i=0;i<1000;++i)select(i%2?0:100000,i%2?5:100005);
    require(selectionClock.elapsed()<5000,"1000 long-script selection counts remain responsive");
    editor->moveCursor(QTextCursor::Start);
    std::printf("PASS SELECTION NATIVE: 198000 characters, 1000 selections=%lldms\n",selectionClock.elapsed());
    selectionClock.restart();
    select(100000,100005);
    for(int i=0;i<1000;++i)strike->trigger();
    require(selectionClock.elapsed()<5000 && dock->textLength()==198000,"1000 format toggles preserve long script and remain responsive");
    std::printf("PASS FORMAT STRESS NATIVE: 198000 characters, 1000 toggles=%lldms\n",selectionClock.elapsed());
    select(0,5); doubleClick(); selectionClock.restart();
    for(int i=0;i<100;++i){
        select(100+i*99,105+i*99); QApplication::sendEvent(editor,&release); waitEvents(1);
        require(paint->isChecked(), "continuous painter stays active across 100 long-script targets");
    }
    require(selectionClock.elapsed()<5000 && dock->textLength()==198000,"continuous painter preserves long script and remains responsive");
    click(paintButton);
    std::printf("PASS CONTINUOUS NATIVE: 198000 characters, 100 targets=%lldms\n",selectionClock.elapsed());
    editor->moveCursor(QTextCursor::Start);
    auto *start = dock->findChild<QFrame *>("speedStartSeparator");
    auto *end = dock->findChild<QFrame *>("speedEndSeparator");
    auto *slider = dock->findChild<QSlider *>("scrollSlider");
    auto *speed = dock->findChild<QDoubleSpinBox *>("scrollSpeed");
    require(start && end && slider && speed, "speed controls and separators exist");
    int compactSlider = 0, compactControls = 0;
    for (const int width : {700, 720, 900}) {
        window.resize(width, 420); waitEvents(30);
        require(start->isVisible() && end && end->isVisible(), "both speed separators remain visible");
        for(auto *action:toolbar->actions())
            require(toolbar->widgetForAction(action)->isVisible(), "all formatting controls remain visible");
        auto *paragraphs = dock->findChild<QToolBar *>("paragraphToolbar");
        for (auto *action : paragraphs->actions())
            require(paragraphs->widgetForAction(action)->isVisible(), "all paragraph controls remain visible");
        if (width == 700) { compactSlider = slider->width(); compactControls = slider->parentWidget()->width(); }
        if (width == 900) require(qAbs((slider->width() - compactSlider) - (slider->parentWidget()->width() - compactControls)) <= 1,
            "the speed slider absorbs row width changes without compressing the buttons");
        require(start->mapTo(dock, QPoint(start->width(), 0)).x() <= slider->mapTo(dock, QPoint()).x() &&
            speed->mapTo(dock, QPoint(speed->width(), 0)).x() <= end->mapTo(dock, QPoint()).x(), "separators enclose the slider and multiplier");
        require(end->styleSheet() == dock->findChild<QFrame *>("characterSeparator")->styleSheet(), "speed separator follows the statistics theme");
        if (width == 700 && !preview.isEmpty()) require(window.grab().save("preview-speed-controls.png"), "save compact speed controls");
    }
    std::printf("PASS SPEED NATIVE: 700/720/900px, all controls visible, slider shrinks, separators ordered and theme matched\n");
    search->clear(); QElapsedTimer clock; clock.start();
    for (int i=0; i<1000; ++i) dock->command(scenepad::Command::NextMark);
    const auto markMs = clock.elapsed();
    require(status->text() == "1000/2000" && markMs < 5000, "1000 cached mark jumps remain responsive");
    editor->moveCursor(QTextCursor::End); clock.restart();
    for (int i=0; i<1000; ++i) { editor->insertPlainText("x"); if (i%100 == 0) waitEvents(2); }
    const auto editMs = clock.elapsed(); waitEvents(200);
    require(search->text().isEmpty() && !status->isVisible() && editor->hasFocus() && editMs < 5000,
        "1000 long-script edits keep automatic search inactive");
    click(next); require(status->text() == "1/2000", "editing at the end resumes with wraparound");
    std::printf("PASS MARK SEARCH NATIVE: chars=%d marks=2000 cached-1000=%lldms edits-1000=%lldms; IME, caret, wraparound and manual queries\n", dock->textLength(), markMs, editMs);
    search->clear();

    auto *progress = dock->findChild<QLabel *>("scrollPercent");
    click(progress); auto *jump = dock->findChild<QLineEdit *>("goToInput");
    require(jump && jump->hasFocus() && jump->selectedText() == jump->text(), "inline navigation selects the full percentage");
    jump->setText("100"); key(jump, Qt::Key_Return);
    require(jump->isHidden() && editor->textCursor().blockNumber() == 99, "digits from the progress entry locate a line");
    click(dock->findChild<QLabel *>("documentStatus")); jump->setText("50%"); key(jump, Qt::Key_Return);
    require(jump->isHidden() && qAbs(dock->scrollProgress() - .5) < .01, "an explicit percentage from the line entry locates progress");
    const int savedPosition = editor->verticalScrollBar()->value();
    click(progress); jump->setText("0"); key(jump, Qt::Key_Return);
    require(jump->isVisible() && jump->property("invalid").toBool() && editor->verticalScrollBar()->value() == savedPosition, "bare zero is not converted to a percentage");
    key(jump, Qt::Key_Escape);
    const auto html = editor->toHtml(); clock.restart();
    for (int i=0; i<1000; ++i) require(dock->goToPosition(i%2 ? QString::number(1+i%2000) : QString::number(i%101)+'%'), "indexed navigation accepts valid input");
    const auto jumpMs = clock.elapsed();
    require(jumpMs < 5000 && editor->toHtml() == html, "1000 indexed jumps preserve the long script");
    for (const auto &target : {QString("0"), QString("2002"), QString("101%"), QString("text")}) require(!dock->goToPosition(target), "invalid navigation is rejected");
    require(dock->goToPosition("0%") && dock->goToPosition("100%"), "percentage endpoints are accepted");
    std::printf("PASS NAVIGATION NATIVE: chars=%d jumps=1000 elapsed=%lldms; both entries, full selection, units and invalid input\n", dock->textLength(), jumpMs);
    auto *bar=editor->verticalScrollBar();
    dock->findChild<QCheckBox *>("startCountdown")->setChecked(false);bar->setValue(bar->maximum()/3);dock->setSpeed(5);dock->startScroll();
    require(dock->isScrolling(),"keyboard test starts auto-scroll");clock.restart();
    for(int i=0;i<1000;++i){key(editor,i%2?Qt::Key_Up:Qt::Key_Down);require(dock->isScrolling(),"arrow keys never stop auto-scroll");}
    auto *animation=dock->findChild<QPropertyAnimation *>("readingScrollAnimation");
    require(animation,"arrow easing animation exists" );
    QElapsedTimer settle;settle.start();
    while(animation->state()==QAbstractAnimation::Running && settle.elapsed()<1000)waitEvents(5);
    require(animation->state()==QAbstractAnimation::Stopped,"arrow easing settles before measuring resumed playback");
    const int afterKeys=bar->value();waitEvents(250);
    require(dock->isScrolling() && bar->value()>afterKeys,"auto-scroll continues immediately after arrow nudges");dock->pauseScroll();
    std::printf("PASS SCROLL KEYS NATIVE: 1000 arrow keys=%lldms\n",clock.elapsed());
    const int wheelStart=bar->value();wheel(editor,-120,Qt::NoModifier);
    require(animation && animation->state()==QAbstractAnimation::Running && bar->value()==wheelStart,"wheel starts a short animation without an immediate jump");
    QElapsedTimer firstFrame; firstFrame.start();
    while(bar->value()==wheelStart && firstFrame.elapsed()<120)waitEvents(5);
    require(bar->value()>wheelStart,"wheel advances over animation frames");
    dock->goToPosition("50%");const int direct=bar->value();waitEvents(180);
    require(bar->value()==direct && animation->state()!=QAbstractAnimation::Running,"direct navigation cancels pending motion");
    clock.restart();for(int i=0;i<1000;++i)wheel(editor,i%2?120:-120,Qt::NoModifier);
    require(clock.elapsed()<5000 && dock->findChildren<QPropertyAnimation *>("readingScrollAnimation").size()==1,"1000 wheel events reuse one bounded animation");
    dock->command(scenepad::Command::Top);waitEvents(180);require(bar->value()==0,"top navigation cannot be displaced by an old animation");
    std::printf("PASS SMOOTH NATIVE: 1000 wheels=%lldms; frame motion, one animation, direct navigation and pause cancellation\n",clock.elapsed());
    if (!preview.isEmpty()) require(window.grab().save(preview + ".png"), "save editing evidence");
    dock->shutdown(); window.close();
}
