#pragma once

#include <QDockWidget>
#include <QPushButton>

extern "C" {
#include <obs.h>
#include "recast-output.h"
}

class SourceTree;

class RecastSourcesDock : public QDockWidget {
	Q_OBJECT

public:
	explicit RecastSourcesDock(recast_output_target_t *target,
				    QWidget *parent = nullptr);
	~RecastSourcesDock();

public slots:
	/* Called when the scenes dock selects a new scene */
	void setCurrentScene(obs_scene_t *scene, obs_source_t *source);

	/* Called when the preview selects an item by clicking */
	void selectSceneItem(obs_sceneitem_t *item);

signals:
	/* Emitted when sources are added/removed/reordered */
	void sourcesModified();

	/* Emitted when user selects an item in the list */
	void itemSelected(obs_sceneitem_t *item);

private slots:
	void onAddSource();
	void onRemoveSource();
	void onMoveUp();
	void onMoveDown();
	void onContextMenu(const QPoint &pos);

private:
	recast_output_target_t *target_;
	obs_scene_t *current_scene_;
	SourceTree *tree_;
	QPushButton *add_btn_;
	QPushButton *remove_btn_;
	QPushButton *up_btn_;
	QPushButton *down_btn_;

	void buildAddSourceMenu(QMenu *menu);
	void createNewSource(const char *type_id, const char *display_name);
	void showTransformDialog(obs_sceneitem_t *item);
};
