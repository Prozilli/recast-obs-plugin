#pragma once

#include <QDockWidget>
#include <QListWidget>
#include <QPushButton>

extern "C" {
#include <obs.h>
#include "recast-output.h"
#include "recast-scene-model.h"
}

class RecastScenesDock : public QDockWidget {
	Q_OBJECT

public:
	explicit RecastScenesDock(recast_output_target_t *target,
				  QWidget *parent = nullptr);
	~RecastScenesDock();

	void refresh();

signals:
	/* Emitted when the user selects a different scene */
	void activeSceneChanged(obs_scene_t *scene, obs_source_t *source);

	/* Emitted when scenes are added/removed/renamed */
	void scenesModified();

private slots:
	void onAddScene();
	void onRemoveScene();
	void onRenameScene();
	void onSceneSelected(int row);
	void onContextMenu(const QPoint &pos);

private:
	recast_output_target_t *target_;
	QListWidget *list_;
	QPushButton *add_btn_;
	QPushButton *remove_btn_;
	QPushButton *rename_btn_;
};
