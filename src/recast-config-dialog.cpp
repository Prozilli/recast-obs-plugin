/*
 * recast-config-dialog.cpp -- Full settings dialog for Recast outputs.
 *
 * Tabbed QDialog with General, Streaming, and Recording tabs.
 * Includes dynamic encoder property UI that regenerates when the
 * encoder selection changes.
 */

#include "recast-config-dialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

extern "C" {
#include <obs-module.h>
#include <obs-frontend-api.h>
#include "recast-protocol.h"
}

/* ====================================================================
 * EncoderPropertyWidget
 * ==================================================================== */

EncoderPropertyWidget::EncoderPropertyWidget(QWidget *parent)
	: QWidget(parent)
{
	layout_ = new QVBoxLayout(this);
	layout_->setContentsMargins(0, 0, 0, 0);
}

EncoderPropertyWidget::~EncoderPropertyWidget()
{
	clearWidgets();
}

void EncoderPropertyWidget::clearWidgets()
{
	for (auto &pw : prop_widgets_) {
		layout_->removeWidget(pw.widget);
		delete pw.widget;
	}
	prop_widgets_.clear();

	/* Remove any remaining children (labels, forms) */
	QLayoutItem *child;
	while ((child = layout_->takeAt(0)) != nullptr) {
		delete child->widget();
		delete child;
	}
}

void EncoderPropertyWidget::setEncoder(const char *encoder_id)
{
	clearWidgets();
	encoder_id_ = QString::fromUtf8(encoder_id ? encoder_id : "");

	if (encoder_id_.isEmpty())
		return;

	obs_properties_t *props =
		obs_get_encoder_properties(encoder_id);
	if (!props)
		return;

	auto *form = new QFormLayout;

	obs_property_t *prop = obs_properties_first(props);
	while (prop) {
		const char *name = obs_property_name(prop);
		const char *desc = obs_property_description(prop);
		enum obs_property_type type = obs_property_get_type(prop);

		PropWidget pw;
		pw.name = QString::fromUtf8(name);
		pw.type = type;
		pw.widget = nullptr;

		switch (type) {
		case OBS_PROPERTY_INT: {
			auto *spin = new QSpinBox;
			spin->setRange(obs_property_int_min(prop),
				       obs_property_int_max(prop));
			spin->setSingleStep(obs_property_int_step(prop));
			pw.widget = spin;
			break;
		}
		case OBS_PROPERTY_FLOAT: {
			auto *dspin = new QDoubleSpinBox;
			dspin->setRange(obs_property_float_min(prop),
					obs_property_float_max(prop));
			dspin->setSingleStep(obs_property_float_step(prop));
			pw.widget = dspin;
			break;
		}
		case OBS_PROPERTY_BOOL: {
			auto *check = new QCheckBox;
			pw.widget = check;
			break;
		}
		case OBS_PROPERTY_TEXT: {
			auto *edit = new QLineEdit;
			pw.widget = edit;
			break;
		}
		case OBS_PROPERTY_LIST: {
			auto *combo = new QComboBox;
			enum obs_combo_type combo_type =
				obs_property_list_type(prop);
			Q_UNUSED(combo_type);

			size_t count = obs_property_list_item_count(prop);
			for (size_t i = 0; i < count; i++) {
				const char *item_name =
					obs_property_list_item_name(prop, i);
				enum obs_combo_format fmt =
					obs_property_list_format(prop);

				if (fmt == OBS_COMBO_FORMAT_STRING) {
					const char *val =
						obs_property_list_item_string(
							prop, i);
					combo->addItem(
						QString::fromUtf8(item_name),
						QString::fromUtf8(
							val ? val : ""));
				} else if (fmt == OBS_COMBO_FORMAT_INT) {
					long long val =
						obs_property_list_item_int(
							prop, i);
					combo->addItem(
						QString::fromUtf8(item_name),
						(qlonglong)val);
				} else if (fmt == OBS_COMBO_FORMAT_FLOAT) {
					double val =
						obs_property_list_item_float(
							prop, i);
					combo->addItem(
						QString::fromUtf8(item_name),
						val);
				}
			}
			pw.widget = combo;
			break;
		}
		default:
			break;
		}

		if (pw.widget) {
			form->addRow(QString::fromUtf8(desc ? desc : name),
				     pw.widget);
			prop_widgets_.push_back(pw);
		}

		obs_property_next(&prop);
	}

	auto *form_widget = new QWidget;
	form_widget->setLayout(form);
	layout_->addWidget(form_widget);

	obs_properties_destroy(props);
}

