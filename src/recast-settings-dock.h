#pragma once

#include <QDockWidget>
#include <QLabel>
#include <QPushButton>

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
	/* Emitted when the user applies settings */
	void settingsChanged(recast_output_target_t *target);

private slots:
	void onOpenSettings();

private:
	recast_output_target_t *target_;
	QLabel *summary_label_;

	void updateSummary();
};
