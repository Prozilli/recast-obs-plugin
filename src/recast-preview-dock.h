#pragma once

#include <QDockWidget>

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

public slots:
	void updateScene(obs_source_t *source);
	void updateResolution(int w, int h);

private:
	recast_output_target_t *target_;
	RecastPreviewWidget *preview_;
	int canvas_w_;
	int canvas_h_;
};