obs_data_t *EncoderPropertyWidget::getSettings() const
{
	obs_data_t *settings = obs_data_create();

	for (const auto &pw : prop_widgets_) {
		QByteArray name_bytes = pw.name.toUtf8();
		const char *name = name_bytes.constData();

		switch (pw.type) {
		case OBS_PROPERTY_INT: {
			auto *spin = static_cast<QSpinBox *>(pw.widget);
			obs_data_set_int(settings, name, spin->value());
			break;
		}
		case OBS_PROPERTY_FLOAT: {
			auto *dspin =
				static_cast<QDoubleSpinBox *>(pw.widget);
			obs_data_set_double(settings, name, dspin->value());
			break;
		}
		case OBS_PROPERTY_BOOL: {
			auto *check = static_cast<QCheckBox *>(pw.widget);
			obs_data_set_bool(settings, name,
					  check->isChecked());
			break;
		}
		case OBS_PROPERTY_TEXT: {
			auto *edit = static_cast<QLineEdit *>(pw.widget);
			obs_data_set_string(settings, name,
					    edit->text().toUtf8().constData());
			break;
		}
		case OBS_PROPERTY_LIST: {
			auto *combo = static_cast<QComboBox *>(pw.widget);
			QVariant val = combo->currentData();
			if (val.typeId() == QMetaType::QString) {
				obs_data_set_string(
					settings, name,
					val.toString().toUtf8().constData());
			} else if (val.typeId() == QMetaType::LongLong) {
				obs_data_set_int(settings, name,
						 val.toLongLong());
			} else if (val.typeId() == QMetaType::Double) {
				obs_data_set_double(settings, name,
						    val.toDouble());
			}
			break;
		}
		default:
			break;
		}
	}

	return settings;
}

void EncoderPropertyWidget::loadSettings(obs_data_t *settings)
{
	if (!settings)
		return;

	for (auto &pw : prop_widgets_) {
		QByteArray name_bytes = pw.name.toUtf8();
		const char *name = name_bytes.constData();

		if (!obs_data_has_user_value(settings, name) &&
		    !obs_data_has_default_value(settings, name))
			continue;

		switch (pw.type) {
		case OBS_PROPERTY_INT: {
			auto *spin = static_cast<QSpinBox *>(pw.widget);
			spin->setValue(
				(int)obs_data_get_int(settings, name));
			break;
		}
		case OBS_PROPERTY_FLOAT: {
			auto *dspin =
				static_cast<QDoubleSpinBox *>(pw.widget);
			dspin->setValue(
				obs_data_get_double(settings, name));
			break;
		}
		case OBS_PROPERTY_BOOL: {
			auto *check = static_cast<QCheckBox *>(pw.widget);
			check->setChecked(
				obs_data_get_bool(settings, name));
			break;
		}
		case OBS_PROPERTY_TEXT: {
			auto *edit = static_cast<QLineEdit *>(pw.widget);
			edit->setText(QString::fromUtf8(
				obs_data_get_string(settings, name)));
			break;
		}
		case OBS_PROPERTY_LIST: {
			auto *combo = static_cast<QComboBox *>(pw.widget);
			const char *str_val =
				obs_data_get_string(settings, name);
			if (str_val && *str_val) {
				int idx = combo->findData(
					QString::fromUtf8(str_val));
				if (idx >= 0)
					combo->setCurrentIndex(idx);
			}
			break;
		}
		default:
			break;
		}
	}
}

/* ====================================================================
 * RecastConfigDialog
 * ==================================================================== */

