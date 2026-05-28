// Lyra — Settings dialog.  See settingsdialog.h.

#include "settingsdialog.h"

#include "bands.h"
#include "hl2_discovery.h"
#include "hl2_stream.h"
#include "palettes.h"
#include "prefs.h"
#include "logdialog.h"

#include <functional>
#include "usb_bcd.h"
#include "wdsp_engine.h"
#include "wxservice.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QListWidget>
#include <QListWidgetItem>
#include <QInputDialog>
#include <QUrl>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QSettings>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QFileDialog>
#include "memorystore.h"
#include "eibistore.h"
#include "tci_server.h"
#include "spotstore.h"
#include "metermodel.h"
#include <QDesktopServices>
#include <memory>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace lyra::ui {

SettingsDialog::SettingsDialog(Prefs *prefs, lyra::ipc::HL2Stream *stream,
                               lyra::ipc::HL2Discovery *discovery,
                               UsbBcd *bcd, lyra::dsp::WdspEngine *engine,
                               lyra::wx::WxService *wx, MemoryStore *memory,
                               EibiStore *eibi, TciServer *tci,
                               SpotStore *spots, MeterModel *meter,
                               QWidget *parent)
    : QDialog(parent), prefs_(prefs), stream_(stream),
      discovery_(discovery), bcd_(bcd), engine_(engine), wx_(wx),
      memory_(memory), eibi_(eibi), tci_(tci), spots_(spots),
      meter_(meter) {
    setWindowTitle(tr("Lyra — Settings"));
    resize(640, 520);

    tabs_ = new QTabWidget(this);
    tabs_->addTab(buildVisualsTab(), tr("Visuals"));
    if (engine_) {
        tabs_->addTab(buildAudioTab(), tr("Audio"));
        tabs_->addTab(buildNoiseTab(), tr("Noise"));
    }
    if (meter_) {
        tabs_->addTab(buildMeterTab(), tr("Meter"));
    }
    if (stream_ || discovery_ || bcd_) {
        tabs_->addTab(buildHardwareTab(), tr("Hardware"));
    }
    if (memory_ || eibi_) {
        tabs_->addTab(buildBandsTab(), tr("Bands"));
    }
    if (tci_) {
        tabs_->addTab(buildNetworkTab(), tr("Network"));
    }
    if (wx_) {
        tabs_->addTab(buildWeatherTab(), tr("Weather"));
    }
    // Further tabs (Radio, Audio, DSP, …) are added as those features
    // land — deliberately no empty placeholder tabs.

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    auto *root = new QVBoxLayout(this);
    root->addWidget(tabs_);
    root->addWidget(buttons);
}

