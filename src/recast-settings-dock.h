#pragma once

#include <QDockWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

extern "C" {
#include <obs.h>
#include "recast-output.h"
}

class RecastSettingsDock : public QDockWidget {
	Q_OBJECT

public:
	explicit RecastSettingsDock(recast_output_target_t *target,
				    QWidget *parent = nullptr);
	~RecastSettingsDock();

	void populateFromTarget();

signals:
	/* Emitted when the user clicks Apply */
	void settingsChanged(recast_output_target_t *target);

private slots:
	void onApply();

private:
	recast_output_target_t *target_;

	QLineEdit *name_edit_;
	QLineEdit *url_edit_;
	QLineEdit *key_edit_;
	QSpinBox *width_spin_;
	QSpinBox *height_spin_;
	QSpinBox *bitrate_spin_;
};