RecastConfigDialog::RecastConfigDialog(recast_output_target_t *target,
				       QWidget *parent)
	: QDialog(parent), target_(target)
{
	setWindowTitle(
		QString("%1: %2")
			.arg(obs_module_text("Recast.Config.Title"))
			.arg(QString::fromUtf8(target->name)));
	setMinimumSize(500, 450);

	auto *root_layout = new QVBoxLayout(this);

	auto *tabs = new QTabWidget;

	/* ---- General Tab ---- */
	auto *general_tab = new QWidget;
	auto *general_form = new QFormLayout(general_tab);

	name_edit_ = new QLineEdit;
	general_form->addRow(obs_module_text("Recast.Name"), name_edit_);

	auto_start_check_ =
		new QCheckBox(obs_module_text("Recast.Config.AutoStart"));
	general_form->addRow("", auto_start_check_);

	auto_stop_check_ =
		new QCheckBox(obs_module_text("Recast.Config.AutoStop"));
	general_form->addRow("", auto_stop_check_);

	tabs->addTab(general_tab,
		     obs_module_text("Recast.Config.General"));

	/* ---- Streaming Tab ---- */
	auto *stream_tab = new QWidget;
	auto *stream_layout = new QVBoxLayout(stream_tab);

	auto *stream_form = new QFormLayout;

	url_edit_ = new QLineEdit;
	url_edit_->setPlaceholderText("rtmp://...");
	stream_form->addRow(obs_module_text("Recast.URL"), url_edit_);

	key_edit_ = new QLineEdit;
	key_edit_->setEchoMode(QLineEdit::Password);
	stream_form->addRow(obs_module_text("Recast.Key"), key_edit_);

	/* Resolution */
	width_spin_ = new QSpinBox;
	width_spin_->setRange(0, 7680);
	width_spin_->setSpecialValueText(obs_module_text("Recast.Auto"));
	stream_form->addRow(obs_module_text("Recast.Settings.Width"),
			    width_spin_);

	height_spin_ = new QSpinBox;
	height_spin_->setRange(0, 7680);
	height_spin_->setSpecialValueText(obs_module_text("Recast.Auto"));
	stream_form->addRow(obs_module_text("Recast.Settings.Height"),
			    height_spin_);

	/* Encoder selection */
	encoder_combo_ = new QComboBox;
	populateEncoderList();
	stream_form->addRow(obs_module_text("Recast.Config.Encoder"),
			    encoder_combo_);

	/* Advanced encoder toggle */
	advanced_check_ = new QCheckBox(
		obs_module_text("Recast.Config.AdvancedEncoder"));
	stream_form->addRow("", advanced_check_);

	/* Bitrate */
	bitrate_spin_ = new QSpinBox;
	bitrate_spin_->setRange(500, 50000);
	bitrate_spin_->setSuffix(" kbps");
	bitrate_spin_->setValue(4000);
	stream_form->addRow(obs_module_text("Recast.Settings.Bitrate"),
			    bitrate_spin_);

	/* Audio track */
	audio_track_combo_ = new QComboBox;
	for (int i = 0; i < 6; i++)
		audio_track_combo_->addItem(
			QString("Track %1").arg(i + 1), i);
	stream_form->addRow(obs_module_text("Recast.Config.AudioTrack"),
			    audio_track_combo_);

	stream_layout->addLayout(stream_form);

	/* Dynamic encoder properties */
	auto *props_group =
		new QGroupBox("Encoder Properties");
	auto *props_layout = new QVBoxLayout(props_group);
	auto *props_scroll = new QScrollArea;
	props_scroll->setWidgetResizable(true);

	encoder_props_ = new EncoderPropertyWidget;
	props_scroll->setWidget(encoder_props_);
	props_layout->addWidget(props_scroll);

	stream_layout->addWidget(props_group);

	/* Wire up encoder combo */
	connect(encoder_combo_,
		QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &RecastConfigDialog::onEncoderChanged);

	/* Enable/disable advanced encoder UI */
	connect(advanced_check_, &QCheckBox::toggled, this,
		[this](bool checked) {
			encoder_combo_->setEnabled(checked);
			bitrate_spin_->setEnabled(checked);
			encoder_props_->setVisible(checked);
		});

	tabs->addTab(stream_tab,
		     obs_module_text("Recast.Config.Streaming"));

	/* ---- Recording Tab ---- */
	auto *rec_tab = new QWidget;
	auto *rec_form = new QFormLayout(rec_tab);

	rec_enabled_check_ = new QCheckBox(
		obs_module_text("Recast.Config.RecEnabled"));
	rec_form->addRow("", rec_enabled_check_);

	auto *path_row = new QHBoxLayout;
	rec_path_edit_ = new QLineEdit;
	path_row->addWidget(rec_path_edit_, 1);
	auto *browse_btn =
		new QPushButton(obs_module_text("Recast.Config.Browse"));
	connect(browse_btn, &QPushButton::clicked,
		this, &RecastConfigDialog::onBrowseRecPath);
	path_row->addWidget(browse_btn);
	rec_form->addRow(obs_module_text("Recast.Config.RecPath"),
			 path_row);

	rec_format_combo_ = new QComboBox;
	rec_format_combo_->addItem("MKV", "mkv");
	rec_format_combo_->addItem("MP4", "mp4");
	rec_format_combo_->addItem("FLV", "flv");
	rec_form->addRow(obs_module_text("Recast.Config.RecFormat"),
			 rec_format_combo_);

	rec_bitrate_spin_ = new QSpinBox;
	rec_bitrate_spin_->setRange(500, 50000);
	rec_bitrate_spin_->setSuffix(" kbps");
	rec_bitrate_spin_->setValue(4000);
	rec_form->addRow(obs_module_text("Recast.Config.RecBitrate"),
			 rec_bitrate_spin_);

	tabs->addTab(rec_tab,
		     obs_module_text("Recast.Config.Recording"));

	root_layout->addWidget(tabs);

	/* Buttons */
	auto *buttons = new QDialogButtonBox(
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted,
		this, &RecastConfigDialog::onApply);
	connect(buttons, &QDialogButtonBox::rejected,
		this, &QDialog::reject);
	root_layout->addWidget(buttons);

	/* Populate from target */
	populateFromTarget();
}

