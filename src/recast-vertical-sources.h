#pragma once

#include <QPushButton>
#include <QWidget>

extern "C" {
#include <obs.h>
}

class QMenu;

class SourceTree;

/*
 * RecastVerticalSourcesDock -- Always-present sources dock for vertical canvas.
 *
 * Wraps existing SourceTree with bottom toolbar [+] [-] [gear] [^] [v].
 * Matches native OBS Sources panel look.
 */

class RecastVerticalSourcesDock : public QWidget {
	Q_OBJECT

public:
	explicit RecastVerticalSourcesDock(QWidget *parent = nullptr);
	~RecastVerticalSourcesDock();

public slots:
	void setCurrentScene(int idx);
	void selectSceneItem(obs_sceneitem_t *item);

signals:
	void sourcesModified();
	void itemSelected(obs_sceneitem_t *item);

private slots:
	void onAddSource();
	void onRemoveSource();
	void onProperties();
	void onMoveUp();
	void onMoveDown();
	void onContextMenu(const QPoint &pos);

private:
	obs_scene_t *current_scene_ = nullptr;
	SourceTree *tree_;
	QPushButton *add_btn_;
	QPushButton *remove_btn_;
	QPushButton *props_btn_;
	QPushButton *up_btn_;
	QPushButton *down_btn_;

	void buildAddSourceMenu(QMenu *menu);
	void buildTransformMenu(QMenu *menu, obs_sceneitem_t *item);
	void createNewSource(const char *type_id, const char *display_name);
	void showTransformDialog(obs_sceneitem_t *item);
};
