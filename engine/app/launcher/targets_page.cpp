// app/launcher/targets_page.cpp -- the Targets page.
#include "targets_page.h"
#include <QScrollArea>
#include <QVBoxLayout>
#include "build_runner.h"
#include "details_sheet.h"
#include "installer.h"
#include "launcher_app.h"
#include "launcher_theme.h"
#include "page_header.h"
#include "progress_panel.h"
#include "segmented_control.h"
#include "target_card.h"
#include "title_manifest.h"
namespace ps2r::launcher {

TargetsPage::TargetsPage(LauncherApp* app, DetailsSheet* details, QWidget* parent) : QWidget(parent), app_(app), details_(details) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget();
    scroll->setWidget(inner);
    auto* col = new QVBoxLayout(inner);
    col->setContentsMargins(LauncherTheme::S5, 0, LauncherTheme::S5, LauncherTheme::S5);
    col->setSpacing(LauncherTheme::S3);
    col->addWidget(new PageHeader(QStringLiteral("Targets"), QStringLiteral("Build the game for another machine. This machine's build is the one Play uses."), inner));
    selector_ = new SegmentedControl(inner);
    selector_->setAccessibleName(QStringLiteral("Target"));
    QStringList labels, ids;
    for (const TargetSpec& t : app->manifest()->targets()) { labels << t.label; ids << t.id; }
    selector_->setItems(labels, ids);
    col->addWidget(selector_, 0, Qt::AlignLeft);
    for (const TargetSpec& t : app->manifest()->targets()) {
        auto* c = new TargetCard(app, t, inner);
        connect(c, &TargetCard::buildRequested, this, [this, id = t.id] { buildTarget(id); });
        connect(c, &TargetCard::detailsRequested, this, [this] { details_->showLogFile(QStringLiteral("Build details"), app_->builder()->currentLogPath()); });
        cards_ << c;
        col->addWidget(c);
    }
    col->addStretch(1);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
    connect(selector_, &SegmentedControl::valueChanged, this, [this](const QString&) { showSelected(); });
    connect(app_, &LauncherApp::stateChanged, this, [this] { for (TargetCard* c : cards_) c->refresh(); });
    connect(app_->builder(), &BuildRunner::progress, this, [this](const QString& text, int done, int total, qint64 elapsed, qint64 remaining) {
        TargetCard* c = card(app_->builder()->runningTarget());
        if (!c) return;
        c->refresh();
        c->progress()->setText(text);
        c->progress()->setProgress(done, total);
        QString cap = QStringLiteral("%1 elapsed").arg(BuildRunner::durationText(elapsed));
        if (remaining >= 0) cap += QStringLiteral(" · about %1 left").arg(BuildRunner::durationText(remaining));
        c->progress()->setCaption(cap);
    });
    connect(app_->builder(), &BuildRunner::finished, this, [this](const QString&, bool, const BuildRecord&) { for (TargetCard* c : cards_) c->refresh(); });
    connect(app_->installer(), &Installer::finished, this, [this](bool ok) {
        if (!pendingAfterInstall_.isEmpty()) {
            const QString id = pendingAfterInstall_;
            pendingAfterInstall_.clear();
            if (ok) app_->build(id);
        }
        for (TargetCard* c : cards_) c->refresh();
    });
    connect(app_->installer(), &Installer::progress, this, [this](const QString& text, int, int, bool) {
        if (pendingAfterInstall_.isEmpty()) return;
        if (TargetCard* c = card(pendingAfterInstall_)) { c->progress()->setVisible(true); c->progress()->setText(text); c->progress()->setIndeterminate(true); }
    });
    select(app_->hostTarget());
}

TargetCard* TargetsPage::card(const QString& targetId) const {
    for (TargetCard* c : cards_) if (c->spec().id == targetId) return c;
    return nullptr;
}

void TargetsPage::select(const QString& targetId) {
    selector_->setCurrentValue(targetId);
    showSelected();
}

void TargetsPage::showSelected() {
    const QString id = selector_->currentValue();
    for (TargetCard* c : cards_) c->setVisible(c->spec().id == id);
}

void TargetsPage::buildTarget(const QString& targetId) {
    const TargetSpec* spec = app_->manifest()->target(targetId);
    if (!spec || app_->builder()->running()) return;
    if (spec->needs.contains("vitasdk") && !app_->installer()->vitaSdkInstalled()) {
        pendingAfterInstall_ = targetId;
        app_->installer()->startVitaSdk();
        return;
    }
    app_->build(targetId);
}

}  // namespace ps2r::launcher
