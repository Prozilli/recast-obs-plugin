#pragma once

#include <QDockWidget>
#include <QListWidget>
#include <QPushButton>

/*
 * RecastVerticalScenesDock -- Always-present scenes dock for vertical canvas.
 *
 * QListWidget + bottom toolbar [+] [-], right-click context menu
 * (Rename, Link to Main Scene, Remove). Matches native OBS Scenes panel look.
 */

class RecastVerticalScenesDock : public QDockWidget {
	Q_OBJECT

public:
	explicit RecastVerticalScenesDock(QWidget *parent = nullptr);
	~RecastVerticalScenesDock();

public slots:
	void refresh();

signals:
	void sceneActivated(int idx);
	void scenesModified();

private slots:
	void onAddScene();
	void onRemoveScene();
	void onSceneSelected(int row);
	void onContextMenu(const QPoint &pos);

private:
	QListWidget *list_;
	QPushButton *add_btn_;
	QPushButton *remove_btn_;
};