RecastConfigDialog::~RecastConfigDialog() {}

void RecastConfigDialog::populateEncoderList()
{
	/* First entry: shared encoder */
	encoder_combo_->addItem(
		obs_module_text("Recast.Config.SharedEncoder"), "");

	/* Enumerate all video encoders */
	const char *enc_id;
	for (size_t i = 0; obs_enum_encoder_types(i, &enc_id); i++) {
		if (!enc_id || !*enc_id)
			continue;

		/* Only video encoders */
		if (obs_get_encoder_type(enc_id) != OBS_ENCODER_VIDEO)
			continue;

		/* Skip deprecated */
		uint32_t caps = obs_get_encoder_caps(enc_id);
		if (caps & OBS_ENCODER_CAP_DEPRECATED)
			continue;

		const char *display =
			obs_encoder_get_display_name(enc_id);

		encoder_combo_->addItem(
			QString::fromUtf8(display ? display : enc_id),
			QString::fromUtf8(enc_id));
	}
}

void RecastConfigDialog::populateFromTarget()
{
	if (!target_)
		return;

	name_edit_->setText(QString::fromUtf8(target_->name));
	auto_start_check_->setChecked(target_->auto_start);
	auto_stop_check_->setChecked(target_->auto_stop);

	url_edit_->setText(QString::fromUtf8(target_->url));
	key_edit_->setText(QString::fromUtf8(target_->key));

	width_spin_->setValue(target_->width);
	height_spin_->setValue(target_->height);

	advanced_check_->setChecked(target_->advanced_encoder);
	encoder_combo_->setEnabled(target_->advanced_encoder);
	bitrate_spin_->setEnabled(target_->advanced_encoder);
	encoder_props_->setVisible(target_->advanced_encoder);

	if (target_->custom_bitrate > 0)
		bitrate_spin_->setValue(target_->custom_bitrate);

	if (target_->encoder_id && *target_->encoder_id) {
		int idx = encoder_combo_->findData(
			QString::fromUtf8(target_->encoder_id));
		if (idx >= 0) {
			encoder_combo_->setCurrentIndex(idx);
			encoder_props_->setEncoder(target_->encoder_id);
			if (target_->encoder_settings)
				encoder_props_->loadSettings(
					target_->encoder_settings);
		}
	}

	audio_track_combo_->setCurrentIndex(target_->audio_track);

	/* Recording */
	rec_enabled_check_->setChecked(target_->rec_enabled);
	rec_path_edit_->setText(target_->rec_path
					? QString::fromUtf8(target_->rec_path)
					: "");

	if (target_->rec_format && *target_->rec_format) {
		int idx = rec_format_combo_->findData(
			QString::fromUtf8(target_->rec_format));
		if (idx >= 0)
			rec_format_combo_->setCurrentIndex(idx);
	}

	if (target_->rec_bitrate > 0)
		rec_bitrate_spin_->setValue(target_->rec_bitrate);
}

