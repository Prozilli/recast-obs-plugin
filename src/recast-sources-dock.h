#pragma once

#include <QDockWidget>
#include <QListWidget>
#include <QPushButton>

extern "C" {
#include <obs.h>
#include "recast-output.h"
}

class RecastSourcesDock : public QDockWidget {
	Q_OBJECT

public:
	explicit RecastSourcesDock(recast_output_target_t *target,
				    QWidget *parent = nullptr);
	~RecastSourcesDock();

public slots:
	/* Called when the scenes dock selects a new scene */
	void setCurrentScene(obs_scene_t *scene, obs_source_t *source);

signals:
	/* Emitted when sources are added/removed/reordered */
	void sourcesModified();

private slots:
	void onAddSource();
	void onRemoveSource();
	void onMoveUp();
	void onMoveDown();
	void onItemChanged(QListWidgetItem *item);
	void onItemDoubleClicked(QListWidgetItem *item);

private:
	recast_output_target_t *target_;
	obs_scene_t *current_scene_;
	QListWidget *list_;
	QPushButton *add_btn_;
	QPushButton *remove_btn_;
	QPushButton *up_btn_;
	QPushButton *down_btn_;

	void refreshItems();
	void addExistingSource();
	void createNewSource(const char *type_id, const char *label);
	void showTransformDialog(obs_sceneitem_t *item);
};
