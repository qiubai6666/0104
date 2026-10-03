#ifndef OUGAFLASHWINDOW_H
#define OUGAFLASHWINDOW_H
#include "ougaflashtypes.h"
#include <QElapsedTimer>
#include <QWidget>
#include <QTextBlock>
#include <QMap>
#include <functional>
class QLineEdit;
class QComboBox;
class QCheckBox;
class QGroupBox;
class QIcon;
class QTableWidget;
class QTableWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QProgressBar;
class OugaCommandRunner;
class OugaFlashService;
class OugaPreparation;
class OugaRomService;
class OugaFlashOverlay;
class OugaFlashWindow : public QWidget {
  Q_OBJECT
public:
  explicit OugaFlashWindow(QWidget *parent = nullptr,
                           OugaCommandRunner *runner = nullptr,
                           const QString &logDirectory = {});
  bool isBusy() const;

protected:
  void closeEvent(QCloseEvent *event) override;
  void showEvent(QShowEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dropEvent(QDropEvent *event) override;

private:
  struct Page {
    QWidget *widget = nullptr;
    QGroupBox *cloud = nullptr, *files = nullptr, *actions = nullptr;
    QLineEdit *url = nullptr, *payloadFile = nullptr, *folder = nullptr;
    QComboBox *preset = nullptr;
    QCheckBox *wipe = nullptr, *reboot = nullptr;
    QCheckBox *ab = nullptr, *force = nullptr, *onlyFbd = nullptr;
    QCheckBox *fbd = nullptr, *fb = nullptr, *rescue = nullptr;
    QPushButton *extract = nullptr, *repair = nullptr, *arb = nullptr,
                *unpack = nullptr, *start = nullptr;
    QVector<Ouga::Partition> images;
    QString directory;
  };
  enum class Task {
    None,
    Scan,
    PathCheck,
    PayloadDownload,
    PayloadArchive,
    PayloadExtract,
    RescueSelection,
    RescueDownload,
    RescueArchive,
    Super,
    Discover,
    Probe,
    Switch,
    Execute,
    ArbDiscover,
    Arb
  };
  OugaFlashService *m_service = nullptr;
  OugaPreparation *m_prepare = nullptr;
  OugaRomService *m_rom = nullptr;
  Page m_pages[2];
  int m_page = 0;
  Task m_task = Task::None;
  bool m_loading = false, m_stopRequested = false, m_session = false;
  quint64 m_generation = 0;
  QString m_selectedTargetSlot;
  Ouga::Platform m_packagePlatform = Ouga::Platform::Unknown;
  QWidget *m_canvas = nullptr, *m_validation = nullptr;
  QGroupBox *m_settings = nullptr, *m_partitions = nullptr, *m_logs = nullptr;
  QCheckBox *m_full = nullptr, *m_sales = nullptr, *m_validateOn = nullptr,
            *m_validateOff = nullptr, *m_selectAll = nullptr;
  QTableWidget *m_table = nullptr;
  QPlainTextEdit *m_log = nullptr;
  QProgressBar *m_progress = nullptr;
  QPushButton *m_stop = nullptr;
  OugaFlashOverlay *m_overlay = nullptr;
  Ouga::FlashMode m_requestedMode = Ouga::FlashMode::Normal;
  Ouga::Device m_device;
  Ouga::Plan m_plan;
  QStringList m_serials, m_extractNames;
  QString m_payloadSource, m_payloadOutput, m_archiveRoot, m_downloaded;
  bool m_unpackPayload = false;
  QString m_currentArb, m_currentArbSerial, m_logDirectory, m_adbTool;
  QElapsedTimer m_transferClock;
  qint64 m_lastBytes = 0;
  static QIcon referenceIcon(const QString &name);
  static QPushButton *button(const QString &text, const QString &name,
                             QWidget *parent, const QString &tone = {});
  static QLineEdit *entry(const QString &placeholder, const QString &name,
                          QWidget *parent);
  void initializeServices(OugaCommandRunner *runner);
  void inspectPath(const QString &name, const QString &file,
                   std::function<void(Ouga::Partition, const QString &)> ready);
  void setExecutionOverlay(bool active);
  void executionProgress(int writes, int total, const QString &stage);
  void transferProgress(qint64 bytes, qint64 total);
  void buildPage(int index);
  void selectPackage(bool afterSales);
  void layoutCards();
  void updateBusy();
  void updateSelection();
  void log(const QString &message);
  void payloadLogStart(const QString &name);
  void payloadLogFinish(const QString &name, bool success);
  QMap<QString, QTextBlock> m_payloadLogBlocks;
  void endTask(bool success, const QString &message);
  bool continueTask(quint64 generation);
  void load(const QString &path);
  void display(const QVector<Ouga::Partition> &images,
               const QString &directory);
  void tableChanged(QTableWidgetItem *item);
  QString toolPath(const QString &key, const QString &fallback = {}) const;
  QString requireTool(const QString &key, const QString &label,
                      const QString &fallback = {});
  void configureService();
  void extractPayload(bool all);
  void preparePayloadSource();
  void runPayload();
  void preparationFinished(bool success, const QString &message);
  void networkFinished(bool success, const QString &message);
  void startFlash(bool repair = false);
  void prepareFlash();
  void discoverDevice();
  void serviceFinished(bool success, const QString &message);
  void buildAndExecute();
  bool confirm(const Ouga::Plan &plan);
  void requestStop();
  void arbDialog();
  void rescueDialog();
};
#endif