void RecastConfigDialog::onEncoderChanged(int index)
{
	QString enc_id = encoder_combo_->itemData(index).toString();
	if (enc_id.isEmpty()) {
		encoder_props_->setEncoder(nullptr);
	} else {
		encoder_props_->setEncoder(enc_id.toUtf8().constData());
	}
}

void RecastConfigDialog::onBrowseRecPath()
{
	QString dir = QFileDialog::getExistingDirectory(
		this, obs_module_text("Recast.Config.RecPath"),
		rec_path_edit_->text());
	if (!dir.isEmpty())
		rec_path_edit_->setText(dir);
}

void RecastConfigDialog::onApply()
{
	if (!target_)
		return;

	/* General */
	QString new_name = name_edit_->text().trimmed();
	if (!new_name.isEmpty()) {
		bfree(target_->name);
		target_->name = bstrdup(new_name.toUtf8().constData());
	}

	target_->auto_start = auto_start_check_->isChecked();
	target_->auto_stop = auto_stop_check_->isChecked();

	/* Streaming */
	QString new_url = url_edit_->text().trimmed();
	bfree(target_->url);
	target_->url = bstrdup(new_url.toUtf8().constData());

	QString new_key = key_edit_->text().trimmed();
	bfree(target_->key);
	target_->key = bstrdup(new_key.toUtf8().constData());

	target_->width = width_spin_->value();
	target_->height = height_spin_->value();

	/* Re-detect protocol */
	target_->protocol = recast_protocol_detect(target_->url);

	/* Update service */
	if (target_->service) {
		obs_data_t *svc_settings = obs_data_create();
		obs_data_set_string(svc_settings, "server", target_->url);
		if (target_->protocol == RECAST_PROTO_WHIP) {
			obs_data_set_string(svc_settings, "bearer_token",
					    target_->key);
		} else {
			obs_data_set_string(svc_settings, "key",
					    target_->key);
		}
		obs_service_update(target_->service, svc_settings);
		obs_data_release(svc_settings);
	}

	/* Encoding */
	target_->advanced_encoder = advanced_check_->isChecked();

	bfree(target_->encoder_id);
	QString enc_id = encoder_combo_->currentData().toString();
	target_->encoder_id =
		enc_id.isEmpty() ? NULL
				 : bstrdup(enc_id.toUtf8().constData());

	target_->custom_bitrate = bitrate_spin_->value();
	target_->audio_track = audio_track_combo_->currentIndex();

	/* Save encoder settings */
	if (target_->encoder_settings)
		obs_data_release(target_->encoder_settings);
	target_->encoder_settings = encoder_props_->getSettings();

	/* Recording */
	target_->rec_enabled = rec_enabled_check_->isChecked();

	bfree(target_->rec_path);
	QString rec_path = rec_path_edit_->text().trimmed();
	target_->rec_path = rec_path.isEmpty()
				    ? NULL
				    : bstrdup(rec_path.toUtf8().constData());

	bfree(target_->rec_format);
	target_->rec_format = bstrdup(
		rec_format_combo_->currentData().toString()
			.toUtf8().constData());

	target_->rec_bitrate = rec_bitrate_spin_->value();

	emit settingsApplied(target_);
	accept();
}
