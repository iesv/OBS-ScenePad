#pragma once
#include <QDockWidget>
#include <QMainWindow>

void returnChecks(const QString &preview)
{
    QTemporaryDir temp;
    require(temp.isValid(), "return temporary directory");
    OBSSceneAutoRelease scene = obs_scene_create("Return checks");
    QMainWindow window;
    auto *container = new QDockWidget("ScenePad", &window);
    auto *dock = new scenepad::Dock(temp.filePath("settings.ini"), container);
    container->setWidget(dock); window.addDockWidget(Qt::BottomDockWidgetArea, container);
    window.resize(900,420);dock->bindScene(obs_scene_get_source(scene));window.show();window.activateWindow();waitEvents(40);
    auto *editor=dock->findChild<QTextEdit *>("noteEditor");
    auto *search=dock->findChild<QLineEdit *>("searchText");
    auto select=[&](int a,int b){auto cursor=editor->textCursor();cursor.setPosition(a);cursor.setPosition(b,QTextCursor::KeepAnchor);editor->setTextCursor(cursor);};
    editor->setPlainText((QString(90,'a') + QString::fromUtf8(" ◆[note] target\n")).repeated(1800));
    editor->document()->documentLayout()->documentSize(); waitEvents(40);
    auto *back=dock->findChild<QToolButton *>("returnPosition");
    require(back && back->isHidden() && !dock->canReturnToPosition(),"return is initially unavailable");
    select(70000,70005); auto *bar=editor->verticalScrollBar(); bar->setValue(bar->maximum()*37/100);
    const int origin=bar->value(),originStart=editor->textCursor().selectionStart(),originEnd=editor->textCursor().selectionEnd();
    const auto originalHtml=editor->toHtml(); QElapsedTimer clock; clock.start();
    for(int i=0;i<1000;++i){require(dock->goToPosition(QString::number(i%100)+'%'),"return stress accepts percentage targets");dock->command(scenepad::Command::NextMark);}
    require(clock.elapsed()<5000 && !back->isHidden(),"1000 mixed jumps retain one return position without delaying navigation");
    std::printf("PASS RETURN NATIVE STRESS: chars=%d, 1000 mixed jumps=%lldms\n",dock->textLength(),clock.elapsed());
    waitEvents(40); const int sideWidth=editor->viewport()->width();
    dock->setRemoteAvailable(true); waitEvents(10);
    auto *returnQr=dock->findChild<QToolButton *>("remoteQrButton");
    require(back->size()==returnQr->size() && back->size()==QSize(24,24) && back->iconSize()==returnQr->iconSize() && !back->icon().isNull() && back->text().isEmpty(),"return uses the same 24px icon-only control as QR");
    require(returnQr->x()-editor->viewport()->mapTo(dock,QPoint(editor->viewport()->width(),0)).x()==4,"the fixed 32px side column leaves only 4px beside the text");
    require(back->geometry().right()==returnQr->geometry().right(),"return aligns with the QR right edge");
    require(returnQr->geometry().top()>back->geometry().bottom(),"return stays above QR in the side column");
    require(back->y()-editor->viewport()->mapTo(dock,QPoint()).y()==4 && editor->viewport()->mapTo(dock,QPoint(0,editor->viewport()->height())).y()-returnQr->geometry().bottom()==5,"side controls keep four-pixel top and bottom insets");
    if(!preview.isEmpty())require(window.grab().save(preview+".side.png"),"save swapped side controls");
    require(editor->viewport()->width()==sideWidth,"showing QR preserves text width");
    dock->setRemoteAvailable(false); waitEvents(10);
    require(editor->viewport()->width()==sideWidth,"QR visibility never changes text width");
    back->click();
    require(back->isHidden() && bar->value()==origin && editor->textCursor().selectionStart()==originStart && editor->textCursor().selectionEnd()==originEnd,"return restores the original scroll and selection after repeated jumps");
    require(editor->viewport()->width()==sideWidth && editor->toHtml()==originalHtml,"return visibility preserves layout and document data");
    search->setText("absent return needle"); waitSearch(*dock);require(back->isHidden(),"unsuccessful search does not create a return position");search->clear();
    select(70000,70005);bar->setValue(origin);search->setText("target");waitSearch(*dock);
    require(!back->isHidden(),"successful search records a return position");
    dock->command(scenepad::Command::ReturnPosition);
    require(back->isHidden() && bar->value()==origin && editor->textCursor().selectionStart()==70000,"return command restores search origin");
    require(!dock->goToPosition("101%") && back->isHidden(),"invalid jumps do not record a return position");
    dock->goToPosition("80%");editor->insertPlainText("x");require(back->isHidden(),"editing invalidates the return position");editor->undo();
    dock->goToPosition("80%");dock->bindScene(nullptr);require(back->isHidden(),"scene changes invalidate the return position");dock->bindScene(obs_scene_get_source(scene));
    select(70000,70005);bar->setValue(origin);clock.restart();
    for(int i=0;i<1000;++i){
        require(dock->goToPosition("80%") && dock->returnToPosition(),"repeated navigation and return succeed");
        require(bar->value()==origin && editor->textCursor().selectionStart()==70000 && editor->textCursor().selectionEnd()==70005 && back->isHidden(),"each return restores the selection and scroll, then clears the origin");
    }
    require(clock.elapsed()<5000,"1000 return cycles remain responsive");
    require(!dock->returnToPosition(),"return without a saved origin is a no-op");
    std::printf("PASS RETURN NATIVE: cursor/scroll, search, one origin, invalid/edit/scene guards, fixed side column; 1000 return cycles=%lldms\n",clock.elapsed());
    dock->shutdown();window.close();
}
