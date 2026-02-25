/*
 * recast-settings-dock.cpp -- Per-output settings dock widget.
 *
 * Shows a summary of current settings and a button to open the
 * full RecastConfigDialog.
 */

#include "recast-settings-dock.h"
#include "recast-config-dialog.h"

#include <QVBoxLayout>

extern "C" {
#include <obs-module.h>
#include <util/platform.h>
#include "recast-protocol.h"
}

RecastSettingsDock::RecastSettingsDock(recast_output_target_t *target,
				       QWidget *parent)
	: QDockWidget(parent), target_(target)
{
	QString title = QString("%1: %2")
				.arg(obs_module_text("Recast.Dock.Settings"))
				.arg(QString::fromUtf8(target->name));
	setWindowTitle(title);
	setObjectName(QString("RecastSettings_%1").arg(
		QString::fromUtf8(target->id)));

	setFeatures(QDockWidget::DockWidgetMovable |
		    QDockWidget::DockWidgetFloatable |
		    QDockWidget::DockWidgetClosable);

	auto *container = new QWidget;
	auto *layout = new QVBoxLayout(container);
	layout->setContentsMargins(4, 4, 4, 4);

	/* Summary label */
	summary_label_ = new QLabel;
	summary_label_->setWordWrap(true);
	summary_label_->setStyleSheet("padding: 4px;");
	layout->addWidget(summary_label_);

	/* Settings button */
	auto *settings_btn = new QPushButton(
		obs_module_text("Recast.Config.Title") +
		QString("..."));
	connect(settings_btn, &QPushButton::clicked,
		this, &RecastSettingsDock::onOpenSettings);
	layout->addWidget(settings_btn);

	layout->addStretch();

	setWidget(container);

	updateSummary();
}

RecastSettingsDock::~RecastSettingsDock() {}

void RecastSettingsDock::populateFromTarget()
{
	updateSummary();
}

void RecastSettingsDock::updateSummary()
{
	if (!target_) {
		summary_label_->setText("No target");
		return;
	}

	QString summary;

	/* Protocol */
	summary += QString("Protocol: %1\n")
			   .arg(QString::fromUtf8(
				   recast_protocol_name(target_->protocol)));

	/* Encoder info */
	if (target_->advanced_encoder) {
		QString enc_name = target_->encoder_id
					   ? QString::fromUtf8(
						     target_->encoder_id)
					   : "obs_x264";
		int br = target_->custom_bitrate > 0
				 ? target_->custom_bitrate
				 : 4000;
		summary += QString("Encoder: %1 @ %2 kbps\n")
				   .arg(enc_name)
				   .arg(br);
	} else {
		summary += "Encoder: Shared (zero overhead)\n";
	}

	/* Resolution */
	if (target_->width > 0 && target_->height > 0) {
		summary += QString("Resolution: %1x%2\n")
				   .arg(target_->width)
				   .arg(target_->height);
	} else {
		summary += "Resolution: Main canvas\n";
	}

	/* Audio track */
	summary += QString("Audio: Track %1\n")
			   .arg(target_->audio_track + 1);

	/* Auto */
	if (target_->auto_start)
		summary += "Auto-start: Yes\n";
	if (target_->auto_stop)
		summary += "Auto-stop: Yes\n";

	/* Recording */
	if (target_->rec_enabled)
		summary += "Recording: Enabled\n";

	summary_label_->setText(summary.trimmed());
}

void RecastSettingsDock::onOpenSettings()
{
	auto *dlg = new RecastConfigDialog(target_, this);
	connect(dlg, &RecastConfigDialog::settingsApplied, this,
		[this](recast_output_target_t *t) {
			updateSummary();

			/* Update dock title */
			QString title =
				QString("%1: %2")
					.arg(obs_module_text(
						"Recast.Dock.Settings"))
					.arg(QString::fromUtf8(t->name));
			setWindowTitle(title);

			emit settingsChanged(t);
		});
	dlg->exec();
	dlg->deleteLater();
}
