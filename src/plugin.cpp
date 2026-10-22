#include "dock.hpp"
#include "remote.hpp"
#include <QDir>
#include <QFileInfo>
#include <QMainWindow>
#include <QPointer>
#include <obs-module.h>
#include <array>

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("haliluya.net")
OBS_MODULE_USE_DEFAULT_LOCALE("obs-scenepad", "en-US")

namespace {
QPointer<scenepad::Dock> dock;
bool exiting = false;
struct Hotkey {
    const char *name;
    const char *description;
    scenepad::Command command;
    obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
};
std::array<Hotkey, 11> hotkeys{{
    {"ScenePad.StartScroll", "HotkeyStart", scenepad::Command::Start},
    {"ScenePad.PauseScroll", "HotkeyPause", scenepad::Command::Pause},
    {"ScenePad.ToggleScroll", "HotkeyToggle", scenepad::Command::Toggle},
    {"ScenePad.Faster", "HotkeyFaster", scenepad::Command::Faster},
    {"ScenePad.Slower", "HotkeySlower", scenepad::Command::Slower},
    {"ScenePad.Top", "HotkeyTop", scenepad::Command::Top},
    {"ScenePad.InsertTime", "HotkeyTime", scenepad::Command::InsertTime},
    {"ScenePad.ToggleLock", "HotkeyToggleLock", scenepad::Command::ToggleLock},
    {"ScenePad.InsertMark", "HotkeyMark", scenepad::Command::InsertMark},
    {"ScenePad.NextMark", "HotkeyNextMark", scenepad::Command::NextMark},
    {"ScenePad.ReturnPosition", "HotkeyReturn", scenepad::Command::ReturnPosition}
}};

void hotkeyPressed(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (!pressed) return;
    const auto command = static_cast<Hotkey *>(data)->command;
    const auto target = dock;
    if (!target) return;
    // Dispatch OBS hotkeys on the Qt UI thread.
    QMetaObject::invokeMethod(target, [target, command] {
        if (target && !exiting) target->command(command);
    }, Qt::QueuedConnection);
}

void frontendEvent(obs_frontend_event event, void *)
{
    if (!dock) return;
    if (event == OBS_FRONTEND_EVENT_EXIT) {
        dock->shutdown();
        exiting = true;
        return;
    }
    dock->frontendEvent(event);
}

void sceneRenamed(void *, calldata_t *data)
{
    auto *source = static_cast<obs_source_t *>(calldata_ptr(data, "source"));
    if (!dock || !obs_scene_from_source(source)) return;
    const auto weak = OBSGetWeakRef(source);
    const auto target = dock;
    // Resolve renamed sources on the UI thread without retaining removed scenes.
    QMetaObject::invokeMethod(target, [target, weak] {
        if (auto scene = OBSGetStrongRef(weak); target && scene && !exiting) target->sceneRenamed(scene);
    }, Qt::QueuedConnection);
}

void saveCallback(obs_data_t *saveData, bool saving, void *)
{
    if (!dock || exiting) return;
    if (saving) {
        dock->flush(false);
        OBSDataAutoRelease data = obs_data_create();
        for (const auto &hotkey : hotkeys) {
            OBSDataArrayAutoRelease keys = obs_hotkey_save(hotkey.id);
            obs_data_set_array(data, hotkey.name, keys);
        }
        obs_data_set_obj(saveData, "scenepad_hotkeys", data);
    } else {
        OBSDataAutoRelease data = obs_data_get_obj(saveData, "scenepad_hotkeys");
        for (const auto &hotkey : hotkeys) {
            OBSDataArrayAutoRelease keys = obs_data_get_array(data, hotkey.name);
            obs_hotkey_load(hotkey.id, keys);
        }
    }
}
}

bool obs_module_load(void)
{
    auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (!mainWindow) return false;
    char *config = obs_module_config_path("settings.ini");
    if (!config) return false;
    const auto path = QString::fromUtf8(config);
    bfree(config);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        blog(LOG_ERROR, "[ScenePad] Could not create settings directory");
        return false;
    }
    dock = new scenepad::Dock(path, mainWindow);
    if (!obs_frontend_add_dock_by_id("ScenePad", obs_module_text("ScenePad"), dock)) {
        delete dock;
        return false;
    }
    for (auto &hotkey : hotkeys)
        hotkey.id = obs_hotkey_register_frontend(hotkey.name, obs_module_text(hotkey.description), hotkeyPressed, &hotkey);
    obs_frontend_add_event_callback(frontendEvent, nullptr);
    signal_handler_connect(obs_get_signal_handler(), "source_rename", sceneRenamed, nullptr);
    obs_frontend_add_save_callback(saveCallback, nullptr);
    blog(LOG_INFO, "[ScenePad] Loaded %s", SCENEPAD_VERSION);
    return true;
}

void obs_module_unload(void)
{
    signal_handler_disconnect(obs_get_signal_handler(), "source_rename", sceneRenamed, nullptr);
    if (dock) dock->shutdown();
    if (!exiting) {
        obs_frontend_remove_event_callback(frontendEvent, nullptr);
        obs_frontend_remove_save_callback(saveCallback, nullptr);
        obs_frontend_remove_dock("ScenePad");
    }
    for (auto &hotkey : hotkeys) {
        if (hotkey.id != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(hotkey.id);
        hotkey.id = OBS_INVALID_HOTKEY_ID;
    }
}

void obs_module_post_load(void)
{
    if (dock) dock->remote()->initialize();
}

MODULE_EXPORT const char *obs_module_name(void) { return obs_module_text("ScenePad"); }
MODULE_EXPORT const char *obs_module_description(void)
{
    return obs_module_text("Description");
}
