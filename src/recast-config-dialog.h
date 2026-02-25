#pragma once

#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

extern "C" {
#include <obs.h>
#include "recast-output.h"
}

/* ---- Dynamic encoder property widget ---- */

class EncoderPropertyWidget : public QWidget {
	Q_OBJECT

public:
	explicit EncoderPropertyWidget(QWidget *parent = nullptr);
	~EncoderPropertyWidget();

	void setEncoder(const char *encoder_id);
	obs_data_t *getSettings() const;
	void loadSettings(obs_data_t *settings);

private:
	QVBoxLayout *layout_;
	QString encoder_id_;

	struct PropWidget {
		QString name;
		QWidget *widget;
		int type; /* obs_property_type */
	};
	std::vector<PropWidget> prop_widgets_;

	void clearWidgets();
};

/* ---- Full config dialog ---- */

class RecastConfigDialog : public QDialog {
	Q_OBJECT

public:
	explicit RecastConfigDialog(recast_output_target_t *target,
				    QWidget *parent = nullptr);
	~RecastConfigDialog();

signals:
	void settingsApplied(recast_output_target_t *target);

private slots:
	void onApply();
	void onEncoderChanged(int index);
	void onBrowseRecPath();

private:
	recast_output_target_t *target_;

	/* General tab */
	QLineEdit *name_edit_;
	QCheckBox *auto_start_check_;
	QCheckBox *auto_stop_check_;

	/* Streaming tab */
	QLineEdit *url_edit_;
	QLineEdit *key_edit_;
	QComboBox *encoder_combo_;
	QCheckBox *advanced_check_;
	QSpinBox *bitrate_spin_;
	QSpinBox *width_spin_;
	QSpinBox *height_spin_;
	QComboBox *audio_track_combo_;
	EncoderPropertyWidget *encoder_props_;

	/* Recording tab */
	QCheckBox *rec_enabled_check_;
	QLineEdit *rec_path_edit_;
	QComboBox *rec_format_combo_;
	QSpinBox *rec_bitrate_spin_;

	void populateEncoderList();
	void populateFromTarget();
};
