#pragma once
#include <QString>
#include <obs-module.h>

namespace scenepad {
inline QString text(const char *key)
{
    auto value = QString::fromUtf8(obs_module_text(key));
#ifdef __APPLE__
    if (qstrcmp(key, "ShortcutBody") == 0) value.replace("Ctrl+Shift+", "⇧⌘").replace("Ctrl+", "⌘").replace("Alt+", "Option+");
#endif
    return value;
}
}
