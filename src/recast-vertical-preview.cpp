/*
 * recast-vertical-preview.cpp -- Always-present 9:16 preview dock.
 *
 * Shows the live vertical canvas with interactive scene editing.
 */

#include "recast-vertical-preview.h"
#include "recast-preview-widget.h"
#include "recast-vertical.h"

#include <QVBoxLayout>

extern "C" {
#include <obs-module.h>
}

RecastVerticalPreviewDock::RecastVerticalPreviewDock(QWidget *parent)
	: QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(2, 2, 2, 2);

	preview_ = new RecastPreviewWidget;
	layout->addWidget(preview_);

	/* Get initial canvas size from vertical controller */
	RecastVertical *v = RecastVertical::instance();
	canvas_w_ = v->canvasWidth();
	canvas_h_ = v->canvasHeight();

	refreshScene();
}

RecastVerticalPreviewDock::~RecastVerticalPreviewDock()
{
	preview_->ClearScene();
}

void RecastVerticalPreviewDock::onActiveSceneChanged(int idx)
{
	Q_UNUSED(idx);
	refreshScene();
}

void RecastVerticalPreviewDock::onCanvasSizeChanged(int w, int h)
{
	canvas_w_ = w;
	canvas_h_ = h;
	refreshScene();
}

void RecastVerticalPreviewDock::refreshScene()
{
	RecastVertical *v = RecastVertical::instance();
	recast_scene_model_t *model = v->sceneModel();

	obs_source_t *src = model
		? recast_scene_model_get_active_source(model)
		: nullptr;
	obs_scene_t *scene = model
		? recast_scene_model_get_active_scene(model)
		: nullptr;

	preview_->SetScene(src, canvas_w_, canvas_h_);
	preview_->SetInteractiveScene(scene);
}
