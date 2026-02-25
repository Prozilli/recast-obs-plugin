/*
 * recast-preview-dock.cpp -- Per-output preview dock widget.
 *
 * Wraps the existing RecastPreviewWidget in a QDockWidget, showing
 * the live scene rendering at the target's resolution.
 */

#include "recast-preview-dock.h"
#include "recast-dock.h" /* for RecastPreviewWidget */

#include <QVBoxLayout>

extern "C" {
#include <obs-module.h>
}

RecastPreviewDock::RecastPreviewDock(recast_output_target_t *target,
				     QWidget *parent)
	: QDockWidget(parent), target_(target), canvas_w_(0), canvas_h_(0)
{
	QString title = QString("%1: %2")
				.arg(obs_module_text("Recast.Dock.Preview"))
				.arg(QString::fromUtf8(target->name));
	setWindowTitle(title);
	setObjectName(QString("RecastPreview_%1").arg(
		QString::fromUtf8(target->id)));

	setFeatures(QDockWidget::DockWidgetMovable |
		    QDockWidget::DockWidgetFloatable |
		    QDockWidget::DockWidgetClosable);

	auto *container = new QWidget;
	auto *layout = new QVBoxLayout(container);
	layout->setContentsMargins(0, 0, 0, 0);

	preview_ = new RecastPreviewWidget;
	layout->addWidget(preview_);

	setWidget(container);

	/* Set initial resolution */
	canvas_w_ = target->width > 0 ? target->width : 1920;
	canvas_h_ = target->height > 0 ? target->height : 1080;

	/* Show active scene if available */
	if (target->use_private_scenes && target->scene_model) {
		obs_source_t *src = recast_scene_model_get_active_source(
			target->scene_model);
		if (src)
			preview_->SetScene(src, canvas_w_, canvas_h_);
	}
}

RecastPreviewDock::~RecastPreviewDock()
{
	preview_->ClearScene();
}

void RecastPreviewDock::updateScene(obs_source_t *source)
{
	preview_->SetScene(source, canvas_w_, canvas_h_);
}

void RecastPreviewDock::updateResolution(int w, int h)
{
	canvas_w_ = w > 0 ? w : canvas_w_;
	canvas_h_ = h > 0 ? h : canvas_h_;

	/* Re-set the scene with the new resolution */
	if (target_->use_private_scenes && target_->scene_model) {
		obs_source_t *src = recast_scene_model_get_active_source(
			target_->scene_model);
		if (src)
			preview_->SetScene(src, canvas_w_, canvas_h_);
	}
}
