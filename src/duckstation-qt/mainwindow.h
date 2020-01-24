#pragma once
#include <QtCore/QThread>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <memory>

#include "settingsdialog.h"
#include "ui_mainwindow.h"

class QLabel;

class GameList;
class GameListWidget;
class QtHostInterface;

class MainWindow : public QMainWindow
{
  Q_OBJECT

public:
  explicit MainWindow(QtHostInterface* host_interface);
  ~MainWindow();

private Q_SLOTS:
  void onEmulationStarting();
  void onEmulationStarted();
  void onEmulationStopped();
  void onEmulationPaused(bool paused);
  void toggleFullscreen();
  void switchRenderer();
  void onPerformanceCountersUpdated(float speed, float fps, float vps, float average_frame_time,
                                    float worst_frame_time);

  void onStartDiscActionTriggered();
  void onChangeDiscActionTriggered();
  void onStartBiosActionTriggered();
  void onOpenDirectoryActionTriggered();
  void onExitActionTriggered();
  void onGitHubRepositoryActionTriggered();
  void onIssueTrackerActionTriggered();
  void onAboutActionTriggered();

private:
  void createGameList();
  void setupAdditionalUi();
  void connectSignals();
  void updateEmulationActions(bool starting, bool running);
  void switchToGameListView();
  void switchToEmulationView();
  void doSettings(SettingsDialog::Category category = SettingsDialog::Category::Count);

  Ui::MainWindow m_ui;

  QtHostInterface* m_host_interface = nullptr;

  std::unique_ptr<GameList> m_game_list;
  GameListWidget* m_game_list_widget = nullptr;
  QWidget* m_display_widget = nullptr;

  QLabel* m_status_speed_widget = nullptr;
  QLabel* m_status_fps_widget = nullptr;
  QLabel* m_status_frame_time_widget = nullptr;

  SettingsDialog* m_settings_dialog = nullptr;

  bool m_emulation_running = false;
};
