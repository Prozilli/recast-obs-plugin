/*
 * recast-preview-dock.cpp -- Per-output preview dock widget.
 *
 * Wraps the existing RecastPreviewWidget in a QDockWidget, showing
 * the live scene rendering at the target's resolution with interactive
 * drag/resize support. Includes record and virtual camera buttons.
 */

#include "recast-preview-dock.h"
#include "recast-dock.h" /* for RecastPreviewWidget */

#include <QHBoxLayout>
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

	/* Toolbar: Record + Virtual Camera */
	auto *toolbar = new QHBoxLayout;
	toolbar->setContentsMargins(4, 2, 4, 2);

	rec_btn_ = new QPushButton("REC");
	rec_btn_->setFixedWidth(60);
	rec_btn_->setCheckable(true);
	rec_btn_->setToolTip("Start/Stop Recording");
	connect(rec_btn_, &QPushButton::clicked,
		this, &RecastPreviewDock::onToggleRecord);
	toolbar->addWidget(rec_btn_);

	vcam_btn_ = new QPushButton("VCam");
	vcam_btn_->setFixedWidth(60);
	vcam_btn_->setCheckable(true);
	vcam_btn_->setToolTip("Start/Stop Virtual Camera");
	connect(vcam_btn_, &QPushButton::clicked,
		this, &RecastPreviewDock::onToggleVirtualCam);
	toolbar->addWidget(vcam_btn_);

	toolbar->addStretch();

	layout->addLayout(toolbar);

	setWidget(container);

	/* Set initial resolution */
	canvas_w_ = target->width > 0 ? target->width : 1920;
	canvas_h_ = target->height > 0 ? target->height : 1080;

	/* Show active scene if available */
	if (target->use_private_scenes && target->scene_model) {
		obs_source_t *src = recast_scene_model_get_active_source(
			target->scene_model);
		obs_scene_t *scene = recast_scene_model_get_active_scene(
			target->scene_model);
		if (src)
			preview_->SetScene(src, canvas_w_, canvas_h_);
		if (scene)
			preview_->SetInteractiveScene(scene);
	}

	refreshButtons();
}

RecastPreviewDock::~RecastPreviewDock()
{
	preview_->ClearScene();
}

void RecastPreviewDock::updateScene(obs_source_t *source)
{
	preview_->SetScene(source, canvas_w_, canvas_h_);
}

void RecastPreviewDock::updateInteractiveScene(obs_scene_t *scene,
					       obs_source_t *source)
{
	Q_UNUSED(source);
	preview_->SetInteractiveScene(scene);
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

void RecastPreviewDock::refreshButtons()
{
	rec_btn_->setChecked(target_->rec_active);
	rec_btn_->setStyleSheet(target_->rec_active
					? "QPushButton { background: #d32f2f; "
					  "color: #fff; font-weight: bold; }"
					: "");

	vcam_btn_->setChecked(target_->virtualcam_active);
	vcam_btn_->setStyleSheet(
		target_->virtualcam_active
			? "QPushButton { background: #1976d2; "
			  "color: #fff; font-weight: bold; }"
			: "");
}

void RecastPreviewDock::onToggleRecord()
{
	if (target_->rec_active) {
		recast_output_target_stop_recording(target_);
	} else {
		recast_output_target_start_recording(target_);
	}
	refreshButtons();
}

void RecastPreviewDock::onToggleVirtualCam()
{
	if (target_->virtualcam_active) {
		recast_output_target_stop_virtualcam(target_);
	} else {
		recast_output_target_start_virtualcam(target_);
	}
	refreshButtons();
}
