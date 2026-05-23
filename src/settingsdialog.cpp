// Lyra — Settings dialog.  See settingsdialog.h.

#include "settingsdialog.h"

#include "bands.h"
#include "hl2_discovery.h"
#include "hl2_stream.h"
#include "palettes.h"
#include "prefs.h"
#include "usb_bcd.h"
#include "wdsp_engine.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QListWidget>
#include <QListWidgetItem>
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
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace lyra::ui {

SettingsDialog::SettingsDialog(Prefs *prefs, lyra::ipc::HL2Stream *stream,
                               lyra::ipc::HL2Discovery *discovery,
                               UsbBcd *bcd, lyra::dsp::WdspEngine *engine,
                               QWidget *parent)
    : QDialog(parent), prefs_(prefs), stream_(stream),
      discovery_(discovery), bcd_(bcd), engine_(engine) {
    setWindowTitle(tr("Lyra — Settings"));
    resize(640, 520);

    tabs_ = new QTabWidget(this);
    tabs_->addTab(buildVisualsTab(), tr("Visuals"));
    if (engine_) {
        tabs_->addTab(buildAudioTab(), tr("Audio"));
    }
    if (stream_ || discovery_ || bcd_) {
        tabs_->addTab(buildHardwareTab(), tr("Hardware"));
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

QWidget *SettingsDialog::buildHardwareTab() {
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

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
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);

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
                   QStringLiteral("vulkan")).toString().toLower();
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

} // namespace lyra::ui