QWidget *SettingsDialog::buildAudioTab() {
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    // RX audio output device chooser — moved here from the Audio panel
    // (old-Lyra layout: the device picker lives in Settings; the panel
    // keeps mute + volume).  Index 0 = HL2 onboard codec (AK4951);
    // 1..N = the operator's PC sound devices.
    auto *outCombo = new QComboBox(page);
    outCombo->addItems(engine_->audioOutputDevices());
    outCombo->setCurrentIndex(engine_->audioDeviceIndex());
    connect(outCombo, &QComboBox::activated, engine_,
            [this](int idx) { engine_->setAudioOutputDevice(idx); });
    // Reflect an external change back into the combo.
    connect(engine_, &lyra::dsp::WdspEngine::audioDeviceChanged, outCombo,
            [this, outCombo]() {
                const int i = engine_->audioDeviceIndex();
                if (outCombo->currentIndex() != i) {
                    outCombo->blockSignals(true);
                    outCombo->setCurrentIndex(i);
                    outCombo->blockSignals(false);
                }
            });
    form->addRow(tr("Output"), outCombo);

    auto *note = new QLabel(
        tr("“HL2 audio jack (AK4951)” plays out the radio's "
           "headphone jack (single-crystal, lowest latency).  Choose a PC "
           "device to use the computer's sound card instead.  Volume and "
           "mute stay on the Audio panel."), page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#8fa6ba;"));
    form->addRow(note);

    return page;
}

QWidget *SettingsDialog::buildBandsTab() {
    // A nested tab set so Time Stations + SW Database (EiBi) can join
    // Memory here later, matching old Lyra's Bands → (Memory / Time / SW).
    auto *page = new QWidget(this);
    auto *outer = new QVBoxLayout(page);
    auto *sub = new QTabWidget(page);

    // ---- Memory subtab ----
    if (memory_) {
    auto *mem = new QWidget(sub);
    auto *v = new QVBoxLayout(mem);

    auto *table = new QTableWidget(0, 5, mem);
    table->setHorizontalHeaderLabels(
        {tr("Name"), tr("Freq (MHz)"), tr("Mode"), tr("RX BW (Hz)"), tr("Notes")});
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);

    auto refreshing = std::make_shared<bool>(false);
    auto refresh = [this, table, refreshing]() {
        *refreshing = true;
        const auto &ps = memory_->presets();
        table->setRowCount(ps.size());
        for (int i = 0; i < ps.size(); ++i) {
            const auto &p = ps[i];
            table->setItem(i, 0, new QTableWidgetItem(p.name));
            table->setItem(i, 1, new QTableWidgetItem(
                QString::number(p.freq / 1.0e6, 'f', 6)));
            table->setItem(i, 2, new QTableWidgetItem(p.mode));
            table->setItem(i, 3, new QTableWidgetItem(
                p.rxBw > 0 ? QString::number(p.rxBw) : QString()));
            table->setItem(i, 4, new QTableWidgetItem(p.notes));
        }
        *refreshing = false;
    };
    refresh();
    connect(memory_, &MemoryStore::changed, table, refresh);
    connect(table, &QTableWidget::itemChanged, table,
            [this, table, refreshing](QTableWidgetItem *) {
        if (*refreshing) return;
        const int r = table->currentRow() >= 0 ? table->currentRow() : 0;
        Q_UNUSED(r);
        // Rebuild every row from the table on any edit (simple + robust).
        for (int i = 0; i < table->rowCount() && i < memory_->count(); ++i) {
            MemoryStore::Preset p;
            p.name  = table->item(i, 0) ? table->item(i, 0)->text() : QString();
            p.freq  = qint64(qRound(
                (table->item(i, 1) ? table->item(i, 1)->text().toDouble() : 0.0)
                * 1.0e6));
            p.mode  = table->item(i, 2) ? table->item(i, 2)->text().toUpper()
                                        : QString();
            p.rxBw  = table->item(i, 3) ? table->item(i, 3)->text().toInt() : 0;
            p.notes = table->item(i, 4) ? table->item(i, 4)->text() : QString();
            if (p.freq > 0) memory_->setPreset(i, p);
        }
    });
    v->addWidget(table);

    auto *btnRow = new QHBoxLayout;
    auto *addBtn = new QPushButton(tr("Store current"), mem);
    connect(addBtn, &QPushButton::clicked, mem, [this]() {
        if (!memory_->addCurrent(QString()))
            QMessageBox::information(this, tr("Memory"),
                tr("The memory bank is full (%1 presets).").arg(MemoryStore::kMax));
    });
    auto *delBtn = new QPushButton(tr("Delete"), mem);
    connect(delBtn, &QPushButton::clicked, mem, [this, table]() {
        const int r = table->currentRow();
        if (r >= 0) memory_->remove(r);
    });
    auto *clrBtn = new QPushButton(tr("Clear all"), mem);
    connect(clrBtn, &QPushButton::clicked, mem, [this]() {
        if (QMessageBox::question(this, tr("Memory"),
                tr("Delete all memory presets?")) == QMessageBox::Yes)
            memory_->clearAll();
    });
    auto *impBtn = new QPushButton(tr("Import CSV…"), mem);
    connect(impBtn, &QPushButton::clicked, mem, [this]() {
        const QString fn = QFileDialog::getOpenFileName(this,
            tr("Import memory CSV"), QString(),
            tr("CSV files (*.csv);;All files (*)"));
        if (fn.isEmpty()) return;
        const auto b = QMessageBox::question(this, tr("Import"),
            tr("Merge with existing presets, or replace them?\n\n"
               "Yes = merge,  No = replace."),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (b == QMessageBox::Cancel) return;
        const auto r = memory_->importCsv(fn, b == QMessageBox::No);
        if (!r.error.isEmpty())
            QMessageBox::warning(this, tr("Import"), r.error);
        else
            QMessageBox::information(this, tr("Import"),
                tr("Imported %1 preset(s); skipped %2.").arg(r.added).arg(r.skipped));
    });
    auto *expBtn = new QPushButton(tr("Export CSV…"), mem);
    connect(expBtn, &QPushButton::clicked, mem, [this]() {
        const QString fn = QFileDialog::getSaveFileName(this,
            tr("Export memory CSV"), QStringLiteral("lyra-memory.csv"),
            tr("CSV files (*.csv)"));
        if (fn.isEmpty()) return;
        if (!memory_->exportCsv(fn))
            QMessageBox::warning(this, tr("Export"),
                                 tr("Could not write the file."));
    });
    btnRow->addWidget(addBtn);
    btnRow->addWidget(delBtn);
    btnRow->addWidget(clrBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(impBtn);
    btnRow->addWidget(expBtn);
    v->addLayout(btnRow);

    auto *hint = new QLabel(
        tr("Edit cells directly. Use the “Mem” button on the Band panel to "
           "store the current frequency and recall presets. RX BW blank = "
           "the mode default. Up to %1 presets.").arg(MemoryStore::kMax), mem);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#8fa6ba;"));
    v->addWidget(hint);

    sub->addTab(mem, tr("Memory"));
    }  // if (memory_)

    // ---- SW Database (EiBi) subtab ----
    if (eibi_) {
        auto *sw = new QWidget(sub);
        auto *sv = new QVBoxLayout(sw);

        auto *enable = new QCheckBox(tr("Enable EiBi shortwave overlay"), sw);
        enable->setChecked(eibi_->enabled());
        connect(enable, &QCheckBox::toggled, eibi_,
                [this](bool on) { eibi_->setEnabled(on); });
        sv->addWidget(enable);

        auto *hideOff = new QCheckBox(tr("Hide stations that are off-air now"), sw);
        hideOff->setChecked(eibi_->hideOffAir());
        connect(hideOff, &QCheckBox::toggled, eibi_,
                [this](bool on) { eibi_->setHideOffAir(on); });
        sv->addWidget(hideOff);

        auto *forceAll = new QCheckBox(
            tr("Show in all bands (otherwise hidden inside amateur bands)"), sw);
        forceAll->setChecked(eibi_->forceAllBands());
        connect(forceAll, &QCheckBox::toggled, eibi_,
                [this](bool on) { eibi_->setForceAllBands(on); });
        sv->addWidget(forceAll);

        auto *pwrRow = new QHBoxLayout;
        pwrRow->addWidget(new QLabel(tr("Minimum transmitter power:"), sw));
        auto *pwr = new QComboBox(sw);
        pwr->addItems({tr("Any"), tr("≥ 50 kW"), tr("≥ 100 kW"), tr("≥ 250 kW")});
        pwr->setCurrentIndex(qBound(0, eibi_->minPower(), 3));
        connect(pwr, QOverload<int>::of(&QComboBox::currentIndexChanged), eibi_,
                [this](int i) { eibi_->setMinPower(i); });
        pwrRow->addWidget(pwr);
        pwrRow->addStretch(1);
        sv->addLayout(pwrRow);

        auto *status = new QLabel(eibi_->statusText(), sw);
        status->setWordWrap(true);
        status->setStyleSheet(QStringLiteral("color:#8fa6ba;"));
        connect(eibi_, &EibiStore::changed, status,
                [this, status]() { status->setText(eibi_->statusText()); });

        auto *swBtns = new QHBoxLayout;
        auto *upd = new QPushButton(tr("Update database now"), sw);
        connect(upd, &QPushButton::clicked, sw, [this, upd]() {
            upd->setEnabled(false);
            upd->setText(tr("Downloading…"));
            eibi_->update();
        });
        connect(eibi_, &EibiStore::downloadFinished, upd,
                [this, upd](bool ok, const QString &msg) {
            upd->setEnabled(true);
            upd->setText(tr("Update database now"));
            if (ok) QMessageBox::information(this, tr("EiBi"), msg);
            else    QMessageBox::warning(this, tr("EiBi"), msg);
        });
        auto *loadBtn = new QPushButton(tr("Load CSV file…"), sw);
        connect(loadBtn, &QPushButton::clicked, sw, [this]() {
            const QString fn = QFileDialog::getOpenFileName(this,
                tr("Import EiBi CSV"), QString(),
                tr("CSV files (*.csv);;All files (*)"));
            if (fn.isEmpty()) return;
            if (eibi_->importFile(fn))
                QMessageBox::information(this, tr("EiBi"), eibi_->statusText());
            else
                QMessageBox::warning(this, tr("EiBi"),
                    tr("Could not read or parse that file. EiBi CSVs are "
                       "semicolon-delimited (KHZ;TIM;DAY;ITU;STN;…)."));
        });
        auto *page2 = new QPushButton(tr("Open EiBi website…"), sw);
        connect(page2, &QPushButton::clicked, sw, []() {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://eibispace.de/")));
        });
        swBtns->addWidget(upd);
        swBtns->addWidget(loadBtn);
        swBtns->addWidget(page2);
        swBtns->addStretch(1);
        sv->addLayout(swBtns);
        sv->addWidget(status);

        auto *swHint = new QLabel(
            tr("EiBi (eibispace.de) is a free shortwave-broadcast schedule. "
               "Click “Update database now” to download the current season "
               "(~3 MB). If the download fails (their TLS certificate expires "
               "from time to time), download the sked-<season>.csv yourself "
               "from the EiBi site and use “Load CSV file…”. Active stations "
               "then appear on the panadapter; cyan = on-air now, grey = "
               "scheduled but off-air. Click a station to tune it in AM."), sw);
        swHint->setWordWrap(true);
        swHint->setStyleSheet(QStringLiteral("color:#8fa6ba;"));
        sv->addWidget(swHint);
        sv->addStretch(1);

        sub->addTab(sw, tr("SW Database"));
    }

    outer->addWidget(sub);
    return page;
}

QWidget *SettingsDialog::buildNetworkTab() {
    // Two-column layout (task #23, continuation of the Hardware/Visuals
    // multi-column work): TCI server on the left, DX-cluster spots on
    // the right.  The TCI status label hangs directly under the TCI
    // group (left column).  The descriptive hint label spans the full
    // width below both columns.  Same gutter (28 px) + same intra-form
    // row spacing (10 px) as the other refactored tabs.
    auto *page = new QWidget(this);
    auto *v = new QVBoxLayout(page);

    auto *cols   = new QWidget(page);
    auto *colsHb = new QHBoxLayout(cols);
    colsHb->setContentsMargins(0, 0, 0, 0);
    colsHb->setSpacing(28);
    auto *leftCol  = new QWidget(cols);
    auto *leftVb   = new QVBoxLayout(leftCol);
    leftVb->setContentsMargins(0, 0, 0, 0);
    auto *rightCol = new QWidget(cols);
    auto *rightVb  = new QVBoxLayout(rightCol);
    rightVb->setContentsMargins(0, 0, 0, 0);
    colsHb->addWidget(leftCol,  1);
    colsHb->addWidget(rightCol, 1);
    v->addWidget(cols);

    auto *grp = new QGroupBox(tr("TCI server"), leftCol);
    auto *form = new QFormLayout(grp);
    form->setVerticalSpacing(10);

    auto *host = new QLineEdit(tci_->bindHost(), grp);
    host->setPlaceholderText(QStringLiteral("127.0.0.1"));
    form->addRow(tr("Bind address:"), host);

    auto *port = new QSpinBox(grp);
    port->setRange(1, 65535);
    port->setValue(tci_->port());
    form->addRow(tr("Port:"), port);

    auto *rate = new QSpinBox(grp);
    rate->setRange(0, 1000);
    rate->setSuffix(tr(" ms"));
    rate->setValue(tci_->rateLimitMs());
    rate->setToolTip(tr("Minimum interval between repeated broadcasts of the "
                        "same parameter (0 = unlimited)."));
    form->addRow(tr("Rate limit:"), rate);
    connect(rate, QOverload<int>::of(&QSpinBox::valueChanged), tci_,
            [this](int ms) { tci_->setRateLimitMs(ms); });

    auto *initial = new QCheckBox(tr("Send full state to clients on connect"), grp);
    initial->setChecked(tci_->sendInitialState());
    connect(initial, &QCheckBox::toggled, tci_,
            [this](bool on) { tci_->setSendInitialState(on); });
    form->addRow(QString(), initial);

    auto *cwlu = new QCheckBox(tr("Add “CW” to the modulations list (CWL/CWU alias)"), grp);
    cwlu->setChecked(tci_->cwluBecomesCw());
    connect(cwlu, &QCheckBox::toggled, tci_,
            [this](bool on) { tci_->setCwluBecomesCw(on); });
    form->addRow(QString(), cwlu);

    auto *emuProto = new QCheckBox(tr("Emulate ExpertSDR3 (report protocol "
                                      "name “ExpertSDR3”)"), grp);
    emuProto->setChecked(tci_->emulateExpertSdr3());
    emuProto->setToolTip(tr("Some clients only recognise ExpertSDR3 / SunSDR "
                            "rigs. Enable these to masquerade as one."));
    connect(emuProto, &QCheckBox::toggled, tci_,
            [this](bool on) { tci_->setEmulateExpertSdr3(on); });
    form->addRow(QString(), emuProto);

    auto *emuDev = new QCheckBox(tr("Emulate SunSDR2 PRO (report device name "
                                    "“SunSDR2PRO”)"), grp);
    emuDev->setChecked(tci_->emulateSunSdr2());
    connect(emuDev, &QCheckBox::toggled, tci_,
            [this](bool on) { tci_->setEmulateSunSdr2(on); });
    form->addRow(QString(), emuDev);

    auto *enable = new QCheckBox(tr("TCI server running"), grp);
    enable->setChecked(tci_->running());
    form->addRow(QString(), enable);

    auto *status = new QLabel(grp);
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("color:#8fa6ba;"));
    auto refreshStatus = [this, status]() {
        if (tci_->running())
            status->setText(tr("Listening — %1 client(s) connected.")
                                .arg(tci_->clientCount()));
        else
            status->setText(tr("Stopped."));
    };
    refreshStatus();
    connect(tci_, &TciServer::runningChanged, status, refreshStatus);
    connect(tci_, &TciServer::clientCountChanged, status, refreshStatus);

    // Apply host/port live before (re)starting; editing them while running
    // rebinds automatically inside TciServer.
    connect(host, &QLineEdit::editingFinished, tci_,
            [this, host]() { tci_->setBindHost(host->text().trimmed()); });
    connect(port, QOverload<int>::of(&QSpinBox::valueChanged), tci_,
            [this](int p) { tci_->setPort(p); });
    connect(enable, &QCheckBox::toggled, tci_,
            [this, host, port](bool on) {
        if (on) {
            tci_->setBindHost(host->text().trimmed());
            tci_->setPort(port->value());
        }
        tci_->setEnabled(on);
    });
    connect(tci_, &TciServer::runningChanged, enable,
            [this, enable]() {
        QSignalBlocker b(enable);
        enable->setChecked(tci_->running());
    });

    leftVb->addWidget(grp);
    leftVb->addWidget(status);
    leftVb->addStretch(1);

    // DX-cluster spot display options.
    if (spots_) {
        auto *sg = new QGroupBox(tr("DX-cluster spots"), rightCol);
        auto *sf = new QFormLayout(sg);
        sf->setVerticalSpacing(10);

        auto *showSpots = new QCheckBox(tr("Show spots on the panadapter"), sg);
        showSpots->setChecked(spots_->showSpots());
        connect(showSpots, &QCheckBox::toggled, spots_,
                [this](bool on) { spots_->setShowSpots(on); });
        sf->addRow(QString(), showSpots);

        auto *maxSp = new QSpinBox(sg);
        maxSp->setRange(1, 5000);
        maxSp->setValue(spots_->maxSpots());
        connect(maxSp, QOverload<int>::of(&QSpinBox::valueChanged), spots_,
                [this](int n) { spots_->setMaxSpots(n); });
        sf->addRow(tr("Max spots:"), maxSp);

        auto *life = new QSpinBox(sg);
        life->setRange(0, 1440);                 // up to 24 h, in minutes
        life->setSuffix(tr(" min"));
        life->setSpecialValueText(tr("never expire"));
        life->setValue(spots_->lifetimeSec() / 60);
        connect(life, QOverload<int>::of(&QSpinBox::valueChanged), spots_,
                [this](int m) { spots_->setLifetimeSec(m * 60); });
        sf->addRow(tr("Spot lifetime:"), life);

        auto *hlOwn = new QCheckBox(tr("Highlight my callsign when spotted"), sg);
        hlOwn->setChecked(spots_->highlightOwn());
        connect(hlOwn, &QCheckBox::toggled, spots_,
                [this](bool on) { spots_->setHighlightOwn(on); });
        sf->addRow(QString(), hlOwn);

        // Colour swatch button that opens a picker for the highlight colour.
        auto *hlColor = new QPushButton(sg);
        hlColor->setFixedWidth(64);
        auto paintSwatch = [hlColor](const QString &hex) {
            hlColor->setStyleSheet(
                QStringLiteral("background:%1;border:1px solid #5a6573;").arg(hex));
        };
        paintSwatch(spots_->highlightColor());
        connect(hlColor, &QPushButton::clicked, sg, [this, paintSwatch]() {
            const QColor c = QColorDialog::getColor(
                QColor(spots_->highlightColor()), this,
                tr("My-callsign highlight colour"));
            if (c.isValid()) {
                const QString hex = c.name();
                spots_->setHighlightColor(hex);
                paintSwatch(hex);
            }
        });
        sf->addRow(tr("Highlight colour:"), hlColor);

        auto *notify = new QCheckBox(tr("Pop up a notification when I'm spotted"), sg);
        notify->setChecked(spots_->notifyOwn());
        connect(notify, &QCheckBox::toggled, spots_,
                [this](bool on) { spots_->setNotifyOwn(on); });
        sf->addRow(QString(), notify);

        auto *cooldown = new QSpinBox(sg);
        cooldown->setRange(0, 240);
        cooldown->setSuffix(tr(" min"));
        cooldown->setSpecialValueText(tr("every time"));
        cooldown->setValue(spots_->notifyCooldownMin());
        cooldown->setToolTip(tr("Don't notify again about your callsign until "
                                "this long has passed (avoids cluster spam)."));
        connect(cooldown, QOverload<int>::of(&QSpinBox::valueChanged), spots_,
                [this](int m) { spots_->setNotifyCooldownMin(m); });
        sf->addRow(tr("Re-notify after:"), cooldown);

        auto *clearSpots = new QPushButton(tr("Clear spots now"), sg);
        connect(clearSpots, &QPushButton::clicked, spots_,
                [this]() { spots_->clearAll(); });
        sf->addRow(QString(), clearSpots);

        rightVb->addWidget(sg);
        rightVb->addStretch(1);
    }

    auto *hint = new QLabel(
        tr("Lets logging and cluster software (SDRLogger+, Log4OM, WSJT-X, …) "
           "control and read Lyra over a WebSocket — frequency, mode, "
           "start/stop, and DX-cluster spots. The de-facto default port is "
           "<b>50001</b>; bind to 127.0.0.1 for same-PC apps, or 0.0.0.0 to "
           "allow other machines on your network. Lyra is receive-only, so "
           "transmit commands are acknowledged inactive.<br><br>"
           "Implements the public TCI v1.9 / v2.0 specification "
           "(© EESDR Expert Electronics)."), page);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#8fa6ba;"));
    v->addWidget(hint);
    v->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildHardwareTab() {
    // Two-column layout (task #22, operator-flagged 3× — the Hardware
    // tab had grown to ~10 groups + 7 inline form rows + the Transmit
    // group, overflowing the dialog height).  Wider is easier to scan
    // than scrolling.  Implementation is intentionally minimal: two
    // sibling QFormLayouts inside a horizontal split; the local `form`
    // pointer is repointed midway so the existing `form->addRow(...)`
    // call sites stay byte-identical.  No widget construction changes,
    // no signal/slot re-wires, no QSettings churn.
    //
    // Layout (top-down, left then right):
    //   LEFT  : Operator/Station, Band plan, Band panel, Diagnostics
    //   RIGHT : Radio (HL2 discovery), external-HW + BCD inline rows,
    //           Transmit (TX safety + PA enable)
    auto *page  = new QWidget(this);
    auto *outer = new QHBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(28);

    auto *leftCol  = new QWidget(page);
    auto *leftForm = new QFormLayout(leftCol);
    leftForm->setContentsMargins(0, 0, 0, 0);
    leftForm->setVerticalSpacing(10);
    outer->addWidget(leftCol, 1);

    auto *rightCol  = new QWidget(page);
    auto *rightForm = new QFormLayout(rightCol);
    rightForm->setContentsMargins(0, 0, 0, 0);
    rightForm->setVerticalSpacing(10);
    outer->addWidget(rightCol, 1);

    // The body below uses `form->addRow(...)` extensively.  Start it
    // pointing at the left column; switch to the right column just
    // before the Radio group lower down.  Avoids touching ~500 lines
    // of widget code.
    QFormLayout *form = leftForm;

    // --- Operator / Station ---
    // Callsign + Maidenhead grid (+ manual lat/lon fallback).  The
    // location drives the weather sources + solar panel; callsign feeds
    // TCI/logging.  Grid wins over manual when valid.
    {
        auto *grp = new QGroupBox(tr("Operator / Station"), page);
        auto *g = new QGridLayout(grp);
        g->setColumnStretch(1, 1);

        g->addWidget(new QLabel(tr("Callsign"), grp), 0, 0);
        auto *call = new QLineEdit(prefs_->callsign(), grp);
        call->setFixedWidth(140);
        call->setPlaceholderText(tr("e.g. N8SDR"));
        connect(call, &QLineEdit::editingFinished, grp,
                [this, call]() { prefs_->setCallsign(call->text()); });
        connect(prefs_, &Prefs::callsignChanged, call, [this, call]() {
            if (call->text() != prefs_->callsign()) call->setText(prefs_->callsign());
        });
        g->addWidget(call, 0, 1);

        g->addWidget(new QLabel(tr("Grid square"), grp), 1, 0);
        auto *grid = new QLineEdit(prefs_->gridSquare(), grp);
        grid->setFixedWidth(140);
        grid->setPlaceholderText(tr("e.g. EN82"));
        g->addWidget(grid, 1, 1);

        g->addWidget(new QLabel(tr("Computed lat/lon"), grp), 2, 0);
        auto *comp = new QLabel(grp);
        comp->setStyleSheet(QStringLiteral("QLabel{color:#8fa6ba;}"));
        g->addWidget(comp, 2, 1);

        g->addWidget(new QLabel(tr("Manual lat (°)"), grp), 3, 0);
        auto *latE = new QLineEdit(grp);
        latE->setFixedWidth(140);
        latE->setPlaceholderText(tr("e.g. 41.5  (fallback if no grid)"));
        if (!std::isnan(prefs_->manualLat()))
            latE->setText(QString::number(prefs_->manualLat(), 'f', 4));
        g->addWidget(latE, 3, 1);

        g->addWidget(new QLabel(tr("Manual lon (°)"), grp), 4, 0);
        auto *lonE = new QLineEdit(grp);
        lonE->setFixedWidth(140);
        lonE->setPlaceholderText(tr("e.g. -83.6"));
        if (!std::isnan(prefs_->manualLon()))
            lonE->setText(QString::number(prefs_->manualLon(), 'f', 4));
        g->addWidget(lonE, 4, 1);

        auto refreshComputed = [this, comp]() {
            double la = 0, lo = 0;
            if (prefs_->operatorLocation(&la, &lo))
                comp->setText(QStringLiteral("%1, %2")
                                  .arg(la, 0, 'f', 4).arg(lo, 0, 'f', 4));
            else
                comp->setText(tr("— not set —"));
        };
        refreshComputed();
        connect(prefs_, &Prefs::locationChanged, comp, refreshComputed);

        connect(grid, &QLineEdit::editingFinished, grp, [this, grid]() {
            prefs_->setGridSquare(grid->text());
            if (grid->text() != prefs_->gridSquare())   // reflect normalization
                grid->setText(prefs_->gridSquare());
        });
        connect(prefs_, &Prefs::gridSquareChanged, grid, [this, grid]() {
            if (grid->text() != prefs_->gridSquare()) grid->setText(prefs_->gridSquare());
        });

        auto applyManual = [this, latE, lonE]() {
            const QString lt = latE->text().trimmed();
            const QString ln = lonE->text().trimmed();
            if (lt.isEmpty() && ln.isEmpty()) {       // clear override
                prefs_->setManualLatLon(std::nan(""), std::nan(""));
                return;
            }
            bool ok1 = false, ok2 = false;
            const double la = lt.toDouble(&ok1);
            const double lo = ln.toDouble(&ok2);
            if (ok1 && ok2) prefs_->setManualLatLon(la, lo);
        };
        connect(latE, &QLineEdit::editingFinished, grp, applyManual);
        connect(lonE, &QLineEdit::editingFinished, grp, applyManual);

        grp->setToolTip(tr("Your location drives the weather alerts and the "
                           "solar panel. The grid square is used when valid; "
                           "manual lat/lon is the fallback."));
        form->addRow(grp);
    }

    // --- Band plan (region for panadapter overlays) ---
    {
        auto *grp = new QGroupBox(tr("Band plan"), page);
        auto *g = new QGridLayout(grp);
        g->setColumnStretch(1, 1);
        g->addWidget(new QLabel(tr("Region"), grp), 0, 0);
        auto *region = new QComboBox(grp);
        region->addItem(tr("US / IARU Region 2"),      QStringLiteral("US"));
        region->addItem(tr("IARU Region 1 (EU / Africa)"), QStringLiteral("IARU_R1"));
        region->addItem(tr("IARU Region 3 (Asia-Pacific)"), QStringLiteral("IARU_R3"));
        region->addItem(tr("None"),                    QStringLiteral("NONE"));
        region->setCurrentIndex(std::max(0, region->findData(prefs_->bandPlanRegion())));
        region->setToolTip(tr("Region for the amateur band-plan overlay "
                              "(sub-band segments, landmarks, out-of-band "
                              "advisories) on the panadapter."));
        connect(region, &QComboBox::currentIndexChanged, grp, [this, region](int) {
            prefs_->setBandPlanRegion(region->currentData().toString());
        });
        connect(prefs_, &Prefs::bandPlanRegionChanged, grp, [this, region]() {
            const int i = region->findData(prefs_->bandPlanRegion());
            if (i >= 0 && region->currentIndex() != i) region->setCurrentIndex(i);
        });
        g->addWidget(region, 0, 1);

        // Overlay-layer toggles (the panadapter top strip).  Each mirrors
        // a Prefs bool; Region = None hides everything regardless.
        auto addLayer = [this, g](int row, const QString &text,
                                  const QString &tip, bool checked,
                                  std::function<void(bool)> set,
                                  QWidget *parent) -> QCheckBox * {
            auto *ck = new QCheckBox(text, parent);
            ck->setChecked(checked);
            ck->setToolTip(tip);
            QObject::connect(ck, &QCheckBox::toggled, parent, set);
            g->addWidget(ck, row, 0, 1, 2);
            return ck;
        };
        auto *segCk = addLayer(1, tr("Sub-band segments"),
            tr("Coloured CW / digital / SSB / FM sub-band strip."),
            prefs_->bandPlanSegments(),
            [this](bool v){ prefs_->setBandPlanSegments(v); }, grp);
        connect(prefs_, &Prefs::bandPlanSegmentsChanged, segCk, [this, segCk]() {
            if (segCk->isChecked() != prefs_->bandPlanSegments())
                segCk->setChecked(prefs_->bandPlanSegments());
        });
        auto *landCk = addLayer(2, tr("Digital landmarks (FT8 / FT4 / WSPR / PSK)"),
            tr("Markers at the common digital-mode calling frequencies. "
               "Click one to tune there."),
            prefs_->bandPlanLandmarks(),
            [this](bool v){ prefs_->setBandPlanLandmarks(v); }, grp);
        connect(prefs_, &Prefs::bandPlanLandmarksChanged, landCk, [this, landCk]() {
            if (landCk->isChecked() != prefs_->bandPlanLandmarks())
                landCk->setChecked(prefs_->bandPlanLandmarks());
        });
        auto *beaconCk = addLayer(3, tr("NCDXF beacon markers"),
            tr("The 5 NCDXF International Beacon Project frequencies."),
            prefs_->bandPlanBeacons(),
            [this](bool v){ prefs_->setBandPlanBeacons(v); }, grp);
        connect(prefs_, &Prefs::bandPlanBeaconsChanged, beaconCk, [this, beaconCk]() {
            if (beaconCk->isChecked() != prefs_->bandPlanBeacons())
                beaconCk->setChecked(prefs_->bandPlanBeacons());
        });
        auto *edgeCk = addLayer(4, tr("Band-edge warning lines"),
            tr("Red dashed lines at each band's edges."),
            prefs_->bandPlanEdges(),
            [this](bool v){ prefs_->setBandPlanEdges(v); }, grp);
        connect(prefs_, &Prefs::bandPlanEdgesChanged, edgeCk, [this, edgeCk]() {
            if (edgeCk->isChecked() != prefs_->bandPlanEdges())
                edgeCk->setChecked(prefs_->bandPlanEdges());
        });

        // Per-mode segment colours — a swatch button per kind (click to
        // recolour; picking the default clears the override).
        g->addWidget(new QLabel(tr("Segment colors"), grp), 5, 0);
        auto *colorRow = new QWidget(grp);
        auto *ch = new QHBoxLayout(colorRow);
        ch->setContentsMargins(0, 0, 0, 0);
        ch->setSpacing(6);
        const QStringList kinds = {QStringLiteral("CW"), QStringLiteral("DIG"),
                                   QStringLiteral("SSB"), QStringLiteral("FM")};
        for (const QString &k : kinds) {
            auto *btn = new QPushButton(k, colorRow);
            btn->setFixedWidth(48);
            btn->setCursor(Qt::PointingHandCursor);
            auto applySwatch = [this, btn, k]() {
                btn->setStyleSheet(QStringLiteral(
                    "QPushButton{background:%1;color:#ffffff;font-weight:700;"
                    "border:1px solid #2a3a4a;border-radius:3px;padding:3px;}")
                    .arg(prefs_->bandPlanColor(k)));
            };
            applySwatch();
            connect(btn, &QPushButton::clicked, colorRow, [this, k]() {
                const QColor picked = QColorDialog::getColor(
                    QColor(prefs_->bandPlanColor(k)), this,
                    tr("%1 segment colour").arg(k));
                if (picked.isValid())
                    prefs_->setBandPlanColor(k, picked.name());
            });
            connect(prefs_, &Prefs::bandPlanColorsChanged, btn, applySwatch);
            ch->addWidget(btn);
        }
        auto *resetColors = new QPushButton(tr("Reset"), colorRow);
        resetColors->setToolTip(tr("Restore the default segment colors."));
        connect(resetColors, &QPushButton::clicked, colorRow, [this, kinds]() {
            for (const QString &k : kinds)
                prefs_->setBandPlanColor(k, QString());   // "" → revert to default
        });
        ch->addWidget(resetColors);
        ch->addStretch(1);
        g->addWidget(colorRow, 5, 1);

        form->addRow(grp);
    }

    // --- Band panel options ---
    {
        auto *grp = new QGroupBox(tr("Band panel"), page);
        auto *v = new QVBoxLayout(grp);
        auto *cb = new QCheckBox(
            tr("Show 11m / CB band row (26.965–27.405 MHz)"), grp);
        cb->setChecked(prefs_->cbBandEnabled());
        cb->setToolTip(tr("Adds a CB / 11-meter row to the Band panel for "
                          "Citizens Band listening."));
        connect(cb, &QCheckBox::toggled, grp,
                [this](bool on) { prefs_->setCbBandEnabled(on); });
        connect(prefs_, &Prefs::cbBandEnabledChanged, cb, [this, cb]() {
            if (cb->isChecked() != prefs_->cbBandEnabled())
                cb->setChecked(prefs_->cbBandEnabled());
        });
        v->addWidget(cb);
        form->addRow(grp);
    }

    // --- Diagnostics (debug logging) ---
    // Release builds run without a console window. This toggle turns on
    // verbose capture so the in-app Log viewer / log file have the full
    // detail we need when chasing a problem.
    {
        auto *grp = new QGroupBox(tr("Diagnostics"), page);
        auto *v = new QVBoxLayout(grp);

        auto *dbg = new QCheckBox(tr("Enable verbose debug logging"), grp);
        dbg->setChecked(prefs_->debugLogging());
        dbg->setToolTip(tr("Capture detailed Debug/Info messages (not just "
                           "warnings/errors). Leave off for normal use; turn "
                           "on to reproduce and capture a problem."));
        connect(dbg, &QCheckBox::toggled, grp,
                [this](bool on) { prefs_->setDebugLogging(on); });
        connect(prefs_, &Prefs::debugLoggingChanged, dbg, [this, dbg]() {
            if (dbg->isChecked() != prefs_->debugLogging())
                dbg->setChecked(prefs_->debugLogging());
        });
        v->addWidget(dbg);

        auto *viewRow = new QHBoxLayout;
        auto *viewBtn = new QPushButton(tr("View Log…"), grp);
        viewBtn->setToolTip(tr("Open the diagnostic log — Copy or Save it to "
                              "send to us if something misbehaves."));
        connect(viewBtn, &QPushButton::clicked, grp, [this]() {
            auto *dlg = new LogDialog(prefs_, this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->show();
            dlg->raise();
        });
        viewRow->addWidget(viewBtn);
        viewRow->addStretch(1);
        v->addLayout(viewRow);

        form->addRow(grp);
    }

    // ---- Switch to the right column (see scaffolding comment at the
    // top of this function).  Everything below — Radio (HL2 discovery
    // + connect), the external-HW / BCD inline form rows, and the
    // Transmit group — lands in the right column.  The right column
    // is intentionally taller (Radio's discovery list + 7 BCD/filter
    // rows + Transmit add up); the left column gets a vertical spacer
    // at the end so its groups stay top-anchored. -----------------
    auto *leftBottomSpacer = new QWidget(leftCol);
    leftBottomSpacer->setSizePolicy(QSizePolicy::Preferred,
                                    QSizePolicy::Expanding);
    form->addRow(leftBottomSpacer);
    form = rightForm;

    // --- Radio (HL2 discovery + connect) ---
    // LAN scan + found-radios list + Open/Close.  Moved here from the
    // old central dev scaffold (Log / wire-stats banner / Found-radios):
    // discovery is a hardware/connection concern, and relocating it frees
    // the main-window real estate for actual control panels.
    if (discovery_ || stream_) {
        auto *radioBox = new QGroupBox(tr("Radio (Hermes Lite 2 / 2+)"), page);
        auto *rv = new QVBoxLayout(radioBox);

        auto *status = new QLabel(tr("Click \"Discover\" to scan the LAN."),
                                  radioBox);
        status->setWordWrap(true);
        status->setStyleSheet(QStringLiteral("QLabel{color:#8fa6ba;}"));

        auto *list = new QListWidget(radioBox);
        list->setMinimumHeight(96);
        list->setToolTip(tr("Radios found on the LAN. Select one, then Open."));

        // Append a found radio (de-duped by IP), stashing every field on
        // the item so Open can remember the full record for auto-connect.
        auto addRadio = [list](const QString &ip, const QString &mac,
                               const QString &board, int codeVer, int betaVer,
                               bool busy, int numRxs) {
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole).toString() == ip)
                    return;
            auto *it = new QListWidgetItem(
                tr("%1  —  %2  (gw v%3.%4, %5 rx)%6")
                    .arg(ip, board).arg(codeVer).arg(betaVer).arg(numRxs)
                    .arg(busy ? tr("  [BUSY]") : QString()), list);
            it->setData(Qt::UserRole,     ip);
            it->setData(Qt::UserRole + 1, mac);
            it->setData(Qt::UserRole + 2, board);
            it->setData(Qt::UserRole + 3, codeVer);
            it->setData(Qt::UserRole + 4, betaVer);
            it->setData(Qt::UserRole + 5, busy);
            it->setData(Qt::UserRole + 6, numRxs);
        };

        // Show the remembered radio on open so the operator sees what
        // auto-connect attached to.
        if (discovery_) {
            const QVariantMap r = discovery_->savedRadio();
            if (!r.value(QStringLiteral("ip")).toString().isEmpty()) {
                addRadio(r.value(QStringLiteral("ip")).toString(),
                         r.value(QStringLiteral("mac")).toString(),
                         r.value(QStringLiteral("boardName")).toString(),
                         r.value(QStringLiteral("codeVersion")).toInt(),
                         r.value(QStringLiteral("betaVersion")).toInt(),
                         r.value(QStringLiteral("busy")).toBool(),
                         r.value(QStringLiteral("numRxs")).toInt());
            }
        }

        auto *scanBtn  = new QPushButton(tr("Discover"), radioBox);
        auto *openBtn  = new QPushButton(tr("Open"), radioBox);
        auto *closeBtn = new QPushButton(tr("Close"), radioBox);
        auto *btnRow = new QHBoxLayout();
        btnRow->addWidget(scanBtn);
        btnRow->addWidget(openBtn);
        btnRow->addWidget(closeBtn);
        btnRow->addStretch(1);

        rv->addWidget(status);
        rv->addWidget(list);
        rv->addLayout(btnRow);

        auto refresh = [this, scanBtn, openBtn, closeBtn, list, status]() {
            const bool running = stream_ && stream_->isRunning();
            scanBtn->setEnabled(discovery_ && !running);
            openBtn->setEnabled(stream_ && !running &&
                                list->currentItem() != nullptr);
            closeBtn->setEnabled(running);
            if (running && stream_)
                status->setText(tr("Connected to %1").arg(stream_->targetIp()));
        };
        refresh();
        connect(list, &QListWidget::currentRowChanged, radioBox,
                [refresh](int) { refresh(); });

        if (discovery_) {
            connect(scanBtn, &QPushButton::clicked, discovery_,
                    [this, status]() {
                        status->setText(tr("Scanning…"));
                        discovery_->scan(1.5, 2);
                    });
            connect(discovery_, &lyra::ipc::HL2Discovery::radioFound, radioBox,
                    [addRadio](QString ip, QString mac, QString board,
                               int cv, int bv, bool busy, int nr) {
                        addRadio(ip, mac, board, cv, bv, busy, nr);
                    });
            connect(discovery_, &lyra::ipc::HL2Discovery::scanFinished, radioBox,
                    [status](int count) {
                        status->setText(
                            tr("Scan complete — %1 radio(s) found.").arg(count));
                    });
        } else {
            scanBtn->setEnabled(false);
        }

        if (stream_) {
            connect(openBtn, &QPushButton::clicked, radioBox, [this, list]() {
                auto *it = list->currentItem();
                if (!it) return;
                const QString ip = it->data(Qt::UserRole).toString();
                if (discovery_) {
                    discovery_->rememberRadio(
                        ip, it->data(Qt::UserRole + 1).toString(),
                        it->data(Qt::UserRole + 2).toString(),
                        it->data(Qt::UserRole + 3).toInt(),
                        it->data(Qt::UserRole + 4).toInt(),
                        it->data(Qt::UserRole + 5).toBool(),
                        it->data(Qt::UserRole + 6).toInt());
                }
                stream_->open(ip);
            });
            connect(closeBtn, &QPushButton::clicked, stream_,
                    [this]() { stream_->close(); });
            connect(stream_, &lyra::ipc::HL2Stream::runningChanged, radioBox,
                    [refresh]() { refresh(); });
        } else {
            openBtn->setEnabled(false);
            closeBtn->setEnabled(false);
        }

        form->addRow(radioBox);
    }

    // --- External filter board (N2ADR) ---
    // Drives the HL2 OC outputs (J16) per band so the board's RX
    // band-pass + 3 MHz HPF relays follow the band — the front-end
    // protection against strong out-of-band signals (e.g. a nearby AM
    // broadcaster).  Default OFF; harmless if no board is connected.
    auto *fb = new QCheckBox(
        tr("Enable external filter board (N2ADR / compatible)"), page);
    fb->setChecked(stream_ && stream_->filterBoardEnabled());
    fb->setToolTip(tr("Switches the HL2 open-collector outputs per band "
                      "to drive an external band-pass filter board.\n"
                      "Leave off if you don't have one (harmless either "
                      "way — the OC pins just drive nothing)."));
    if (stream_) {
        connect(fb, &QCheckBox::toggled, stream_,
                &lyra::ipc::HL2Stream::setFilterBoardEnabled);
        connect(stream_, &lyra::ipc::HL2Stream::filterBoardChanged, fb,
                [fb](bool on) { if (fb->isChecked() != on) fb->setChecked(on); });
    }
    form->addRow(tr("Filter board"), fb);

    // Live OC-pin readout (which J16 pins are currently driven).
    auto *oc = new QLabel(page);
    auto setOcText = [oc](int pattern) {
        oc->setText(tr("Pins: %1").arg(lyra::ocPatternText(pattern)));
    };
    setOcText(stream_ ? stream_->ocBits() : 0);
    oc->setStyleSheet(QStringLiteral(
        "QLabel{color:#8fa6ba;font-family:Consolas;}"));
    if (stream_) {
        connect(stream_, &lyra::ipc::HL2Stream::ocBitsChanged, oc, setOcText);
    }
    form->addRow(tr("OC outputs"), oc);

    // --- Auto-LNA (overload-triggered front-end protection) ---
    // On sustained ADC overload the LNA backs off; when the band clears
    // it creeps gain back up, riding the overload edge.  The master
    // on/off lives on the DSP+Audio panel (Row 1 "Auto"); these are the
    // tuning knobs (mirrors the reference Ant/Filters Auto-ATT options).
    {
        auto *undo = new QCheckBox(
            tr("Creep gain back up when the band clears (Undo)"), page);
        undo->setChecked(stream_ && stream_->autoLnaUndo());
        undo->setToolTip(tr(
            "On: after backing off on overload, Auto-LNA raises the gain "
            "1 dB per hold interval once clipping stops.\n"
            "Off: Auto-LNA only ever backs the gain off and holds it there."));
        if (stream_) {
            connect(undo, &QCheckBox::toggled, stream_,
                    &lyra::ipc::HL2Stream::setAutoLnaUndo);
            connect(stream_, &lyra::ipc::HL2Stream::autoLnaChanged, undo,
                    [this, undo]() {
                        const bool on = stream_->autoLnaUndo();
                        if (undo->isChecked() != on) undo->setChecked(on);
                    });
        }
        form->addRow(tr("Auto-LNA"), undo);

        auto *hold = new QSpinBox(page);
        hold->setRange(1, 60);
        hold->setSuffix(tr(" s"));
        hold->setValue(stream_ ? stream_->autoLnaHoldSec() : 4);
        hold->setToolTip(tr(
            "How long to wait after clipping stops before creeping the gain "
            "back up — and the interval between each +1 dB step."));
        if (stream_) {
            connect(hold, qOverload<int>(&QSpinBox::valueChanged), stream_,
                    &lyra::ipc::HL2Stream::setAutoLnaHoldSec);
            connect(stream_, &lyra::ipc::HL2Stream::autoLnaChanged, hold,
                    [this, hold]() {
                        const int s = stream_->autoLnaHoldSec();
                        if (hold->value() != s) hold->setValue(s);
                    });
        }
        form->addRow(tr("Hold time"), hold);
    }

    // --- USB-BCD output (external linear-amp band switching) ---
    // FTDI cable that outputs the Yaesu BCD band code so an amp's
    // auto-bandswitch follows Lyra.  Default OFF; ⚠ the wrong code at
    // power can route TX through the wrong filter — verify per band.
    if (bcd_) {
        if (!bcd_->available()) {
            auto *na = new QLabel(
                tr("FTDI driver (ftd2xx.dll) not found — install the FTDI "
                   "D2XX driver to use USB-BCD."), page);
            na->setWordWrap(true);
            na->setStyleSheet(QStringLiteral("QLabel{color:#8a9aac;}"));
            form->addRow(tr("USB-BCD"), na);
        } else {
            auto *en = new QCheckBox(
                tr("Enable USB-BCD amp band output"), page);
            en->setChecked(bcd_->enabled());
            en->setToolTip(tr("Outputs the Yaesu BCD band code on an FTDI "
                              "cable so a linear amp follows the band.\n"
                              "⚠ Verify wiring + low-power test per band "
                              "before keying at full power."));
            connect(en, &QCheckBox::toggled, bcd_, &UsbBcd::setEnabled);
            connect(bcd_, &UsbBcd::enabledChanged, en, [en](bool on) {
                if (en->isChecked() != on) en->setChecked(on);
            });
            form->addRow(tr("USB-BCD"), en);

            auto *dev = new QComboBox(page);
            dev->addItem(tr("(none)"), QString());
            for (const QString &s : bcd_->devices()) {
                dev->addItem(s, s);
            }
            int di = dev->findData(bcd_->serial());
            dev->setCurrentIndex(di >= 0 ? di : 0);
            connect(dev, &QComboBox::currentIndexChanged, bcd_, [this, dev](int) {
                bcd_->setSerial(dev->currentData().toString());
            });
            form->addRow(tr("BCD cable"), dev);

            auto *sixty = new QCheckBox(
                tr("60 m uses the 40 m filter (BCD 3)"), page);
            sixty->setChecked(bcd_->sixtyAsForty());
            sixty->setToolTip(tr("60 m has no standard BCD code. On = use "
                                 "the 40 m code so the amp picks its 40 m "
                                 "filter; off = bypass."));
            connect(sixty, &QCheckBox::toggled, bcd_, &UsbBcd::setSixtyAsForty);
            connect(bcd_, &UsbBcd::sixtyAsFortyChanged, sixty, [sixty](bool on) {
                if (sixty->isChecked() != on) sixty->setChecked(on);
            });
            form->addRow(QString(), sixty);

            auto *code = new QLabel(page);
            auto setCode = [code](int c) {
                code->setText(tr("BCD code: %1").arg(c));
            };
            setCode(bcd_->currentBcd());
            code->setStyleSheet(QStringLiteral(
                "QLabel{color:#8fa6ba;font-family:Consolas;}"));
            connect(bcd_, &UsbBcd::currentBcdChanged, code, setCode);
            form->addRow(tr("BCD output"), code);
        }
    }

    // --- TX safety timeout + PA enable (TX-0c-pa-debug) ---
    // The operating-time TX controls (Drive %, MOX) live on the front-
    // facing TX dock (TxPanel.qml) — operator touches them between QSOs
    // and on every key.  This Settings group keeps the deliberate-arm
    // safety controls: the timeout config (set once, leave it) and PA
    // Enable (the "RF possible" gate, deliberately gated behind a trip
    // to Settings so a stray click on the front panel can't put RF on
    // the air).  Per the §15.20 spec: 1..20 min range, 10 min default,
    // bypass for long-form modes.  All persisted via QSettings inside
    // HL2Stream's setters.
    if (stream_) {
        auto *grp = new QGroupBox(tr("Transmit"), page);
        auto *g = new QGridLayout(grp);
        g->setColumnStretch(1, 1);

        g->addWidget(new QLabel(tr("TX safety timeout (min)"), grp), 0, 0);
        auto *toSpin = new QSpinBox(grp);
        toSpin->setRange(lyra::ipc::HL2Stream::kTxTimeoutMinSec / 60,
                         lyra::ipc::HL2Stream::kTxTimeoutMaxSec / 60);
        toSpin->setValue(stream_->txTimeoutSec() / 60);
        toSpin->setFixedWidth(80);
        toSpin->setToolTip(tr(
            "Auto-clears MOX if the radio stays keyed continuously past "
            "this many minutes.  Protects against a stuck PTT, an "
            "operator falling asleep, or a software bug latching MOX. "
            "1..20 min."));
        connect(toSpin, QOverload<int>::of(&QSpinBox::valueChanged), grp,
                [this](int min) {
            if (stream_) stream_->setTxTimeoutSec(min * 60);
        });
        connect(stream_, &lyra::ipc::HL2Stream::txTimeoutSecChanged, toSpin,
                [toSpin](int sec) {
            if (toSpin->value() != sec / 60) toSpin->setValue(sec / 60);
        });
        g->addWidget(toSpin, 0, 1, Qt::AlignLeft);

        auto *bypass = new QCheckBox(tr("Bypass safety timeout"), grp);
        bypass->setChecked(stream_->txTimeoutBypass());
        bypass->setToolTip(tr(
            "Disables the auto-release entirely.  Use for long-form modes "
            "(AM ragchew, slow CW beacon).  When OFF the safety timer "
            "re-arms with a fresh full duration on every keydown."));
        connect(bypass, &QCheckBox::toggled, grp, [this](bool on) {
            if (stream_) stream_->setTxTimeoutBypass(on);
        });
        connect(stream_, &lyra::ipc::HL2Stream::txTimeoutBypassChanged,
                bypass, [bypass](bool on) {
            if (bypass->isChecked() != on) bypass->setChecked(on);
        });
        g->addWidget(bypass, 1, 0, 1, 2);

        // Help label — sets expectations for the bench checklist.
        auto *help = new QLabel(grp);
        help->setText(tr(
            "<b>Safety:</b> the timeout clears MOX through the normal "
            "FSM keyup chain (ATT-on-TX restores, T/R relay drops, log "
            "line emitted).  Bypass + a stuck PTT = radio keyed until "
            "you intervene — only set Bypass when you know you need it."));
        help->setWordWrap(true);
        help->setStyleSheet(QStringLiteral("QLabel{color:#8fa6ba;}"));
        g->addWidget(help, 2, 0, 1, 2);

        // TX Drive % + MOX moved to the front-facing TX dock
        // (TxPanel.qml) — operator-tunable / per-key controls belong
        // on a real panel, not buried in a Settings dialog.  PA Enable
        // stays here as the deliberate-arm safety gate (a Settings trip
        // is the friction that keeps a stray front-panel click from
        // putting RF on the air).

        // --- PA enable (the first-RF gate) ---
        // Default-OFF on every stream open/close cycle (HL2Stream's
        // B-safety defensive clears enforce that); persisted across
        // clean Lyra exit-and-relaunch (tx/paEnabled).  The checkbox
        // reflects the live Stream.paEnabled value via paEnabledChanged
        // signal — so the post-open() safety clear immediately
        // unchecks the box in the UI without operator action.
        auto *paBox = new QCheckBox(tr("Enable PA (puts RF on the antenna)"), grp);
        paBox->setChecked(stream_->paEnabled());
        paBox->setToolTip(tr(
            "Sets the gateware PA-enable bit (C2 bit 3 of slot 10).  "
            "When checked AND MOX is keyed, the HL2 PA bias engages — "
            "PA current rises from ~0 to your idle-bias value (~0.2 A "
            "on a typical HL2+).  Combined with TX Drive > 0 % this "
            "puts real RF on the antenna.  Bench safety: use a dummy "
            "load + watt-meter for first key.  Defensively cleared on "
            "every stream stop/start (operator must re-check after a "
            "Stop/Open cycle)."));
        connect(paBox, &QCheckBox::toggled, grp, [this](bool on) {
            if (stream_) stream_->setPaEnabled(on);
        });
        connect(stream_, &lyra::ipc::HL2Stream::paEnabledChanged, paBox,
                [paBox](bool on) {
            if (paBox->isChecked() != on) paBox->setChecked(on);
        });
        g->addWidget(paBox, 3, 0, 1, 2);

        // Red warning label below the PA checkbox — sets the bench
        // expectation explicitly so an operator who just enables it
        // without reading the tooltip still sees the safety guidance.
        auto *paWarn = new QLabel(grp);
        paWarn->setText(tr(
            "<b style='color:#d11515;'>⚠ RF SAFETY:</b>  This puts the "
            "radio's PA on the air on the next key.  USE A DUMMY LOAD "
            "AND WATT-METER for the first session.  Phase-3-EXIT "
            "kill-test before any antenna: while keyed, taskkill /F "
            "lyra.exe and verify PA-current drops within a few seconds "
            "(HL2 gateware watchdog)."));
        paWarn->setWordWrap(true);
        paWarn->setStyleSheet(QStringLiteral("QLabel{color:#cccccc;}"));
        g->addWidget(paWarn, 4, 0, 1, 2);

        form->addRow(grp);
    }

    return page;
}

QWidget *SettingsDialog::buildMeterTab() {
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

    // S-meter calibration trim (calDb) — applied live + persisted by the
    // MeterModel.  The meter reads WDSP RXA_S_PK; this offset lands it on
    // an absolute dBm / S-unit scale.  With RXA_S_PK the trim is small.
    auto *cal = new QDoubleSpinBox(page);
    cal->setRange(-60.0, 60.0);
    cal->setDecimals(1);
    cal->setSingleStep(0.5);
    cal->setSuffix(tr(" dB"));
    cal->setValue(meter_ ? meter_->calDb() : 0.0);
    cal->setToolTip(tr(
        "S-meter calibration trim.\n\n"
        "The meter reads WDSP's in-passband RXA_S_PK level; this offset "
        "lands it on an absolute dBm / S-unit scale. Tune to a known-level "
        "reference — WWV, or a signal "
        "generator at a known dBm — and adjust until the S-reading "
        "matches. With RXA_S_PK the needed trim is only a few dB."));
    connect(cal, &QDoubleSpinBox::valueChanged, this,
            [this](double v) { if (meter_) meter_->setCalDb(v); });
    form->addRow(tr("S-meter calibration:"), cal);

    // Peak-hold dwell — how long the peak marker hangs at a new high
    // before it starts to fall.  Operator-tunable; applied live + persisted.
    auto *hold = new QSpinBox(page);
    hold->setRange(100, 5000);
    hold->setSingleStep(100);
    hold->setSuffix(tr(" ms"));
    hold->setValue(meter_ ? meter_->peakHoldMs() : 800);
    hold->setToolTip(tr(
        "Peak-hold time — how long the meter's peak marker hangs at a new "
        "high before decaying. Raise it to make brief peaks linger longer "
        "(easier to catch a fleeting signal / QSB crest); lower it for a "
        "snappier marker. Default 800 ms."));
    connect(hold, &QSpinBox::valueChanged, this,
            [this](int v) { if (meter_) meter_->setPeakHoldMs(v); });
    form->addRow(tr("Meter peak-hold:"), hold);

    // Max-hold "high-water mark" — a second, slower marker that latches
    // the highest level seen and eases down gently (distinct red marker
    // on the Arc / Bar).  Enable + its own dwell time.
    auto *maxOn = new QCheckBox(tr("Show max-peak marker"), page);
    maxOn->setChecked(meter_ ? meter_->maxPeakEnabled() : true);
    maxOn->setToolTip(tr(
        "A second meter marker (red) that latches the highest level "
        "reached and falls back gently — a 'high-water mark' that lets you "
        "read a fleeting DX or QSB crest long after the fast peak pip has "
        "dropped."));
    connect(maxOn, &QCheckBox::toggled, this,
            [this](bool on) { if (meter_) meter_->setMaxPeakEnabled(on); });
    form->addRow(QString(), maxOn);

    auto *maxHold = new QSpinBox(page);
    maxHold->setRange(1, 60);
    maxHold->setSingleStep(1);
    maxHold->setSuffix(tr(" s"));
    maxHold->setValue(meter_ ? meter_->maxHoldMs() / 1000 : 3);
    maxHold->setToolTip(tr(
        "How long the max-peak marker holds at a new high before it "
        "begins its gentle fall. Longer = the high-water mark lingers "
        "more before easing down. Default 3 s."));
    maxHold->setEnabled(maxOn->isChecked());
    connect(maxOn, &QCheckBox::toggled, maxHold, &QWidget::setEnabled);
    connect(maxHold, &QSpinBox::valueChanged, this,
            [this](int v) { if (meter_) meter_->setMaxHoldMs(v * 1000); });
    form->addRow(tr("Max-peak hold:"), maxHold);

    auto *hint = new QLabel(tr(
        "Set this once against a known signal. The meter compensates for "
        "the LNA gain automatically, so the reading stays put as you "
        "adjust LNA — you can calibrate at any setting."), page);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#8a9aac;"));
    form->addRow(hint);

    return page;
}

void SettingsDialog::selectTopic(const QString &topic) {
    // Map a panel help topic -> the Settings tab that owns it.  Only the
    // Visuals tab exists today; panadapter/visuals land there.  As Radio
    // / Audio / DSP tabs are added, extend this map (and find the tab by
    // its title so indices stay robust).
    static const QHash<QString, QString> kTabFor = {
        {QStringLiteral("panadapter"), QStringLiteral("Visuals")},
        {QStringLiteral("visuals"),    QStringLiteral("Visuals")},
        {QStringLiteral("display"),    QStringLiteral("Visuals")},
        {QStringLiteral("hardware"),   QStringLiteral("Hardware")},
        {QStringLiteral("radio"),      QStringLiteral("Hardware")},
        {QStringLiteral("audio"),      QStringLiteral("Audio")},
        {QStringLiteral("memory"),     QStringLiteral("Bands")},
        {QStringLiteral("bands"),      QStringLiteral("Bands")},
        // tuning has no dedicated tab yet -> falls through to the first
        // tab so the dialog still opens somewhere sensible.
    };
    const QString want = kTabFor.value(topic);
    if (!want.isEmpty()) {
        for (int i = 0; i < tabs_->count(); ++i) {
            if (tabs_->tabText(i) == want) {
                tabs_->setCurrentIndex(i);
                break;
            }
        }
    }
}

QWidget *SettingsDialog::buildVisualsTab() {
    // Three-column layout (task #22, the SECOND big-tab refactor after
    // Hardware).  Visuals had 26 controls / groups stacked vertically
    // — the longest tab in Settings.  Same minimal-surgery technique
    // as Hardware: three sibling QFormLayouts inside a horizontal
    // split, the local `form` pointer repointed twice so the existing
    // ~600 lines of `form->addRow(...)` call sites stay byte-identical.
    // No widget construction changes, no signal/slot re-wires.
    //
    // Layout (top-down, left → middle → right):
    //   LEFT   : Trace colour group, Waterfall palette group
    //   MIDDLE : Waterfall speed + dB (auto/floor/ceiling), Spectrum
    //            fill group, Peak markers group, Noise floor group,
    //            Trace smoothing, Peak glow, Glass sheen, Watermark
    //   RIGHT  : Meteors + Meteor frequency + Gold fireballs, Gridline
    //            brightness, Cursor readout, Frame rate, Spectrum dB
    //            (auto/floor/ceiling), Graphics backend group
    auto *page  = new QWidget(this);
    auto *outer = new QHBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(14);

    auto *leftCol  = new QWidget(page);
    auto *leftForm = new QFormLayout(leftCol);
    leftForm->setContentsMargins(0, 0, 0, 0);
    leftForm->setVerticalSpacing(10);
    outer->addWidget(leftCol, 1);

    auto *midCol  = new QWidget(page);
    auto *midForm = new QFormLayout(midCol);
    midForm->setContentsMargins(0, 0, 0, 0);
    midForm->setVerticalSpacing(10);
    outer->addWidget(midCol, 1);

    auto *rightCol  = new QWidget(page);
    auto *rightForm = new QFormLayout(rightCol);
    rightForm->setContentsMargins(0, 0, 0, 0);
    rightForm->setVerticalSpacing(10);
    outer->addWidget(rightCol, 1);

    // Body below addresses `form` extensively.  Start at left, switch
    // to middle just before "Waterfall speed", switch to right just
    // before "Meteor streaks".
    QFormLayout *form = leftForm;

    // --- Trace colour: Solid (picked colour) OR By-strength (palette) ---
    // Solid mirrors old Lyra (preset chips + custom chooser).  By-strength
    // is the intensity gradient old Lyra couldn't achieve: the trace
    // colour tracks signal level via a palette LUT.
    {
        auto *box = new QWidget(page);
        auto *bv = new QVBoxLayout(box);
        bv->setContentsMargins(0, 0, 0, 0);
        bv->setSpacing(4);

        // Mode selector.
        auto *mode = new QComboBox(box);
        mode->addItems({tr("Solid color"), tr("By signal strength")});
        mode->setCurrentIndex(prefs_->traceMode());
        bv->addWidget(mode);

        // --- By-strength: palette combo (presets + Custom ramp) ---
        auto *palCombo = new QComboBox(box);
        palCombo->addItems(lyra::palettes::names());
        const int customIdx = palCombo->count();   // index just past presets
        palCombo->addItem(tr("Custom color…"));
        palCombo->setCurrentIndex(prefs_->palette());
        connect(palCombo, &QComboBox::currentIndexChanged,
                prefs_, &Prefs::setPalette);
        connect(prefs_, &Prefs::paletteChanged, box, [this, palCombo]() {
            if (palCombo->currentIndex() != prefs_->palette())
                palCombo->setCurrentIndex(prefs_->palette());
        });
        bv->addWidget(palCombo);

        // Live gradient preview of the selected by-strength palette (or
        // the custom dark->hue->white ramp).  Weak signal at the left,
        // strong at the right — so the operator sees what they're picking
        // instead of choosing a palette blind.  Built from the SAME LUT /
        // ramp the panadapter draws with, so it's a faithful preview.
        auto *palPreview = new QLabel(box);
        palPreview->setFixedHeight(16);
        palPreview->setMinimumWidth(180);
        palPreview->setScaledContents(true);
        palPreview->setToolTip(tr("Weak signal (left) → strong (right)"));
        palPreview->setStyleSheet(QStringLiteral(
            "QLabel{border:1px solid #2a3a4a;border-radius:3px;}"));
        auto updatePreview = [this, palPreview, customIdx](int idx) {
            QImage img(256, 1, QImage::Format_RGB888);
            if (idx == customIdx) {
                const QColor base(prefs_->strengthColor());
                for (int i = 0; i < 256; ++i) {
                    const double t = i / 255.0;
                    int r, g, b;
                    if (t <= 0.65) {                    // black -> base hue
                        const double f = t / 0.65;
                        r = int(base.red()   * f);
                        g = int(base.green() * f);
                        b = int(base.blue()  * f);
                    } else {                            // base -> toward white
                        const double f = (t - 0.65) / 0.35;
                        r = int(base.red()   + (255 - base.red())   * f * 0.7);
                        g = int(base.green() + (255 - base.green()) * f * 0.7);
                        b = int(base.blue()  + (255 - base.blue())  * f * 0.7);
                    }
                    img.setPixelColor(i, 0, QColor(std::clamp(r, 0, 255),
                                                   std::clamp(g, 0, 255),
                                                   std::clamp(b, 0, 255)));
                }
            } else {
                const auto &l = lyra::palettes::lut(idx);
                for (int i = 0; i < 256; ++i)
                    img.setPixelColor(i, 0, QColor(l[i][0], l[i][1], l[i][2]));
            }
            palPreview->setPixmap(QPixmap::fromImage(img));
        };
        updatePreview(prefs_->palette());
        // Combo change (user OR programmatic sync) repaints the preview;
        // a custom-base-colour change does too when "Custom…" is active.
        connect(palCombo, &QComboBox::currentIndexChanged, box, updatePreview);
        connect(prefs_, &Prefs::strengthColorChanged, box,
                [palCombo, updatePreview]() {
                    updatePreview(palCombo->currentIndex());
                });
        bv->addWidget(palPreview);

        // Custom by-strength base colour (the dark->hue->bright ramp,
        // like Amber).  Shown only when "Custom color…" is selected.
        auto *strSwatch = new QPushButton(box);
        strSwatch->setFixedSize(64, 22);
        strSwatch->setToolTip(tr("Base colour for the custom by-strength "
                                 "ramp — click to choose"));
        auto setStr = [strSwatch](const QString &hex) {
            strSwatch->setStyleSheet(QStringLiteral(
                "QPushButton{background:%1;border:1px solid #2a3a4a;"
                "border-radius:3px;}").arg(hex));
        };
        setStr(prefs_->strengthColor());
        connect(strSwatch, &QPushButton::clicked, box, [this, box]() {
            const QColor c = QColorDialog::getColor(
                QColor(prefs_->strengthColor()), box,
                tr("By-strength base colour"));
            if (c.isValid()) prefs_->setStrengthColor(c.name());
        });
        connect(prefs_, &Prefs::strengthColorChanged, box,
                [this, setStr]() { setStr(prefs_->strengthColor()); });
        bv->addWidget(strSwatch);

        // --- Solid: swatch + custom + preset chips (sub-widget) ---
        auto *colorBox = new QWidget(box);
        auto *bvc = new QVBoxLayout(colorBox);
        bvc->setContentsMargins(0, 0, 0, 0);
        bvc->setSpacing(4);

        auto *swatch = new QPushButton(colorBox);
        swatch->setFixedSize(44, 22);
        swatch->setToolTip(tr("Current trace colour"));
        auto setSwatch = [swatch](const QString &hex) {
            swatch->setStyleSheet(QStringLiteral(
                "QPushButton{background:%1;border:1px solid #2a3a4a;"
                "border-radius:3px;}").arg(hex));
        };
        setSwatch(prefs_->traceColor());
        auto *customBtn = new QPushButton(tr("Custom color…"), colorBox);
        customBtn->setFixedWidth(140);

        auto *topRow = new QHBoxLayout();
        topRow->addWidget(swatch);
        topRow->addWidget(customBtn);
        topRow->addStretch(1);
        bvc->addLayout(topRow);

        // 18 preset chips, 3 rows of 6 (warm / cool / accents).
        static const char *kPresets[18] = {
            "#e53935", "#fb8c00", "#ffb300", "#fdd835", "#c0ca33", "#7cb342",
            "#26a69a", "#00acc1", "#039be5", "#1e88e5", "#3949ab", "#8e24aa",
            "#d81b60", "#ff7043", "#6d4c41", "#78909c", "#eceff1", "#ffffff" };
        auto *pg = new QGridLayout();
        pg->setHorizontalSpacing(2);
        pg->setVerticalSpacing(2);
        for (int i = 0; i < 18; ++i) {
            const QString hx = QString::fromLatin1(kPresets[i]);
            auto *chip = new QPushButton(colorBox);
            chip->setFixedSize(26, 18);
            chip->setCursor(Qt::PointingHandCursor);
            chip->setToolTip(hx);
            chip->setStyleSheet(QStringLiteral(
                "QPushButton{background:%1;border:1px solid #2a3a4a;"
                "border-radius:2px;}"
                "QPushButton:hover{border:1px solid #00e5ff;}").arg(hx));
            connect(chip, &QPushButton::clicked, prefs_,
                    [this, hx]() { prefs_->setTraceColor(hx); });
            pg->addWidget(chip, i / 6, i % 6);
        }
        bvc->addLayout(pg);
        bv->addWidget(colorBox);

        connect(customBtn, &QPushButton::clicked, colorBox, [this, colorBox]() {
            const QColor c = QColorDialog::getColor(
                QColor(prefs_->traceColor()), colorBox,
                tr("Spectrum trace colour"));
            if (c.isValid()) prefs_->setTraceColor(c.name());
        });
        connect(prefs_, &Prefs::traceColorChanged, colorBox,
                [this, setSwatch]() { setSwatch(prefs_->traceColor()); });

        // Mode visibility: Solid -> colour box; By-strength -> palette
        // combo (+ the custom base-colour swatch only when "Custom…").
        auto applyMode = [colorBox, palCombo, palPreview, strSwatch,
                          customIdx](int m) {
            const bool byStr = (m == 1);
            colorBox->setVisible(!byStr);
            palCombo->setVisible(byStr);
            palPreview->setVisible(byStr);
            strSwatch->setVisible(byStr && palCombo->currentIndex() == customIdx);
        };
        applyMode(prefs_->traceMode());
        connect(mode, &QComboBox::currentIndexChanged, this,
                [this, applyMode](int m) { prefs_->setTraceMode(m); applyMode(m); });
        connect(prefs_, &Prefs::traceModeChanged, box,
                [this, mode, applyMode]() {
            if (mode->currentIndex() != prefs_->traceMode())
                mode->setCurrentIndex(prefs_->traceMode());
            applyMode(prefs_->traceMode());
        });
        // Palette change can flip to/from Custom -> re-evaluate the swatch.
        connect(palCombo, &QComboBox::currentIndexChanged, box,
                [mode, applyMode](int) { applyMode(mode->currentIndex()); });

        form->addRow(tr("Trace color"), box);
    }

    // --- Waterfall palette (INDEPENDENT of the trace palette) ---
    // The operator can run, e.g., an Ocean trace over a Rainbow
    // waterfall.  New palettes added later become choices for both.
    {
        auto *wbox = new QWidget(page);
        auto *wv = new QVBoxLayout(wbox);
        wv->setContentsMargins(0, 0, 0, 0);
        wv->setSpacing(4);

        auto *wpal = new QComboBox(wbox);
        wpal->addItems(lyra::palettes::names());
        const int wCustomIdx = wpal->count();
        wpal->addItem(tr("Custom color…"));
        wpal->setCurrentIndex(prefs_->waterfallPalette());
        connect(wpal, &QComboBox::currentIndexChanged,
                prefs_, &Prefs::setWaterfallPalette);
        connect(prefs_, &Prefs::waterfallPaletteChanged, wbox, [this, wpal]() {
            if (wpal->currentIndex() != prefs_->waterfallPalette())
                wpal->setCurrentIndex(prefs_->waterfallPalette());
        });
        wv->addWidget(wpal);

        // Live gradient preview (same builder as the trace palette).
        auto *wprev = new QLabel(wbox);
        wprev->setFixedHeight(16);
        wprev->setMinimumWidth(180);
        wprev->setScaledContents(true);
        wprev->setToolTip(tr("Weak signal (left) → strong (right)"));
        wprev->setStyleSheet(QStringLiteral(
            "QLabel{border:1px solid #2a3a4a;border-radius:3px;}"));
        auto wUpdatePreview = [this, wprev, wCustomIdx](int idx) {
            QImage img(256, 1, QImage::Format_RGB888);
            if (idx == wCustomIdx) {
                const QColor base(prefs_->waterfallColor());
                for (int i = 0; i < 256; ++i) {
                    const double t = i / 255.0;
                    int r, g, b;
                    if (t <= 0.65) {
                        const double f = t / 0.65;
                        r = int(base.red() * f);
                        g = int(base.green() * f);
                        b = int(base.blue() * f);
                    } else {
                        const double f = (t - 0.65) / 0.35;
                        r = int(base.red()   + (255 - base.red())   * f * 0.7);
                        g = int(base.green() + (255 - base.green()) * f * 0.7);
                        b = int(base.blue()  + (255 - base.blue())  * f * 0.7);
                    }
                    img.setPixelColor(i, 0, QColor(std::clamp(r, 0, 255),
                                                   std::clamp(g, 0, 255),
                                                   std::clamp(b, 0, 255)));
                }
            } else {
                const auto &l = lyra::palettes::lut(idx);
                for (int i = 0; i < 256; ++i)
                    img.setPixelColor(i, 0, QColor(l[i][0], l[i][1], l[i][2]));
            }
            wprev->setPixmap(QPixmap::fromImage(img));
        };
        wUpdatePreview(prefs_->waterfallPalette());
        connect(wpal, &QComboBox::currentIndexChanged, wbox, wUpdatePreview);
        connect(prefs_, &Prefs::waterfallColorChanged, wbox,
                [wpal, wUpdatePreview]() { wUpdatePreview(wpal->currentIndex()); });
        wv->addWidget(wprev);

        // Custom-ramp base colour (shown only when "Custom color…").
        auto *wswatch = new QPushButton(wbox);
        wswatch->setFixedSize(64, 22);
        wswatch->setToolTip(tr("Base colour for the custom waterfall ramp"));
        auto wSetSwatch = [wswatch](const QString &hex) {
            wswatch->setStyleSheet(QStringLiteral(
                "QPushButton{background:%1;border:1px solid #2a3a4a;"
                "border-radius:3px;}").arg(hex));
        };
        wSetSwatch(prefs_->waterfallColor());
        connect(wswatch, &QPushButton::clicked, wbox, [this, wbox]() {
            const QColor c = QColorDialog::getColor(
                QColor(prefs_->waterfallColor()), wbox,
                tr("Waterfall base colour"));
            if (c.isValid()) prefs_->setWaterfallColor(c.name());
        });
        connect(prefs_, &Prefs::waterfallColorChanged, wbox,
                [this, wSetSwatch]() { wSetSwatch(prefs_->waterfallColor()); });
        wv->addWidget(wswatch);

        auto wApplyVis = [wswatch, wCustomIdx](int idx) {
            wswatch->setVisible(idx == wCustomIdx);
        };
        wApplyVis(prefs_->waterfallPalette());
        connect(wpal, &QComboBox::currentIndexChanged, wbox, wApplyVis);

        form->addRow(tr("Waterfall palette"), wbox);
    }

    // ---- Switch to the middle column (see scaffolding comment at the
    // top of this function). -----------------------------------------
    {
        auto *spacer = new QWidget(leftCol);
        spacer->setSizePolicy(QSizePolicy::Preferred,
                              QSizePolicy::Expanding);
        form->addRow(spacer);
    }
    form = midForm;

    // --- Waterfall speed (history rows per second) ---
    auto *wspeed = new QSpinBox(page);
    wspeed->setRange(1, 120);
    wspeed->setSuffix(tr(" rows/s"));
    wspeed->setValue(prefs_->waterfallSpeed());
    wspeed->setToolTip(tr("How fast the waterfall scrolls (decoupled from "
                          "the render frame rate). Between rows, incoming "
                          "frames are peak-held so brief signals still show "
                          "at slow speeds."));
    connect(wspeed, &QSpinBox::valueChanged, prefs_, &Prefs::setWaterfallSpeed);
    connect(prefs_, &Prefs::waterfallSpeedChanged, page, [this, wspeed]() {
        if (wspeed->value() != prefs_->waterfallSpeed())
            wspeed->setValue(prefs_->waterfallSpeed());
    });
    form->addRow(tr("Waterfall speed"), wspeed);

    // --- Waterfall dB range (INDEPENDENT of the panadapter scale) ---
    // Auto-fit OR a manual floor/ceiling.  The manual spinboxes gray
    // out while Auto is on.
    auto *wfAuto = new QCheckBox(tr("Auto-fit the waterfall dB range"), page);
    wfAuto->setChecked(prefs_->waterfallDbAuto());
    wfAuto->setToolTip(tr("Track the band automatically (noise floor − 15 dB "
                          "to peak + 15 dB). Off = use the floor/ceiling "
                          "below."));
    connect(wfAuto, &QCheckBox::toggled, prefs_, &Prefs::setWaterfallDbAuto);
    form->addRow(tr("Waterfall dB"), wfAuto);

    // The waterfall's own floor/ceiling — tune these to make detail
    // pop in the heat map without changing the spectrum scale.
    auto *wfMin = new QDoubleSpinBox(page);
    wfMin->setRange(-200.0, 0.0);
    wfMin->setSuffix(tr(" dB"));
    wfMin->setValue(prefs_->waterfallDbMin());
    wfMin->setToolTip(tr("Waterfall floor — levels at/below this map to the "
                         "darkest palette colour."));
    connect(wfMin, &QDoubleSpinBox::valueChanged,
            prefs_, &Prefs::setWaterfallDbMin);
    connect(prefs_, &Prefs::waterfallDbMinChanged, page, [this, wfMin]() {
        if (wfMin->value() != prefs_->waterfallDbMin())
            wfMin->setValue(prefs_->waterfallDbMin());
    });
    form->addRow(tr("Waterfall dB — floor"), wfMin);

    auto *wfMax = new QDoubleSpinBox(page);
    wfMax->setRange(-200.0, 20.0);
    wfMax->setSuffix(tr(" dB"));
    wfMax->setValue(prefs_->waterfallDbMax());
    wfMax->setToolTip(tr("Waterfall ceiling — levels at/above this map to the "
                         "brightest palette colour."));
    connect(wfMax, &QDoubleSpinBox::valueChanged,
            prefs_, &Prefs::setWaterfallDbMax);
    connect(prefs_, &Prefs::waterfallDbMaxChanged, page, [this, wfMax]() {
        if (wfMax->value() != prefs_->waterfallDbMax())
            wfMax->setValue(prefs_->waterfallDbMax());
    });
    form->addRow(tr("Waterfall dB — ceiling"), wfMax);

    // Gray the manual floor/ceiling out while Auto is on.
    auto applyWfAuto = [wfMin, wfMax](bool a) {
        wfMin->setEnabled(!a);
        wfMax->setEnabled(!a);
    };
    applyWfAuto(prefs_->waterfallDbAuto());
    connect(prefs_, &Prefs::waterfallDbAutoChanged, page,
            [this, wfAuto, applyWfAuto]() {
                const bool a = prefs_->waterfallDbAuto();
                if (wfAuto->isChecked() != a) wfAuto->setChecked(a);
                applyWfAuto(a);
            });

    // --- Spectrum fill on/off + fill colour ---
    // The fill colour applies in SOLID trace mode (lets the fill differ
    // from the trace line, old-Lyra parity); in by-strength mode the
    // fill follows the intensity palette.
    {
        auto *fbox = new QWidget(page);
        auto *fh = new QHBoxLayout(fbox);
        fh->setContentsMargins(0, 0, 0, 0);
        fh->setSpacing(8);

        auto *fill = new QCheckBox(tr("Show fill beneath the trace"), fbox);
        fill->setChecked(prefs_->fillEnabled());
        connect(fill, &QCheckBox::toggled, prefs_, &Prefs::setFillEnabled);
        connect(prefs_, &Prefs::fillEnabledChanged, fbox, [this, fill]() {
            if (fill->isChecked() != prefs_->fillEnabled())
                fill->setChecked(prefs_->fillEnabled());
        });
        fh->addWidget(fill);

        fh->addWidget(new QLabel(tr("Colour:"), fbox));
        auto *fswatch = new QPushButton(fbox);
        fswatch->setFixedSize(44, 22);
        fswatch->setToolTip(tr("Spectrum fill colour (solid trace mode)"));
        auto fSet = [fswatch](const QString &hex) {
            fswatch->setStyleSheet(QStringLiteral(
                "QPushButton{background:%1;border:1px solid #2a3a4a;"
                "border-radius:3px;}").arg(hex));
        };
        fSet(prefs_->fillColor());
        connect(fswatch, &QPushButton::clicked, fbox, [this, fbox]() {
            const QColor c = QColorDialog::getColor(
                QColor(prefs_->fillColor()), fbox, tr("Spectrum fill colour"));
            if (c.isValid()) prefs_->setFillColor(c.name());
        });
        connect(prefs_, &Prefs::fillColorChanged, fbox,
                [this, fSet]() { fSet(prefs_->fillColor()); });
        fh->addWidget(fswatch);
        fh->addStretch(1);

        form->addRow(tr("Spectrum fill"), fbox);
    }

    // --- Peak-hold markers (mode/hold-time + Clear live on the Display
    // panel; here are the look-and-feel knobs) ---
    {
        auto *pbox = new QWidget(page);
        auto *pv = new QVBoxLayout(pbox);
        pv->setContentsMargins(0, 0, 0, 0);
        pv->setSpacing(4);

        // Row 1: enable + style + show-dB + colour.
        auto *r1 = new QHBoxLayout();
        auto *pEn = new QCheckBox(tr("Show peak markers"), pbox);
        pEn->setChecked(prefs_->peakEnabled());
        connect(pEn, &QCheckBox::toggled, prefs_, &Prefs::setPeakEnabled);
        connect(prefs_, &Prefs::peakEnabledChanged, pbox, [this, pEn]() {
            if (pEn->isChecked() != prefs_->peakEnabled())
                pEn->setChecked(prefs_->peakEnabled());
        });
        r1->addWidget(pEn);

        r1->addWidget(new QLabel(tr("Style:"), pbox));
        auto *pStyle = new QComboBox(pbox);
        pStyle->addItems({tr("Line"), tr("Dots"), tr("Triangles")});
        pStyle->setCurrentIndex(prefs_->peakStyle());
        connect(pStyle, &QComboBox::currentIndexChanged,
                prefs_, &Prefs::setPeakStyle);
        connect(prefs_, &Prefs::peakStyleChanged, pbox, [this, pStyle]() {
            if (pStyle->currentIndex() != prefs_->peakStyle())
                pStyle->setCurrentIndex(prefs_->peakStyle());
        });
        r1->addWidget(pStyle);

        auto *pShow = new QCheckBox(tr("Show dB"), pbox);
        pShow->setChecked(prefs_->peakShowDb());
        connect(pShow, &QCheckBox::toggled, prefs_, &Prefs::setPeakShowDb);
        connect(prefs_, &Prefs::peakShowDbChanged, pbox, [this, pShow]() {
            if (pShow->isChecked() != prefs_->peakShowDb())
                pShow->setChecked(prefs_->peakShowDb());
        });
        r1->addWidget(pShow);

        r1->addWidget(new QLabel(tr("Colour:"), pbox));
        auto *pSwatch = new QPushButton(pbox);
        pSwatch->setFixedSize(44, 22);
        pSwatch->setToolTip(tr("Peak marker colour"));
        auto pSet = [pSwatch](const QString &hex) {
            pSwatch->setStyleSheet(QStringLiteral(
                "QPushButton{background:%1;border:1px solid #2a3a4a;"
                "border-radius:3px;}").arg(hex));
        };
        pSet(prefs_->peakColor());
        connect(pSwatch, &QPushButton::clicked, pbox, [this, pbox]() {
            const QColor c = QColorDialog::getColor(
                QColor(prefs_->peakColor()), pbox, tr("Peak marker colour"));
            if (c.isValid()) prefs_->setPeakColor(c.name());
        });
        connect(prefs_, &Prefs::peakColorChanged, pbox,
                [this, pSet]() { pSet(prefs_->peakColor()); });
        r1->addWidget(pSwatch);
        r1->addStretch(1);
        pv->addLayout(r1);

        // Row 2: decay rate (dB/s) — applies to the timed hold modes.
        auto *r2 = new QHBoxLayout();
        r2->addWidget(new QLabel(tr("Decay:"), pbox));
        auto *pDecay = new QSpinBox(pbox);
        pDecay->setRange(1, 120);
        pDecay->setSuffix(tr(" dB/s"));
        pDecay->setValue(static_cast<int>(prefs_->peakDecayDbps() + 0.5));
        pDecay->setToolTip(tr("How fast held peaks fall once past their "
                              "hold window (timed modes)."));
        connect(pDecay, &QSpinBox::valueChanged, prefs_,
                [this](int v) { prefs_->setPeakDecayDbps(v); });
        connect(prefs_, &Prefs::peakDecayDbpsChanged, pbox, [this, pDecay]() {
            const int v = static_cast<int>(prefs_->peakDecayDbps() + 0.5);
            if (pDecay->value() != v) pDecay->setValue(v);
        });
        r2->addWidget(pDecay);
        r2->addStretch(1);
        pv->addLayout(r2);

        form->addRow(tr("Peak markers"), pbox);
    }

    // --- Noise-floor reference line (dashed line at the rolling
    // ~20th-percentile floor + an "NF -NN dBFS" label) ---
    {
        auto *nbox = new QWidget(page);
        auto *nh = new QHBoxLayout(nbox);
        nh->setContentsMargins(0, 0, 0, 0);
        nh->setSpacing(8);

        auto *nfEn = new QCheckBox(tr("Show noise-floor line"), nbox);
        nfEn->setChecked(prefs_->noiseFloorEnabled());
        connect(nfEn, &QCheckBox::toggled, prefs_, &Prefs::setNoiseFloorEnabled);
        connect(prefs_, &Prefs::noiseFloorEnabledChanged, nbox, [this, nfEn]() {
            if (nfEn->isChecked() != prefs_->noiseFloorEnabled())
                nfEn->setChecked(prefs_->noiseFloorEnabled());
        });
        nh->addWidget(nfEn);

        nh->addWidget(new QLabel(tr("Colour:"), nbox));
        auto *nfSwatch = new QPushButton(nbox);
        nfSwatch->setFixedSize(44, 22);
        nfSwatch->setToolTip(tr("Noise-floor line colour"));
        auto nfSet = [nfSwatch](const QString &hex) {
            nfSwatch->setStyleSheet(QStringLiteral(
                "QPushButton{background:%1;border:1px solid #2a3a4a;"
                "border-radius:3px;}").arg(hex));
        };
        nfSet(prefs_->noiseFloorColor());
        connect(nfSwatch, &QPushButton::clicked, nbox, [this, nbox]() {
            const QColor c = QColorDialog::getColor(
                QColor(prefs_->noiseFloorColor()), nbox,
                tr("Noise-floor line colour"));
            if (c.isValid()) prefs_->setNoiseFloorColor(c.name());
        });
        connect(prefs_, &Prefs::noiseFloorColorChanged, nbox,
                [this, nfSet]() { nfSet(prefs_->noiseFloorColor()); });
        nh->addWidget(nfSwatch);
        nh->addStretch(1);

        form->addRow(tr("Noise floor"), nbox);
    }

    // --- Trace smoothing (temporal EWMA, 0 = off) ---
    auto *smooth = new QSpinBox(page);
    smooth->setRange(0, 10);
    smooth->setSpecialValueText(tr("Off"));   // shown at 0
    smooth->setValue(prefs_->smoothing());
    smooth->setToolTip(tr("Smooths the trace over time (higher = "
                          "smoother but slower response). 0 = off."));
    connect(smooth, &QSpinBox::valueChanged, prefs_, &Prefs::setSmoothing);
    connect(prefs_, &Prefs::smoothingChanged, page, [this, smooth]() {
        if (smooth->value() != prefs_->smoothing())
            smooth->setValue(prefs_->smoothing());
    });
    form->addRow(tr("Trace smoothing"), smooth);

    // --- Peak glow (GPU bloom around the bright trace, 0 = off) ---
    auto *glow = new QSpinBox(page);
    glow->setRange(0, 100);
    glow->setSuffix(tr(" %"));
    glow->setSpecialValueText(tr("Off"));   // shown at 0
    glow->setValue(prefs_->glow());
    glow->setToolTip(tr("Luminous bloom around the bright parts of the "
                        "trace. 0 = off."));
    connect(glow, &QSpinBox::valueChanged, prefs_, &Prefs::setGlow);
    connect(prefs_, &Prefs::glowChanged, page, [this, glow]() {
        if (glow->value() != prefs_->glow()) glow->setValue(prefs_->glow());
    });
    form->addRow(tr("Peak glow"), glow);

    // --- Glass sheen (diagonal reflection over the panel, 0 = off) ---
    auto *sheen = new QSpinBox(page);
    sheen->setRange(0, 100);
    sheen->setSuffix(tr(" %"));
    sheen->setSpecialValueText(tr("Off"));
    sheen->setValue(prefs_->glassSheen());
    sheen->setToolTip(tr("Subtle diagonal glass reflection over the "
                         "panadapter. 0 = off."));
    connect(sheen, &QSpinBox::valueChanged, prefs_, &Prefs::setGlassSheen);
    connect(prefs_, &Prefs::glassSheenChanged, page, [this, sheen]() {
        if (sheen->value() != prefs_->glassSheen())
            sheen->setValue(prefs_->glassSheen());
    });
    form->addRow(tr("Glass sheen"), sheen);

    // --- Lyra constellation watermark ---
    auto *wm = new QCheckBox(tr("Show Lyra constellation watermark"), page);
    wm->setChecked(prefs_->watermark());
    connect(wm, &QCheckBox::toggled, prefs_, &Prefs::setWatermark);
    connect(prefs_, &Prefs::watermarkChanged, page, [this, wm]() {
        if (wm->isChecked() != prefs_->watermark())
            wm->setChecked(prefs_->watermark());
    });
    form->addRow(tr("Watermark"), wm);

    // ---- Switch to the right column. -------------------------------
    {
        auto *spacer = new QWidget(midCol);
        spacer->setSizePolicy(QSizePolicy::Preferred,
                              QSizePolicy::Expanding);
        form->addRow(spacer);
    }
    form = rightForm;

    // --- Meteor streaks (rare ambient shooting stars) ---
    auto *met = new QCheckBox(tr("Show occasional meteor streaks"), page);
    met->setChecked(prefs_->meteors());
    connect(met, &QCheckBox::toggled, prefs_, &Prefs::setMeteors);
    connect(prefs_, &Prefs::meteorsChanged, page, [this, met]() {
        if (met->isChecked() != prefs_->meteors())
            met->setChecked(prefs_->meteors());
    });
    form->addRow(tr("Meteors"), met);

    // Average seconds between meteors (spread 0.5x..1.6x around this).
    auto *metGap = new QSpinBox(page);
    metGap->setRange(5, 120);
    metGap->setSingleStep(5);
    metGap->setSuffix(tr(" s"));
    metGap->setValue(prefs_->meteorGap());
    metGap->setToolTip(tr("Average gap between meteors; lower = more frequent."));
    connect(metGap, &QSpinBox::valueChanged, prefs_, &Prefs::setMeteorGap);
    connect(prefs_, &Prefs::meteorGapChanged, page, [this, metGap]() {
        if (metGap->value() != prefs_->meteorGap())
            metGap->setValue(prefs_->meteorGap());
    });
    form->addRow(tr("Meteor frequency"), metGap);

    // Percent chance a meteor is a warm-gold fireball (vs cool blue).
    auto *metGold = new QSpinBox(page);
    metGold->setRange(0, 100);
    metGold->setSuffix(tr(" %"));
    metGold->setValue(prefs_->meteorGold());
    metGold->setToolTip(tr("Chance a meteor is a warm-gold fireball; "
                           "0 = always cool blue."));
    connect(metGold, &QSpinBox::valueChanged, prefs_, &Prefs::setMeteorGold);
    connect(prefs_, &Prefs::meteorGoldChanged, page, [this, metGold]() {
        if (metGold->value() != prefs_->meteorGold())
            metGold->setValue(prefs_->meteorGold());
    });
    form->addRow(tr("Gold fireballs"), metGold);

    // --- Gridline brightness (0 = off, 100 = bright) ---
    auto *grid = new QSpinBox(page);
    grid->setRange(0, 100);
    grid->setSpecialValueText(tr("Off"));   // shown at 0
    grid->setValue(prefs_->gridLevel());
    connect(grid, &QSpinBox::valueChanged, prefs_, &Prefs::setGridLevel);
    connect(prefs_, &Prefs::gridLevelChanged, page, [this, grid]() {
        if (grid->value() != prefs_->gridLevel()) grid->setValue(prefs_->gridLevel());
    });
    form->addRow(tr("Gridline brightness"), grid);

    // --- Cursor frequency readout (floats near the pointer) ---
    auto *curRdt = new QCheckBox(
        tr("Show frequency readout at the cursor"), page);
    curRdt->setChecked(prefs_->cursorReadout());
    curRdt->setToolTip(tr("A small frequency label that follows the mouse "
                          "over the panadapter."));
    connect(curRdt, &QCheckBox::toggled, prefs_, &Prefs::setCursorReadout);
    connect(prefs_, &Prefs::cursorReadoutChanged, page, [this, curRdt]() {
        if (curRdt->isChecked() != prefs_->cursorReadout())
            curRdt->setChecked(prefs_->cursorReadout());
    });
    form->addRow(tr("Cursor readout"), curRdt);

    // --- Target frame rate (fps) ---
    auto *fps = new QSpinBox(page);
    fps->setRange(1, 240);
    fps->setSuffix(tr(" fps"));
    fps->setValue(prefs_->targetFps());
    connect(fps, &QSpinBox::valueChanged, prefs_, &Prefs::setTargetFps);
    connect(prefs_, &Prefs::targetFpsChanged, page, [this, fps]() {
        if (fps->value() != prefs_->targetFps()) fps->setValue(prefs_->targetFps());
    });
    form->addRow(tr("Frame rate"), fps);

    // --- dB display range (vertical scale) ---
    // Auto-fit OR a manual floor/ceiling.  The manual spinboxes gray
    // out while Auto is on (the panadapter auto-fits internally).
    auto *pdAuto = new QCheckBox(tr("Auto-fit the spectrum dB range"), page);
    pdAuto->setChecked(prefs_->dbAuto());
    pdAuto->setToolTip(tr("Track the band automatically (noise floor − 15 dB "
                          "to peak + 15 dB). Off = use the floor/ceiling "
                          "below. Dragging the right-edge dB scale also "
                          "turns this off."));
    connect(pdAuto, &QCheckBox::toggled, prefs_, &Prefs::setDbAuto);
    form->addRow(tr("Spectrum dB"), pdAuto);

    auto *dbMin = new QDoubleSpinBox(page);
    dbMin->setRange(-200.0, 0.0);
    dbMin->setSuffix(tr(" dB"));
    dbMin->setValue(prefs_->dbMin());
    connect(dbMin, &QDoubleSpinBox::valueChanged, prefs_, &Prefs::setDbMin);
    connect(prefs_, &Prefs::dbMinChanged, page, [this, dbMin]() {
        if (dbMin->value() != prefs_->dbMin()) dbMin->setValue(prefs_->dbMin());
    });
    form->addRow(tr("dB range — floor"), dbMin);

    auto *dbMax = new QDoubleSpinBox(page);
    dbMax->setRange(-200.0, 20.0);
    dbMax->setSuffix(tr(" dB"));
    dbMax->setValue(prefs_->dbMax());
    connect(dbMax, &QDoubleSpinBox::valueChanged, prefs_, &Prefs::setDbMax);
    connect(prefs_, &Prefs::dbMaxChanged, page, [this, dbMax]() {
        if (dbMax->value() != prefs_->dbMax()) dbMax->setValue(prefs_->dbMax());
    });
    form->addRow(tr("dB range — ceiling"), dbMax);

    // Gray the manual floor/ceiling out while Auto is on, and keep the
    // checkbox in sync if Auto is flipped elsewhere (e.g. a dB-edge drag
    // on the panadapter turns it off).
    auto applyPdAuto = [dbMin, dbMax](bool a) {
        dbMin->setEnabled(!a);
        dbMax->setEnabled(!a);
    };
    applyPdAuto(prefs_->dbAuto());
    connect(prefs_, &Prefs::dbAutoChanged, page,
            [this, pdAuto, applyPdAuto]() {
                const bool a = prefs_->dbAuto();
                if (pdAuto->isChecked() != a) pdAuto->setChecked(a);
                applyPdAuto(a);
            });

    // --- Graphics backend (advanced; restart to apply) ---
    // The Qt RHI backend is fixed at startup, so this writes a QSettings
    // key (read in main.cpp before the app launches) rather than applying
    // live.  "Auto" lets Qt pick per platform; explicit choices still
    // fall back transparently if unavailable.  Stored OUTSIDE Prefs since
    // it isn't a live display option.
    {
        auto *gbox = new QWidget(page);
        auto *gh = new QHBoxLayout(gbox);
        gh->setContentsMargins(0, 0, 0, 0);
        gh->setSpacing(8);

        auto *gfx = new QComboBox(gbox);
        gfx->addItem(tr("Auto (recommended)"), QStringLiteral("auto"));
        gfx->addItem(QStringLiteral("Vulkan"),        QStringLiteral("vulkan"));
        gfx->addItem(QStringLiteral("Direct3D 12"),   QStringLiteral("d3d12"));
        gfx->addItem(QStringLiteral("Direct3D 11"),   QStringLiteral("d3d11"));
        gfx->addItem(QStringLiteral("OpenGL"),        QStringLiteral("opengl"));
        const QString cur = QSettings()
            .value(QStringLiteral("ui/graphicsBackend"),
                   QStringLiteral("auto")).toString().toLower();
        const int idx = std::max(0, gfx->findData(cur));
        gfx->setCurrentIndex(idx);
        connect(gfx, &QComboBox::currentIndexChanged, gfx, [gfx](int i) {
            QSettings().setValue(QStringLiteral("ui/graphicsBackend"),
                                 gfx->itemData(i).toString());
        });
        gh->addWidget(gfx);

        auto *note = new QLabel(tr("Restart Lyra to apply"), gbox);
        note->setStyleSheet(QStringLiteral("QLabel{color:#8a9aac;}"));
        gh->addWidget(note);
        gh->addStretch(1);

        form->addRow(tr("Graphics backend"), gbox);
    }

    return page;
}

QWidget *SettingsDialog::buildNoiseTab() {
    // Captured noise-profile manager: list the saved profiles (one .lnp
    // file each) with their rate/FFT/date, and rename / delete (multi) /
    // open the folder.  Live capture + apply + tuning live on the
    // DSP+Audio panel; this tab is curation.
    auto *page = new QWidget(this);
    auto *outer = new QVBoxLayout(page);

    auto *grp = new QGroupBox(tr("Captured noise profiles"), page);
    auto *g = new QVBoxLayout(grp);

    auto *list = new QListWidget(grp);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    auto refresh = [this, list]() {
        list->clear();
        const QStringList names = engine_->noiseProfiles();
        for (const QString &n : names) {
            const QString info = engine_->noiseProfileInfo(n);
            auto *it = new QListWidgetItem(
                info.isEmpty() ? n : QStringLiteral("%1   —   %2").arg(n, info));
            it->setData(Qt::UserRole, n);
            list->addItem(it);
        }
    };
    refresh();
    connect(engine_, &lyra::dsp::WdspEngine::noiseProfilesChanged, list, refresh);
    g->addWidget(list);

    auto *btnRow = new QHBoxLayout;
    auto *renameBtn = new QPushButton(tr("Rename…"), grp);
    auto *delBtn    = new QPushButton(tr("Delete"), grp);
    auto *revealBtn = new QPushButton(tr("Open folder"), grp);
    btnRow->addWidget(renameBtn);
    btnRow->addWidget(delBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(revealBtn);
    g->addLayout(btnRow);

    auto selectionUpdate = [list, renameBtn, delBtn]() {
        const int n = list->selectedItems().size();
        renameBtn->setEnabled(n == 1);
        delBtn->setEnabled(n >= 1);
    };
    selectionUpdate();
    connect(list, &QListWidget::itemSelectionChanged, grp, selectionUpdate);
    connect(engine_, &lyra::dsp::WdspEngine::noiseProfilesChanged, grp, selectionUpdate);

    connect(renameBtn, &QPushButton::clicked, grp, [this, list]() {
        const auto items = list->selectedItems();
        if (items.size() != 1) return;
        const QString old = items.first()->data(Qt::UserRole).toString();
        bool ok = false;
        const QString nn = QInputDialog::getText(
            this, tr("Rename profile"), tr("New name:"),
            QLineEdit::Normal, old, &ok);
        if (ok && !nn.trimmed().isEmpty()) {
            if (!engine_->renameNoiseProfile(old, nn))
                QMessageBox::information(this, tr("Rename"),
                    tr("Couldn't rename — that name is already in use."));
        }
    });

    connect(delBtn, &QPushButton::clicked, grp, [this, list]() {
        const auto items = list->selectedItems();
        if (items.isEmpty()) return;
        const int n = items.size();
        if (QMessageBox::question(this, tr("Delete profiles"),
                tr("Delete %1 selected profile%2? This removes the file(s).")
                    .arg(n).arg(n == 1 ? QString() : QStringLiteral("s")))
            != QMessageBox::Yes)
            return;
        QStringList names;
        for (auto *it : items) names << it->data(Qt::UserRole).toString();
        for (const QString &nm : names) engine_->deleteNoiseProfile(nm);
    });

    connect(revealBtn, &QPushButton::clicked, grp, [this]() {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(engine_->noiseProfilesDir()));
    });

    outer->addWidget(grp);

    auto *path = new QLabel(
        tr("Profiles are stored as .lnp files in:\n%1")
            .arg(engine_->noiseProfilesDir()), page);
    path->setWordWrap(true);
    path->setStyleSheet(QStringLiteral("color:#8a9aac; font-size:11px;"));
    outer->addWidget(path);
    outer->addStretch(1);
    return page;
}

QWidget *SettingsDialog::buildWeatherTab() {
    // Two-column layout (task #23, continuation of the Hardware/Visuals/
    // Network refactor): the disclaimer + master-enable checkboxes stay
    // full-width at the top (they gate every other widget below), then a
    // horizontal split puts Sources + Notifications on the left and
    // Thresholds + API credentials on the right.  Same 28 px gutter +
    // 10 px form row spacing as the other refactored tabs.
    auto *page = new QWidget(this);
    auto *outer = new QVBoxLayout(page);
    auto apply = [this]() { if (wx_) wx_->reloadConfig(); };

    // Disclaimer + master enable (enable gated on accepting the disclaimer).
    auto *disc = new QCheckBox(
        tr("I understand these alerts are advisory only and not a "
           "substitute for official warnings."), page);
    disc->setChecked(QSettings().value(QStringLiteral("wx/disclaimer_accepted"),
                                       false).toBool());
    outer->addWidget(disc);

    auto *enable = new QCheckBox(tr("Enable weather alerts"), page);
    enable->setChecked(QSettings().value(QStringLiteral("wx/enabled"), false).toBool());
    enable->setEnabled(disc->isChecked());
    outer->addWidget(enable);

    // Two-column container below the gating checkboxes.
    auto *cols   = new QWidget(page);
    auto *colsHb = new QHBoxLayout(cols);
    colsHb->setContentsMargins(0, 0, 0, 0);
    colsHb->setSpacing(28);
    auto *leftCol  = new QWidget(cols);
    auto *leftVb   = new QVBoxLayout(leftCol);
    leftVb->setContentsMargins(0, 0, 0, 0);
    auto *rightCol = new QWidget(cols);
    auto *rightVb  = new QVBoxLayout(rightCol);
    rightVb->setContentsMargins(0, 0, 0, 0);
    colsHb->addWidget(leftCol,  1);
    colsHb->addWidget(rightCol, 1);
    outer->addWidget(cols);
    QBoxLayout *col = leftVb;   // pointer the four blocks below append to

    connect(disc, &QCheckBox::toggled, page, [enable, apply](bool on) {
        QSettings().setValue(QStringLiteral("wx/disclaimer_accepted"), on);
        enable->setEnabled(on);
        if (!on) enable->setChecked(false);
        apply();
    });
    connect(enable, &QCheckBox::toggled, page, [apply](bool on) {
        QSettings().setValue(QStringLiteral("wx/enabled"), on);
        apply();
    });

    // --- Sources ---
    {
        auto *grp = new QGroupBox(tr("Sources"), page);
        auto *v = new QVBoxLayout(grp);
        auto mkSrc = [v, page, apply](const QString &label,
                                      const QString &key, bool def) {
            auto *cb = new QCheckBox(label, page);
            cb->setChecked(QSettings().value(key, def).toBool());
            QObject::connect(cb, &QCheckBox::toggled, page,
                             [key, apply](bool on) {
                QSettings().setValue(key, on);
                apply();
            });
            v->addWidget(cb);
        };
        mkSrc(tr("Blitzortung — global lightning (free, no key)"),
              QStringLiteral("wx/src_blitzortung"), true);
        mkSrc(tr("NWS / weather.gov — wind + severe alerts (free)"),
              QStringLiteral("wx/src_nws"), true);
        mkSrc(tr("NWS METAR — live wind from a station (free)"),
              QStringLiteral("wx/src_nws_metar"), true);
        mkSrc(tr("Ambient Weather — your station (needs keys below)"),
              QStringLiteral("wx/src_ambient"), false);
        mkSrc(tr("Ecowitt — your station (needs keys below)"),
              QStringLiteral("wx/src_ecowitt"), false);
        col->addWidget(grp);
    }

    // --- Thresholds + station/units ---
    col = rightVb;   // switch to RIGHT column
    {
        auto *grp = new QGroupBox(tr("Thresholds"), page);
        auto *f = new QFormLayout(grp);
        f->setVerticalSpacing(10);

        // Distance unit drives the lightning-range display; range is
        // always STORED in km (wx/lightning_range_km), shown in mi/km.
        auto *unit = new QComboBox(grp);
        unit->addItem(tr("Miles"), QStringLiteral("mi"));
        unit->addItem(tr("Kilometres"), QStringLiteral("km"));
        unit->setCurrentIndex(std::max(0, unit->findData(QSettings()
            .value(QStringLiteral("wx/distance_unit"),
                   QStringLiteral("mi")).toString())));
        auto isKm = [unit]() {
            return unit->currentData().toString() == QStringLiteral("km");
        };

        auto *range = new QSpinBox(grp);
        range->setRange(3, 600);
        auto syncRange = [range, isKm]() {
            const double km = QSettings()
                .value(QStringLiteral("wx/lightning_range_km"), 80.0).toDouble();
            range->blockSignals(true);
            range->setSuffix(isKm() ? tr(" km") : tr(" mi"));
            range->setValue(qRound(isKm() ? km : km / 1.60934));
            range->blockSignals(false);
        };
        syncRange();
        connect(range, &QSpinBox::valueChanged, grp, [isKm, apply](int v) {
            const double km = isKm() ? double(v) : v * 1.60934;
            QSettings().setValue(QStringLiteral("wx/lightning_range_km"), km);
            apply();
        });
        f->addRow(tr("Lightning range"), range);

        auto *ws = new QSpinBox(grp);
        ws->setRange(5, 100);
        ws->setSuffix(tr(" mph"));
        ws->setValue(qRound(QSettings()
            .value(QStringLiteral("wx/wind_sustained_mph"), 30.0).toDouble()));
        connect(ws, &QSpinBox::valueChanged, grp, [apply](int v) {
            QSettings().setValue(QStringLiteral("wx/wind_sustained_mph"),
                                 double(v));
            apply();
        });
        f->addRow(tr("Wind sustained ≥"), ws);

        auto *wg = new QSpinBox(grp);
        wg->setRange(10, 150);
        wg->setSuffix(tr(" mph"));
        wg->setValue(qRound(QSettings()
            .value(QStringLiteral("wx/wind_gust_mph"), 40.0).toDouble()));
        connect(wg, &QSpinBox::valueChanged, grp, [apply](int v) {
            QSettings().setValue(QStringLiteral("wx/wind_gust_mph"), double(v));
            apply();
        });
        f->addRow(tr("Wind gust ≥"), wg);

        auto *metar = new QLineEdit(
            QSettings().value(QStringLiteral("wx/nws_metar_station")).toString(), grp);
        metar->setPlaceholderText(tr("ICAO, e.g. KTOL"));
        connect(metar, &QLineEdit::editingFinished, grp, [metar, apply]() {
            QSettings().setValue(QStringLiteral("wx/nws_metar_station"),
                                 metar->text().trimmed().toUpper());
            apply();
        });
        f->addRow(tr("METAR station"), metar);

        connect(unit, &QComboBox::currentIndexChanged, grp,
                [unit, apply, syncRange](int) {
            QSettings().setValue(QStringLiteral("wx/distance_unit"),
                                 unit->currentData().toString());
            syncRange();   // re-display the range in the newly-chosen unit
            apply();
        });
        f->addRow(tr("Distance unit"), unit);
        col->addWidget(grp);
    }

    // --- Notifications + test ---
    col = leftVb;   // back to LEFT column
    {
        auto *grp = new QGroupBox(tr("Notifications"), page);
        auto *v = new QVBoxLayout(grp);
        auto *toastCb = new QCheckBox(tr("Desktop notification on alert"), page);
        toastCb->setChecked(QSettings()
            .value(QStringLiteral("wx/desktop_enabled"), true).toBool());
        connect(toastCb, &QCheckBox::toggled, page, [apply](bool on) {
            QSettings().setValue(QStringLiteral("wx/desktop_enabled"), on);
            apply();
        });
        v->addWidget(toastCb);
        auto *audioCb = new QCheckBox(tr("Audible chime on alert"), page);
        audioCb->setChecked(QSettings()
            .value(QStringLiteral("wx/audio_enabled"), true).toBool());
        connect(audioCb, &QCheckBox::toggled, page, [apply](bool on) {
            QSettings().setValue(QStringLiteral("wx/audio_enabled"), on);
            apply();
        });
        v->addWidget(audioCb);
        auto *nwsWindCb = new QCheckBox(
            tr("Pop NWS High/Extreme Wind Warnings"), page);
        nwsWindCb->setChecked(QSettings()
            .value(QStringLiteral("wx/nws_wind_toast"), false).toBool());
        nwsWindCb->setToolTip(tr(
            "When on, an active NWS High/Extreme Wind Warning for your "
            "location pops a wind toast even if your local readings are "
            "below the sustained/gust thresholds above.  When off (default), "
            "the wind toast follows your thresholds only — the header badge "
            "still reflects the NWS warning either way."));
        connect(nwsWindCb, &QCheckBox::toggled, page, [apply](bool on) {
            QSettings().setValue(QStringLiteral("wx/nws_wind_toast"), on);
            apply();
        });
        v->addWidget(nwsWindCb);
        auto *test = new QPushButton(tr("Send test alert"), page);
        test->setToolTip(tr("Light the header badges + fire one toast for "
                            "a few seconds."));
        connect(test, &QPushButton::clicked, page,
                [this]() { if (wx_) wx_->fireTestSnapshot(); });
        v->addWidget(test);
        col->addWidget(grp);
        leftVb->addStretch(1);   // push left column groups to the top
    }

    // --- API credentials (Ambient / Ecowitt) ---
    col = rightVb;   // back to RIGHT column
    {
        auto *grp = new QGroupBox(tr("Station API credentials"), page);
        auto *f = new QFormLayout(grp);
        f->setVerticalSpacing(10);
        auto mkKey = [f, grp, apply](const QString &label, const QString &key) {
            auto *e = new QLineEdit(QSettings().value(key).toString(), grp);
            e->setEchoMode(QLineEdit::Password);
            QObject::connect(e, &QLineEdit::editingFinished, grp,
                             [e, key, apply]() {
                QSettings().setValue(key, e->text().trimmed());
                apply();
            });
            f->addRow(label, e);
        };
        mkKey(tr("Ambient API key"), QStringLiteral("wx/ambient_api_key"));
        mkKey(tr("Ambient Application key"), QStringLiteral("wx/ambient_app_key"));
        mkKey(tr("Ecowitt Application key"), QStringLiteral("wx/ecowitt_app_key"));
        mkKey(tr("Ecowitt API key"), QStringLiteral("wx/ecowitt_api_key"));
        auto *mac = new QLineEdit(
            QSettings().value(QStringLiteral("wx/ecowitt_mac")).toString(), grp);
        mac->setPlaceholderText(tr("e.g. 34:94:54:AB:CD:EF"));
        connect(mac, &QLineEdit::editingFinished, grp, [mac, apply]() {
            QSettings().setValue(QStringLiteral("wx/ecowitt_mac"),
                                 mac->text().trimmed().toUpper());
            apply();
        });
        f->addRow(tr("Ecowitt gateway MAC"), mac);
        col->addWidget(grp);
        rightVb->addStretch(1);   // push right column groups to the top
    }

    outer->addStretch(1);
    return page;
}

} // namespace lyra::ui
