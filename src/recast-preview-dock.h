#pragma once

#include <QDockWidget>
#include <QPushButton>

extern "C" {
#include <obs.h>
#include "recast-output.h"
}

class RecastPreviewWidget;

class RecastPreviewDock : public QDockWidget {
	Q_OBJECT

public:
	explicit RecastPreviewDock(recast_output_target_t *target,
				   QWidget *parent = nullptr);
	~RecastPreviewDock();

	/* Access the inner preview widget for signal wiring */
	RecastPreviewWidget *previewWidget() const { return preview_; }

public slots:
	void updateScene(obs_source_t *source);
	void updateResolution(int w, int h);
	void updateInteractiveScene(obs_scene_t *scene, obs_source_t *source);

private slots:
	void onToggleRecord();
	void onToggleVirtualCam();

private:
	recast_output_target_t *target_;
	RecastPreviewWidget *preview_;
	QPushButton *rec_btn_;
	QPushButton *vcam_btn_;
	int canvas_w_;
	int canvas_h_;

	void refreshButtons();
};
