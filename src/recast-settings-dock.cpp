/*
 * recast-settings-dock.cpp -- Per-output settings dock widget.
 *
 * Provides editable fields for the output's name, RTMP URL, stream key,
 * resolution, and bitrate.
 */

#include "recast-settings-dock.h"

#include <QFormLayout>
#include <QVBoxLayout>

extern "C" {
#include <obs-module.h>
#include <util/platform.h>
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

	auto *form = new QFormLayout;

	/* Name */
	name_edit_ = new QLineEdit;
	form->addRow(obs_module_text("Recast.Name"), name_edit_);

	/* RTMP URL */
	url_edit_ = new QLineEdit;
	url_edit_->setPlaceholderText("rtmp://...");
	form->addRow(obs_module_text("Recast.URL"), url_edit_);

	/* Stream Key */
	key_edit_ = new QLineEdit;
	key_edit_->setEchoMode(QLineEdit::Password);
	form->addRow(obs_module_text("Recast.Key"), key_edit_);

	/* Resolution */
	width_spin_ = new QSpinBox;
	width_spin_->setRange(0, 7680);
	width_spin_->setSpecialValueText(obs_module_text("Recast.Auto"));
	form->addRow(obs_module_text("Recast.Settings.Width"), width_spin_);

	height_spin_ = new QSpinBox;
	height_spin_->setRange(0, 7680);
	height_spin_->setSpecialValueText(obs_module_text("Recast.Auto"));
	form->addRow(obs_module_text("Recast.Settings.Height"), height_spin_);

	/* Bitrate */
	bitrate_spin_ = new QSpinBox;
	bitrate_spin_->setRange(500, 50000);
	bitrate_spin_->setSuffix(" kbps");
	bitrate_spin_->setValue(4000);
	form->addRow(obs_module_text("Recast.Settings.Bitrate"),
		     bitrate_spin_);

	layout->addLayout(form);

	/* Apply button */
	auto *apply_btn =
		new QPushButton(obs_module_text("Recast.Settings.Apply"));
	connect(apply_btn, &QPushButton::clicked,
		this, &RecastSettingsDock::onApply);
	layout->addWidget(apply_btn);

	layout->addStretch();

	setWidget(container);

	populateFromTarget();
}

RecastSettingsDock::~RecastSettingsDock() {}

void RecastSettingsDock::populateFromTarget()
{
	if (!target_)
		return;

	name_edit_->setText(QString::fromUtf8(target_->name));
	url_edit_->setText(QString::fromUtf8(target_->url));
	key_edit_->setText(QString::fromUtf8(target_->key));
	width_spin_->setValue(target_->width);
	height_spin_->setValue(target_->height);
}

void RecastSettingsDock::onApply()
{
	if (!target_)
		return;

	/* Update name */
	QString new_name = name_edit_->text().trimmed();
	if (!new_name.isEmpty()) {
		bfree(target_->name);
		target_->name = bstrdup(new_name.toUtf8().constData());
	}

	/* Update URL */
	QString new_url = url_edit_->text().trimmed();
	bfree(target_->url);
	target_->url = bstrdup(new_url.toUtf8().constData());

	/* Update key */
	QString new_key = key_edit_->text().trimmed();
	bfree(target_->key);
	target_->key = bstrdup(new_key.toUtf8().constData());

	/* Update resolution */
	bool resolution_changed =
		(target_->width != width_spin_->value()) ||
		(target_->height != height_spin_->value());

	target_->width = width_spin_->value();
	target_->height = height_spin_->value();

	/* Update service settings */
	if (target_->service) {
		obs_data_t *svc_settings = obs_data_create();
		obs_data_set_string(svc_settings, "url",
				    target_->url);
		obs_data_set_string(svc_settings, "key",
				    target_->key);
		obs_service_update(target_->service, svc_settings);
		obs_data_release(svc_settings);
	}

	/* Update dock title */
	QString title = QString("%1: %2")
				.arg(obs_module_text("Recast.Dock.Settings"))
				.arg(new_name);
	setWindowTitle(title);

	emit settingsChanged(target_);

	Q_UNUSED(resolution_changed);
}
